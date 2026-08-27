/**
 * @file converter_db_ota.c
 * @brief Replace the converter database over HTTP
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "converter_db_ota.h"
#include "zigbee/converter/zb_converter_loader.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_scanner.h"
#endif
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "DB_OTA";

/** Longest single file the database produces is index.json at about 133 KB. */
#define DB_OTA_MAX_FILE_BYTES   (512 * 1024)
#define DB_OTA_HTTP_TIMEOUT_MS  10000
/** How many consecutive quiet reads to sit through before calling it dead. */
/** How long a transfer may make no progress at all before it counts as dead. */
#define DB_OTA_STALL_LIMIT_US   (30 * 1000000LL)
/** HTTP receive buffer. The IDF default of 512 is small against a 1440 MSS. */
#define DB_OTA_RX_BUFFER        4096
#define DB_OTA_TASK_STACK       8192
/* Above the application tasks, below lwIP's TCP/IP task at 18.
 *
 * At 4 this sat level with the ESPHome server on a single core that also runs
 * Zigbee and Wi-Fi, and a task that does not drain its socket promptly has its
 * receive window closed by the peer. Every failed transfer had the same shape:
 * a few tens of KB, then nothing for 30 s. */
/* The download needs to outrun nothing in particular, but it must drain its
 * socket or the peer closes the window — at 4, level with the ESPHome server on
 * a single core that also runs Zigbee and Wi-Fi, it never finished. Ten works.
 *
 * A check is a different animal now that it reads a 139-byte manifest instead
 * of a 134 KB index: it finishes in milliseconds and has no business delaying
 * anything, least of all the priority-4 zb_prov and zb_avail tasks that a
 * freshly paired device depends on. */
#define DB_OTA_TASK_PRIO        10
#define DB_OTA_CHECK_TASK_PRIO  3
#define DB_OTA_URL_MAX          192

static volatile bool s_running;
static char s_status[96] = "idle";
static char s_base_url[DB_OTA_URL_MAX];

/** Set when the run should stop after index.json and only compare revisions. */
static bool s_check_only = false;
/** Revision the source offers, from its index.json. Empty until a check runs. */
static char s_available_revision[40] = "";
/** True when the source offers a revision different from the installed one. */
static bool s_update_available = false;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_status);
}

bool converter_db_ota_in_progress(void) { return s_running; }
const char *converter_db_ota_status(void) { return s_status; }

/* ------------------------------------------------------------------------ */

/** @brief Remove a directory and everything in it. Missing is not an error. */
static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *ent;
    char path[160];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%.80s/%.64s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/**
 * @brief Fetch one file into the staging directory
 *
 * The body is buffered in PSRAM before it touches the filesystem, so a
 * connection that dies halfway leaves no half-written file behind for the
 * validation pass to accept.
 */
static esp_err_t fetch_file(const char *name, char **out_body, size_t *out_len)
{
    char url[DB_OTA_URL_MAX + 80];
    snprintf(url, sizeof(url), "%.191s/%.64s", s_base_url, name);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = DB_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        /* Lets an https:// URL work, which is the only kind GitHub serves. The
         * certificate bundle is already in the build; it was simply never
         * attached here, so every https source failed at the handshake.
         *
         * Worth knowing before reaching for it: TLS tolerates packet loss less
         * well than plain HTTP, because a record that arrives incomplete ends
         * the session rather than costing one retransmission. On a link that is
         * dropping packets, a plain http:// source is the easier one. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* The default receive buffer is 512 bytes against a 1440-byte MSS, so
         * every segment takes three reads to drain and the socket spends most
         * of its time holding data the application has not collected yet. */
        .buffer_size = DB_OTA_RX_BUFFER,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }

    int64_t len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", name, status);
        set_status("%.24s: HTTP %d", name, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    if (len <= 0 || len > DB_OTA_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "%s: length %lld is outside what can be read", name, (long long)len);
        set_status("%.24s: length %lld rejected", name, (long long)len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = mem_ng_calloc(1, (size_t)len + 1, MEM_CAP_PSRAM);
    if (body == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Neither read() nor read_response() on their own.
     *
     * esp_http_client_read() returns -ESP_ERR_HTTP_EAGAIN when the socket had
     * nothing ready inside timeout_ms, which on a radio shared with Zigbee is a
     * normal pause rather than the end of the body. esp_http_client_read_response()
     * loops over read() but returns the moment it sees anything <= 0, EAGAIN
     * included — so one quiet moment truncated the whole file and the first
     * attempt stopped after 1245 of 134381 bytes.
     *
     * Loop here instead and treat a stall as "keep waiting", bounded so that a
     * connection which really has died still ends the run. */
    int got = 0;
    int64_t last_progress = esp_timer_get_time();
    while (got < (int)len) {
        int r = esp_http_client_read(client, body + got, (int)len - got);
        if (r > 0) {
            got += r;
            last_progress = esp_timer_get_time();
            continue;
        }
        /* Zero, not a negative sentinel.
         *
         * esp_transport_read() returns 0 when the socket had nothing ready
         * inside its timeout, and esp_http_client_read() passes that straight
         * out as the count it has collected so far — which is 0 when the pause
         * came first. It does not return -ESP_ERR_HTTP_EAGAIN on this path, so
         * a loop that only waits through EAGAIN gives up on the first quiet
         * moment, which is what happened here: the transfer was working, 4096
         * then 2048 bytes at a time, and was abandoned at the first gap.
         *
         * A negative value is a real error and ends the run. Zero is a pause,
         * and pauses are the normal condition on a radio that time-shares with
         * Zigbee. The bound is on time without progress rather than on a count
         * of pauses, because the number of pauses says nothing about whether
         * the connection is still alive. */
        if (r == 0 || r == -ESP_ERR_HTTP_EAGAIN) {
            if (esp_timer_get_time() - last_progress < DB_OTA_STALL_LIMIT_US) {
                continue;
            }
            ESP_LOGW(TAG, "%s: no progress for %d s at %d of %lld bytes",
                     name, (int)(DB_OTA_STALL_LIMIT_US / 1000000),
                     got, (long long)len);
            break;
        }
        ESP_LOGW(TAG, "%s: read returned %d at %d of %lld bytes",
                 name, r, got, (long long)len);
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (got != (int)len) {
        ESP_LOGW(TAG, "%s: %d of %lld bytes", name, got, (long long)len);
        set_status("%.24s: got %d of %lld bytes", name, got, (long long)len);
        mem_ng_free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    body[len] = '\0';
    *out_body = body;
    *out_len = (size_t)len;
    return ESP_OK;
}

/** @brief Write a staged file, having first checked that it is valid JSON. */
static esp_err_t stage_file(const char *name, char *body, size_t len)
{
    cJSON *probe = cJSON_Parse(body);
    if (probe == NULL) {
        ESP_LOGW(TAG, "%s does not parse — refusing to stage it", name);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(probe);

    char path[160];
    snprintf(path, sizeof(path), "%s/%.64s", CONVERTER_DB_STAGING, name);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot write %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(body, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "%s: wrote %zu of %zu bytes", name, written, len);
        unlink(path);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/** @brief Every filename the index names, plus the profile files. */
static esp_err_t collect_names(const char *index_body, char ***out, size_t *out_count)
{
    cJSON *root = cJSON_Parse(index_body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *manufacturers = cJSON_GetObjectItem(root, "manufacturers");
    if (manufacturers == NULL || !cJSON_IsObject(manufacturers)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t cap = 64, count = 0;
    char **names = mem_ng_calloc(cap, sizeof(char *), MEM_CAP_PSRAM);
    if (names == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, manufacturers) {
        cJSON *files = cJSON_GetObjectItem(entry, "files");
        cJSON *file = cJSON_GetObjectItem(entry, "file");

        const char *candidates[8];
        int n = 0;
        if (files != NULL && cJSON_IsArray(files)) {
            cJSON *f = NULL;
            cJSON_ArrayForEach(f, files) {
                if (cJSON_IsString(f) && n < 8) candidates[n++] = cJSON_GetStringValue(f);
            }
        } else if (file != NULL && cJSON_IsString(file)) {
            candidates[n++] = cJSON_GetStringValue(file);
        }

        for (int i = 0; i < n; i++) {
            bool seen = false;
            for (size_t j = 0; j < count; j++) {
                if (strcmp(names[j], candidates[i]) == 0) { seen = true; break; }
            }
            if (seen) continue;

            if (count == cap) {
                cap *= 2;
                char **bigger = mem_ng_calloc(cap, sizeof(char *), MEM_CAP_PSRAM);
                if (bigger == NULL) break;
                memcpy(bigger, names, count * sizeof(char *));
                mem_ng_free(names);
                names = bigger;
            }
            names[count++] = strdup(candidates[i]);
        }
    }
    cJSON_Delete(root);

    *out = names;
    *out_count = count;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */

static void db_ota_task(void *arg)
{
    (void)arg;
    char **names = NULL;
    size_t name_count = 0;
    char *index_body = NULL;
    size_t index_len = 0;
    bool ok = false;

    /* The HTTP client and the transport describe every read they attempt, at
     * debug level, and that is the only view into why a transfer stops. Turned
     * on for the duration of an update rather than globally, because it is
     * several lines per 4 KB otherwise. */
    /* Pause BLE for the duration.
     *
     * Wi-Fi, Zigbee and BLE share one RF path on this SoC, and a database
     * update is the most sustained receive this firmware ever does. Measured
     * with the scanner running: the board read 2640 bytes of a 134381-byte file
     * and then received nothing for 30 s, while its own ESPHome session dropped
     * out of Home Assistant. The app OTA path already stops the scanner for the
     * same reason; this one did not.
     *
     * Restarted at the end, whatever the outcome. */
#if CONFIG_BT_SCANNER_ENABLED
    bool ble_was_running = ble_scanner_is_running();
    if (ble_was_running) {
        ble_scanner_stop();
        ESP_LOGI(TAG, "BLE scanner paused for the database update");
    }
#endif

    esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANSPORT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_DEBUG);

    set_status("fetching index");

    remove_tree(CONVERTER_DB_STAGING);
    if (mkdir(CONVERTER_DB_STAGING, 0777) != 0) {
        set_status("cannot create the staging directory");
        goto done;
    }

    /* A check asks one question, and manifest.json answers it in 139 bytes.
     *
     * This used to fetch index.json — 134 KB — to compare a revision string,
     * which on this link takes minutes and holds a network task above the
     * Zigbee provisioning tasks for all of it. A source that predates the
     * manifest simply 404s here and the code falls through to the index, which
     * is what it always did. */
    if (s_check_only) {
        char *man = NULL;
        size_t man_len = 0;
        if (fetch_file("manifest.json", &man, &man_len) == ESP_OK) {
            cJSON *mj = cJSON_Parse(man);
            if (mj != NULL) {
                cJSON *rev = cJSON_GetObjectItem(mj, "db_revision");
                if (rev && cJSON_IsString(rev) && cJSON_GetStringValue(rev)) {
                    strlcpy(s_available_revision, cJSON_GetStringValue(rev),
                            sizeof(s_available_revision));
                    const char *installed = zb_converter_loader_get_revision();
                    s_update_available = (installed[0] != '\0') &&
                                         (strcmp(installed, s_available_revision) != 0);
                    set_status(s_update_available
                               ? "update available: %.24s"
                               : "up to date (%.24s)", s_available_revision);
                    ok = true;
                }
                cJSON_Delete(mj);
            }
            mem_ng_free(man);
            if (ok) {
                goto done;
            }
        }
        ESP_LOGI(TAG, "No usable manifest.json — falling back to index.json");
    }

    if (fetch_file("index.json", &index_body, &index_len) != ESP_OK) {
        /* fetch_file() has already described what went wrong — how many bytes
         * arrived, or which HTTP status came back. Replacing that with a
         * generic line throws away the only detail worth having. */
        if (strncmp(s_status, "fetching", 8) == 0) {
            set_status("index.json could not be fetched");
        }
        goto done;
    }
    if (stage_file("index.json", index_body, index_len) != ESP_OK) {
        set_status("index.json is not usable");
        goto done;
    }
    /* A check reads the revision and stops. Fetching 134 KB to answer "is there
     * anything new" is already more than one would like; fetching the whole
     * 2.6 MB database to find out would be absurd. */
    {
        cJSON *idx = cJSON_Parse(index_body);
        if (idx != NULL) {
            cJSON *rev = cJSON_GetObjectItem(idx, "db_revision");
            if (rev && cJSON_IsString(rev) && cJSON_GetStringValue(rev)) {
                strlcpy(s_available_revision, cJSON_GetStringValue(rev),
                        sizeof(s_available_revision));
                const char *installed = zb_converter_loader_get_revision();
                s_update_available = (installed[0] != '\0') &&
                                     (strcmp(installed, s_available_revision) != 0);
            } else {
                s_available_revision[0] = '\0';
                s_update_available = false;
            }
            cJSON_Delete(idx);
        }
    }
    if (s_check_only) {
        if (s_available_revision[0] == '\0') {
            set_status("source has no revision to compare");
        } else if (s_update_available) {
            set_status("update available: %.24s", s_available_revision);
        } else {
            set_status("up to date (%.24s)", s_available_revision);
        }
        ok = true;
        goto done;
    }

    if (collect_names(index_body, &names, &name_count) != ESP_OK) {
        set_status("index.json names no files");
        goto done;
    }

    /* Profiles are not in the index — the file follows from the id, which is
     * the point of that scheme — so they are fetched until one is missing. */
    size_t fetched = 0;
    for (size_t i = 0; i < name_count; i++) {
        char *body = NULL;
        size_t len = 0;
        if (fetch_file(names[i], &body, &len) != ESP_OK) {
            set_status("%s could not be fetched", names[i]);
            goto done;
        }
        esp_err_t ret = stage_file(names[i], body, len);
        mem_ng_free(body);
        if (ret != ESP_OK) {
            set_status("%s is not usable", names[i]);
            goto done;
        }
        fetched++;
        if ((fetched % 16) == 0) {
            set_status("fetched %zu of %zu files", fetched, name_count);
        }
    }

    for (int profile = 0; profile < 64; profile++) {
        char name[32];
        snprintf(name, sizeof(name), "profiles_%d.json", profile);
        char *body = NULL;
        size_t len = 0;
        if (fetch_file(name, &body, &len) != ESP_OK) {
            break;   /* the first missing one ends the run */
        }
        esp_err_t ret = stage_file(name, body, len);
        mem_ng_free(body);
        if (ret != ESP_OK) {
            set_status("%s is not usable", name);
            goto done;
        }
        fetched++;
    }

    /* Swap. The database in use is kept until the new one has loaded, so a
     * database that parses file by file and still fails to index does not cost
     * the gateway the one it was running on. */
    set_status("swapping in %zu files", fetched);
    remove_tree(CONVERTER_DB_PREVIOUS);
    if (rename(CONVERTER_DB_DIR, CONVERTER_DB_PREVIOUS) != 0) {
        set_status("could not set the running database aside");
        goto done;
    }
    if (rename(CONVERTER_DB_STAGING, CONVERTER_DB_DIR) != 0) {
        rename(CONVERTER_DB_PREVIOUS, CONVERTER_DB_DIR);
        set_status("could not move the new database into place");
        goto done;
    }

    if (zb_converter_loader_reload_index() != ESP_OK) {
        remove_tree(CONVERTER_DB_DIR);
        rename(CONVERTER_DB_PREVIOUS, CONVERTER_DB_DIR);
        zb_converter_loader_reload_index();
        set_status("new database did not load — rolled back");
        goto done;
    }

    remove_tree(CONVERTER_DB_PREVIOUS);
    set_status("updated, %zu files", fetched);
    ok = true;

done:
#if CONFIG_BT_SCANNER_ENABLED
    if (ble_was_running && !ble_scanner_is_running()) {
        ble_scanner_start();
        ESP_LOGI(TAG, "BLE scanner resumed");
    }
#endif
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("TRANSPORT", ESP_LOG_INFO);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_INFO);
    if (!ok) {
        remove_tree(CONVERTER_DB_STAGING);
    }
    if (index_body) mem_ng_free(index_body);
    if (names) {
        for (size_t i = 0; i < name_count; i++) free(names[i]);
        mem_ng_free(names);
    }
    s_running = false;
    vTaskDelete(NULL);
}

/** @brief Shared by the update and the check; @p check_only stops after the index. */
static esp_err_t db_ota_begin(const char *base_url, bool check_only)
{
    if (base_url == NULL || base_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
    /* A trailing slash would give "…//index.json", which some servers reject. */
    size_t n = strlen(s_base_url);
    while (n > 0 && s_base_url[n - 1] == '/') {
        s_base_url[--n] = '\0';
    }

    s_check_only = check_only;
    s_running = true;
    set_status(check_only ? "checking" : "starting");

    if (xTaskCreate(db_ota_task, check_only ? "db_check" : "db_ota",
                    DB_OTA_TASK_STACK, NULL,
                    check_only ? DB_OTA_CHECK_TASK_PRIO : DB_OTA_TASK_PRIO,
                    NULL) != pdPASS) {
        s_running = false;
        set_status("could not start the download task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t converter_db_ota_start(const char *base_url)
{
    return db_ota_begin(base_url, false);
}

esp_err_t converter_db_ota_check(const char *base_url)
{
    return db_ota_begin(base_url, true);
}

bool converter_db_ota_update_available(void)
{
    return s_update_available;
}

const char *converter_db_ota_available_revision(void)
{
    return s_available_revision;
}
