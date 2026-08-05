/**
 * @file wifi_captive_portal.c
 * @brief WiFi Captive Portal Implementation
 *
 * Implements an open AP with DNS redirect and HTTP server for WiFi
 * configuration when no valid credentials are available.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "wifi_captive_portal.h"

#if CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "core/memory/memory_manager_ng.h"

#include "wifi_manager.h"
#include "wifi_config.h"
#include "captive_portal_html.h"
#include "utils/version.h"

static const char *TAG = "CAPTIVE_PORTAL";

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Scan results HTML buffer size (PSRAM, ~270 bytes per network × 20) */
#define CAPTIVE_PORTAL_SCAN_BUF_SIZE    6144

/* ============================================================================
 * State
 * ============================================================================ */

static captive_portal_state_t s_state = CAPTIVE_PORTAL_INACTIVE;
static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;
static StackType_t *s_dns_stack = NULL;  /**< DNS task stack in PSRAM (freed on stop) */
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_portal_events = NULL;
static captive_portal_subsystem_cb_t s_subsystem_cb = NULL;

/** @brief Event bits */
#define PORTAL_SUCCESS_BIT  BIT0
#define PORTAL_TIMEOUT_BIT  BIT1

/** @brief Connection test state */
static char s_test_ssid[WIFI_SSID_MAX_LEN];
static char s_test_password[WIFI_PASSWORD_MAX_LEN];
static char s_connected_ip[WIFI_IP_STRING_LEN];
static volatile bool s_test_success = false;
static volatile bool s_test_in_progress = false;
static char s_test_error[64];

/* ============================================================================
 * HTML Escape Helper
 * ============================================================================ */

/**
 * @brief HTML-escape a string to prevent XSS
 *
 * Escapes &, <, >, ", ' characters. Output is written to dst.
 *
 * @param dst   Output buffer
 * @param dst_size  Output buffer size
 * @param src   Input string (untrusted)
 * @return Number of bytes written (excluding NUL)
 */
static int html_escape(char *dst, size_t dst_size, const char *src)
{
    int written = 0;
    while (*src && written < (int)(dst_size - 6)) { /* 6 = max escape length */
        switch (*src) {
            case '&':
                written += snprintf(dst + written, dst_size - written, "&amp;");
                break;
            case '<':
                written += snprintf(dst + written, dst_size - written, "&lt;");
                break;
            case '>':
                written += snprintf(dst + written, dst_size - written, "&gt;");
                break;
            case '"':
                written += snprintf(dst + written, dst_size - written, "&quot;");
                break;
            case '\'':
                written += snprintf(dst + written, dst_size - written, "&#39;");
                break;
            default:
                dst[written++] = *src;
                break;
        }
        src++;
    }
    dst[written] = '\0';
    return written;
}

/* ============================================================================
 * DNS Redirect Server
 * ============================================================================ */

/**
 * @brief DNS header structure (simplified)
 */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/**
 * @brief DNS redirect task - answers all queries with AP IP
 *
 * Uses task notification for clean shutdown instead of blind delay.
 */
static void dns_redirect_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Set receive timeout so we can check for stop notification */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS redirect server started on port 53");

    /* AP IP in network byte order: 192.168.4.1 */
    uint8_t ap_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t client_len;

    /* Run until someone actually asks us to stop.
     *
     * This used to be `s_state == ACTIVE || s_state == TESTING`, but
     * start_dns_redirect() is called while the state is still STARTING —
     * ACTIVE is only set afterwards. The task therefore evaluated the
     * condition as false on its very first iteration and exited immediately,
     * roughly 6ms after logging "DNS redirect server started". The portal AP
     * came up but nothing redirected DNS, so phones never auto-opened the
     * config page and the address had to be typed in by hand.
     *
     * Phrasing it as a stop condition makes the task independent of the
     * startup ordering. */
    while (s_state != CAPTIVE_PORTAL_STOPPING && s_state != CAPTIVE_PORTAL_INACTIVE) {
        /* Check for stop notification (non-blocking)
         * v6.0 compatible: xTaskNotifyWait replaces deprecated ulTaskNotifyTake */
        uint32_t notification_value;
        if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, 0) == pdTRUE) {
            break;
        }

        client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&client_addr, &client_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        /* Build response: copy query, set response flags, add answer */
        uint8_t resp[512];
        if (len > 460) {
            continue; /* Too large, skip */
        }
        memcpy(resp, buf, len);

        dns_header_t *resp_hdr = (dns_header_t *)resp;
        resp_hdr->flags = htons(0x8180); /* Standard response, no error */
        resp_hdr->ancount = htons(1);    /* One answer record appended */

        /* Append answer record after the query section */
        int resp_len = len;
        /* Answer: pointer to name in query (0xC00C), type A, class IN, TTL 60, rdlength 4, IP */
        uint8_t answer[] = {
            0xC0, 0x0C,             /* Name pointer to offset 12 (first question) */
            0x00, 0x01,             /* Type A */
            0x00, 0x01,             /* Class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60 seconds */
            0x00, 0x04,             /* RDLENGTH 4 */
            ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]
        };
        memcpy(resp + resp_len, answer, sizeof(answer));
        resp_len += sizeof(answer);

        sendto(sock, resp, resp_len, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS redirect server stopped");
    /* Notify stop function that we're done */
    xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ============================================================================
 * WiFi Scan Helpers
 * ============================================================================ */

/**
 * @brief Generate HTML for scan results with XSS-safe SSID escaping
 *
 * @param buf Output buffer (should be in PSRAM)
 * @param buf_size Buffer size
 * @return Length written
 */
static int generate_scan_html(char *buf, size_t buf_size)
{
    wifi_ap_record_t *ap_list = mem_ng_calloc(CAPTIVE_PORTAL_MAX_SCAN_APS,
                                               sizeof(wifi_ap_record_t),
                                               MEM_CAP_PSRAM);
    if (!ap_list) {
        return snprintf(buf, buf_size, "<p class='scan-hint'>Memory error</p>");
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        mem_ng_free(ap_list);
        return snprintf(buf, buf_size,
                        "<p class='scan-hint'>Scan failed: %s</p>",
                        esp_err_to_name(ret));
    }

    uint16_t ap_count = CAPTIVE_PORTAL_MAX_SCAN_APS;
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    if (ap_count == 0) {
        mem_ng_free(ap_list);
        /* Say that an empty list is not proof of an empty airwave.
         *
         * Measured on this hardware: a scan-only build with no connect
         * attempts, passive, 200ms dwell, ten rounds over five minutes,
         * reported zero access points every single time — not one beacon, not
         * even a neighbour — while the same board associates at -43 dBm and
         * stays up for hours. Scanning is effectively blind here.
         *
         * "No networks found" therefore reads as "your network is gone" when
         * the truth is "this list cannot be trusted, type the name". The form
         * below already takes a hand-entered SSID, so this is only about not
         * sending the user off to debug their router. */
        return snprintf(buf, buf_size,
                        "<p class='scan-hint'>No networks found. This list is "
                        "unreliable on this hardware and can be empty even when "
                        "your network is fine &mdash; type the name below.</p>");
    }

    /* Escaped SSID buffers: max 32 chars × 6 (worst case &#39;) + NUL */
    char ssid_html[200];
    char ssid_attr[200];

    int written = 0;
    for (int i = 0; i < ap_count && written < (int)(buf_size - 512); i++) {
        /* Skip our own AP */
        if (strncmp((char *)ap_list[i].ssid, CAPTIVE_PORTAL_AP_SSID_PREFIX,
                    strlen(CAPTIVE_PORTAL_AP_SSID_PREFIX)) == 0) {
            continue;
        }

        /* Skip empty SSIDs */
        if (strlen((char *)ap_list[i].ssid) == 0) {
            continue;
        }

        /* Escape SSID for HTML display and JS attribute */
        html_escape(ssid_html, sizeof(ssid_html), (char *)ap_list[i].ssid);
        html_escape(ssid_attr, sizeof(ssid_attr), (char *)ap_list[i].ssid);

        /* Signal strength class */
        const char *sig_class = "mid";
        int bars = 2;
        if (ap_list[i].rssi > -50) {
            sig_class = "";
            bars = 4;
        } else if (ap_list[i].rssi > -65) {
            sig_class = "";
            bars = 3;
        } else if (ap_list[i].rssi > -80) {
            sig_class = "mid";
            bars = 2;
        } else {
            sig_class = "weak";
            bars = 1;
        }

        /* Lock icon for encrypted networks */
        bool encrypted = (ap_list[i].authmode != WIFI_AUTH_OPEN);

        written += snprintf(buf + written, buf_size - written,
            "<div class='net %s' onclick=\"selectNet('%s')\">"
            "<span class='ssid'>%s</span>"
            "<span class='info'>%s"
            "<span class='bars'>",
            sig_class, ssid_attr, ssid_html,
            encrypted ? "<span class='lock'></span>" : "");

        for (int b = 1; b <= 4; b++) {
            written += snprintf(buf + written, buf_size - written,
                "<span class='b%d'%s></span>",
                b, b > bars ? " style='opacity:.2'" : "");
        }

        written += snprintf(buf + written, buf_size - written,
            "</span>%ddBm</span></div>", ap_list[i].rssi);
    }

    mem_ng_free(ap_list);
    return written;
}

/* ============================================================================
 * Connection Test Task
 * ============================================================================ */

/**
 * @brief Background task for WiFi connection testing
 *
 * Runs the connection test asynchronously so the HTTP server stays responsive
 * for /status polling and other requests.
 */
static void connect_test_task(void *arg)
{
    ESP_LOGI(TAG, "Testing connection to: %s", s_test_ssid);

    /* Disconnect current STA and try new credentials */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Clear WIFI_CONNECTED_BIT before starting connection test */
    EventGroupHandle_t wifi_events = wifi_manager_get_event_group();
    xEventGroupClearBits(wifi_events, BIT0);

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, s_test_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, s_test_password, sizeof(sta_config.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();

    /* Wait for connection result */
    EventBits_t bits = xEventGroupWaitBits(wifi_events, BIT0,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(CAPTIVE_PORTAL_CONNECT_TIMEOUT_MS));

    if (bits & BIT0) {
        /* Connection successful */
        s_test_success = true;
        wifi_manager_get_ip(s_connected_ip, sizeof(s_connected_ip));

        ESP_LOGI(TAG, "Connection successful! IP: %s", s_connected_ip);

        /* Save credentials to NVS */
        wifi_manager_config_t nvs_config = {0};
        strlcpy(nvs_config.ssid, s_test_ssid, sizeof(nvs_config.ssid));
        strlcpy(nvs_config.password, s_test_password, sizeof(nvs_config.password));
        nvs_config.max_retry = CONFIG_WIFI_MAXIMUM_RETRY;
        esp_err_t nvs_ret = wifi_config_save_to_nvs(&nvs_config);
        if (nvs_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save WiFi config to NVS: %s", esp_err_to_name(nvs_ret));
            strlcpy(s_test_error, "Connected but NVS save failed", sizeof(s_test_error));
            /* Still count as success - WiFi is connected, just won't persist */
        }

        s_state = CAPTIVE_PORTAL_SUCCESS;
        xEventGroupSetBits(s_portal_events, PORTAL_SUCCESS_BIT);
    } else {
        /* Connection failed */
        ESP_LOGW(TAG, "Connection to '%s' failed", s_test_ssid);
        s_test_success = false;
        strlcpy(s_test_error, "Connection failed or timed out", sizeof(s_test_error));
        s_state = CAPTIVE_PORTAL_ACTIVE;

        /* Disconnect failed attempt */
        esp_wifi_disconnect();
    }

    s_test_in_progress = false;
    vTaskDelete(NULL);
}

/* ============================================================================
 * HTTP Handlers
 * ============================================================================ */

/**
 * @brief Handler for GET / - Main configuration page
 *
 * Uses strstr-based template replacement for efficiency.
 */
static esp_err_t http_get_root_handler(httpd_req_t *req)
{
    /* Allocate response buffer in PSRAM */
    char *resp = mem_alloc(CAPTIVE_PORTAL_RESPONSE_BUF_SIZE, MEM_CAP_PSRAM);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Generate scan results */
    char *scan_html = mem_alloc(CAPTIVE_PORTAL_SCAN_BUF_SIZE, MEM_CAP_PSRAM);
    if (!scan_html) {
        mem_ng_free(resp);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    generate_scan_html(scan_html, CAPTIVE_PORTAL_SCAN_BUF_SIZE);

    /* Build response: replace placeholders using strstr */
    const char *ver = version_get_number();
    const char *src = CAPTIVE_PORTAL_HTML;
    int written = 0;
    int resp_size = CAPTIVE_PORTAL_RESPONSE_BUF_SIZE;

    while (*src && written < resp_size - 1) {
        /* Find next placeholder */
        const char *scan_pos = strstr(src, "%SCAN_RESULTS%");
        const char *ver_pos = strstr(src, "%VERSION%");

        /* Determine which comes first */
        const char *next = NULL;
        if (scan_pos && (!ver_pos || scan_pos < ver_pos)) {
            next = scan_pos;
        } else if (ver_pos) {
            next = ver_pos;
        }

        if (!next) {
            /* No more placeholders - copy rest */
            int remaining = strlen(src);
            if (written + remaining > resp_size - 1) {
                remaining = resp_size - 1 - written;
            }
            memcpy(resp + written, src, remaining);
            written += remaining;
            break;
        }

        /* Copy text before placeholder */
        int prefix_len = next - src;
        if (prefix_len > 0 && written + prefix_len < resp_size - 1) {
            memcpy(resp + written, src, prefix_len);
            written += prefix_len;
        }

        /* Replace placeholder */
        if (next == scan_pos) {
            int slen = strlen(scan_html);
            if (written + slen < resp_size - 1) {
                memcpy(resp + written, scan_html, slen);
                written += slen;
            }
            src = scan_pos + 14; /* strlen("%SCAN_RESULTS%") */
        } else {
            int vlen = strlen(ver);
            if (written + vlen < resp_size - 1) {
                memcpy(resp + written, ver, vlen);
                written += vlen;
            }
            src = ver_pos + 9; /* strlen("%VERSION%") */
        }
    }
    resp[written] = '\0';

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, written);

    mem_ng_free(scan_html);
    mem_ng_free(resp);
    return ESP_OK;
}

/**
 * @brief Handler for GET /scan - Rescan networks (returns HTML fragment)
 */
static esp_err_t http_get_scan_handler(httpd_req_t *req)
{
    char *scan_html = mem_alloc(CAPTIVE_PORTAL_SCAN_BUF_SIZE, MEM_CAP_PSRAM);
    if (!scan_html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int len = generate_scan_html(scan_html, CAPTIVE_PORTAL_SCAN_BUF_SIZE);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, scan_html, len);

    mem_ng_free(scan_html);
    return ESP_OK;
}

/**
 * @brief URL-decode a string in place
 */
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Extract a form parameter value from URL-encoded body
 */
static bool extract_param(const char *body, const char *key, char *value, size_t value_size)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= value_size) {
                len = value_size - 1;
            }
            memcpy(value, p, len);
            value[len] = '\0';
            url_decode(value);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

/**
 * @brief Handler for POST /connect - Start async credential test
 *
 * Starts a background task for the connection test and returns immediately.
 * The client polls /status for the result.
 */
static esp_err_t http_post_connect_handler(httpd_req_t *req)
{
    /* Reject if test already in progress */
    if (s_test_in_progress) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Test already in progress\"}");
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"No data received\"}");
        return ESP_OK;
    }
    body[received] = '\0';

    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char password[WIFI_PASSWORD_MAX_LEN] = {0};

    if (!extract_param(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing SSID\"}");
        return ESP_OK;
    }
    extract_param(body, "password", password, sizeof(password));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"SSID is empty\"}");
        return ESP_OK;
    }

    /* Store test credentials and start background task */
    strlcpy(s_test_ssid, ssid, sizeof(s_test_ssid));
    strlcpy(s_test_password, password, sizeof(s_test_password));
    s_test_success = false;
    s_test_in_progress = true;
    s_test_error[0] = '\0';
    s_state = CAPTIVE_PORTAL_TESTING;

    BaseType_t task_ret = xTaskCreate(connect_test_task, "wifi_test",
                                       3072, NULL, 5, NULL);  /* Reduced from 4096 */
    if (task_ret != pdPASS) {
        s_test_in_progress = false;
        s_state = CAPTIVE_PORTAL_ACTIVE;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to start test task\"}");
        return ESP_OK;
    }

    /* Return immediately - client polls /status */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"testing\":true}");
    return ESP_OK;
}

/**
 * @brief Handler for GET /status - Connection test status
 */
static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    char resp[192];

    if (s_test_in_progress) {
        snprintf(resp, sizeof(resp), "{\"state\":\"testing\"}");
    } else if (s_test_success) {
        snprintf(resp, sizeof(resp),
                 "{\"state\":\"connected\",\"ip\":\"%s\"}", s_connected_ip);
    } else if (s_test_error[0] != '\0') {
        snprintf(resp, sizeof(resp),
                 "{\"state\":\"failed\",\"error\":\"%s\"}", s_test_error);
    } else {
        snprintf(resp, sizeof(resp), "{\"state\":\"idle\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/**
 * @brief Handler for captive portal detection URLs (redirect to /)
 */
static esp_err_t http_captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

/* ============================================================================
 * HTTP Server Setup
 * ============================================================================ */

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = CAPTIVE_PORTAL_HTTP_STACK_SIZE;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    /* HTTPD_DEFAULT_CONFIG() asks for 7 open sockets. With
     * CONFIG_LWIP_MAX_SOCKETS=8 and 3 sockets taken by the server itself, the
     * ceiling is 5 — httpd_start() rejected the config with
     * ESP_ERR_INVALID_ARG, so the captive portal never came up. That is
     * exactly the situation where it is needed: it only runs when WiFi failed.
     * Four concurrent connections is plenty for a one-page config form. */
    config.max_open_sockets = 4;

    esp_err_t ret = httpd_start(&s_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
    const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET,
        .handler = http_get_root_handler
    };
    const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET,
        .handler = http_get_scan_handler
    };
    const httpd_uri_t uri_connect = {
        .uri = "/connect", .method = HTTP_POST,
        .handler = http_post_connect_handler
    };
    const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET,
        .handler = http_get_status_handler
    };
    const httpd_uri_t uri_generate_204 = {
        .uri = "/generate_204", .method = HTTP_GET,
        .handler = http_captive_redirect_handler
    };
    const httpd_uri_t uri_hotspot = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET,
        .handler = http_captive_redirect_handler
    };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_connect);
    httpd_register_uri_handler(s_httpd, &uri_status);
    httpd_register_uri_handler(s_httpd, &uri_generate_204);
    httpd_register_uri_handler(s_httpd, &uri_hotspot);

    ESP_LOGI(TAG, "HTTP server started with 6 URI handlers");
    return ESP_OK;
}

/* ============================================================================
 * AP Setup
 * ============================================================================ */

static esp_err_t start_ap(void)
{
    /* Create AP netif if not already created */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "Failed to create AP netif");
            return ESP_FAIL;
        }
    }

    /* Switch to APSTA mode */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Generate SSID with MAC suffix */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X",
             CAPTIVE_PORTAL_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t ap_config = {
        .ap = {
            .max_connection = CAPTIVE_PORTAL_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 0, /* Auto - ESP-IDF matches STA channel in APSTA mode */
        },
    };
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "AP started: SSID=%s (open, IP=%s)",
             ap_ssid, CAPTIVE_PORTAL_AP_IP);
    return ESP_OK;
}

/* ============================================================================
 * DNS Task
 * ============================================================================ */

static esp_err_t start_dns_redirect(void)
{
    /* Allocate stack in PSRAM (stored in s_dns_stack for cleanup) */
    s_dns_stack = mem_alloc(CAPTIVE_PORTAL_DNS_STACK_SIZE, MEM_CAP_PSRAM);
    if (!s_dns_stack) {
        ESP_LOGE(TAG, "Failed to allocate DNS task stack in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    static StaticTask_t dns_task_tcb;
    s_dns_task = xTaskCreateStatic(dns_redirect_task, "dns_redir",
                                    CAPTIVE_PORTAL_DNS_STACK_SIZE / sizeof(StackType_t),
                                    NULL, CAPTIVE_PORTAL_DNS_PRIORITY,
                                    s_dns_stack, &dns_task_tcb);
    if (!s_dns_task) {
        mem_ng_free(s_dns_stack);
        s_dns_stack = NULL;
        ESP_LOGE(TAG, "Failed to create DNS redirect task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t captive_portal_start(captive_portal_subsystem_cb_t subsystem_cb)
{
    if (s_state != CAPTIVE_PORTAL_INACTIVE) {
        ESP_LOGW(TAG, "Portal already active (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi Captive Portal");
    ESP_LOGI(TAG, "========================================");

    s_state = CAPTIVE_PORTAL_STARTING;
    s_subsystem_cb = subsystem_cb;
    s_test_success = false;
    s_test_in_progress = false;
    s_test_error[0] = '\0';

    /* Create event group */
    s_portal_events = xEventGroupCreate();
    if (!s_portal_events) {
        ESP_LOGE(TAG, "Failed to create event group");
        s_state = CAPTIVE_PORTAL_INACTIVE;
        return ESP_ERR_NO_MEM;
    }

    /* Disable wifi_manager auto-reconnect so it doesn't fight the portal */
    wifi_manager_set_auto_reconnect(false);

    /* Notify subsystems to stop (frees ~75KB) */
    if (s_subsystem_cb) {
        ESP_LOGI(TAG, "Stopping BLE/ESPHome subsystems for portal...");
        s_subsystem_cb(true);
    }

    ESP_LOGI(TAG, "Heap after subsystem stop: free=%lu, internal=%lu",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Start AP */
    esp_err_t ret = start_ap();
    if (ret != ESP_OK) {
        goto cleanup;
    }

    /* Start DNS redirect */
    ret = start_dns_redirect();
    if (ret != ESP_OK) {
        goto cleanup;
    }

    /* Start HTTP server */
    ret = start_http_server();
    if (ret != ESP_OK) {
        goto cleanup;
    }

    s_state = CAPTIVE_PORTAL_ACTIVE;

    ESP_LOGI(TAG, "Captive portal active (timeout=%ds)",
             CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC);

    /* Block until success or timeout */
    EventBits_t bits = xEventGroupWaitBits(
        s_portal_events,
        PORTAL_SUCCESS_BIT | PORTAL_TIMEOUT_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC * 1000));

    if (bits & PORTAL_SUCCESS_BIT) {
        ESP_LOGI(TAG, "WiFi configured successfully via captive portal");
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Captive portal timed out");
        ret = ESP_ERR_TIMEOUT;
    }

    /* Stop portal */
    captive_portal_stop();
    return ret;

cleanup:
    ESP_LOGE(TAG, "Failed to start captive portal");
    captive_portal_stop();
    return ESP_FAIL;
}

esp_err_t captive_portal_stop(void)
{
    if (s_state == CAPTIVE_PORTAL_INACTIVE) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping captive portal...");
    s_state = CAPTIVE_PORTAL_STOPPING;

    /* Stop HTTP server */
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    /* Signal DNS task to stop and wait for confirmation */
    if (s_dns_task) {
        xTaskNotifyGive(s_dns_task);
        /* Wait up to 3s for the task to finish (recvfrom timeout is 1s) */
        for (int i = 0; i < 30 && s_dns_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_dns_task != NULL) {
            ESP_LOGW(TAG, "DNS task did not exit cleanly");
            s_dns_task = NULL;
        }
    }

    /* Free DNS task stack (PSRAM) - safe now that task has exited */
    if (s_dns_stack) {
        mem_ng_free(s_dns_stack);
        s_dns_stack = NULL;
    }

    /* Switch back to STA-only mode */
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Re-enable wifi_manager auto-reconnect */
    wifi_manager_set_auto_reconnect(true);

    /* Destroy AP netif */
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    /* Clean up event group */
    if (s_portal_events) {
        vEventGroupDelete(s_portal_events);
        s_portal_events = NULL;
    }

    /* Restart subsystems */
    if (s_subsystem_cb) {
        ESP_LOGI(TAG, "Restarting BLE/ESPHome subsystems...");
        s_subsystem_cb(false);
        s_subsystem_cb = NULL;
    }

    s_state = CAPTIVE_PORTAL_INACTIVE;
    ESP_LOGI(TAG, "Captive portal stopped");
    return ESP_OK;
}

captive_portal_state_t captive_portal_get_state(void)
{
    return s_state;
}

bool captive_portal_is_active(void)
{
    return (s_state == CAPTIVE_PORTAL_ACTIVE || s_state == CAPTIVE_PORTAL_TESTING);
}

#else /* CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE not defined */

esp_err_t captive_portal_start(captive_portal_subsystem_cb_t subsystem_cb)
{
    (void)subsystem_cb;
    ESP_LOGW("CAPTIVE_PORTAL", "Captive portal disabled in Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t captive_portal_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

captive_portal_state_t captive_portal_get_state(void)
{
    return CAPTIVE_PORTAL_INACTIVE;
}

bool captive_portal_is_active(void)
{
    return false;
}

#endif /* CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE */
