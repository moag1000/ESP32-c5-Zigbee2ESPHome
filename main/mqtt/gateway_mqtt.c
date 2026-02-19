/**
 * @file gateway_mqtt.c
 * @brief MQTT Client Implementation
 *
 * Note: Renamed from mqtt_client.c to avoid collision with ESP-IDF's mqtt_client.h
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

/* Include ESP-IDF MQTT client first (angle brackets to avoid local shadow) */
#include <mqtt_client.h>
/* Our wrapper header */
#include "gateway_mqtt.h"
#include "mqtt_buffer.h"
#include "gateway_defaults.h"
#include "mqtt_topics.h"
#include "core/monitoring/perf_metrics.h"
#include "core/events/event_bus.h"
#include "core/memory/buffer_pool_helpers.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* LED status integration */
#if CONFIG_GW_LED_ENABLED
#include "core/led_status_manager.h"
#endif

/* Log tag */
static const char *TAG = "MQTT";

/* MQTT client state */
static struct {
    esp_mqtt_client_handle_t client;
    mqtt_config_t config;
    mqtt_state_t state;
    mqtt_stats_t stats;
    mqtt_message_callback_t message_callback;
    mqtt_connection_callback_t connection_callback;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t publish_mutex;
    bool initialized;
    bool started;
    bool wifi_connected;                /**< Track WiFi connectivity for pause/resume */
    bool paused_for_wifi;               /**< True if MQTT was paused due to WiFi loss */
    /* Latency measurement state */
    int64_t ping_sent_time_us;          /**< Timestamp when ping was sent (microseconds) */
    uint32_t latency_ms;                /**< Last measured latency in milliseconds */
    int ping_msg_id;                    /**< Message ID of the ping publish (for PUBACK tracking) */
    bool latency_measuring;             /**< True if a latency measurement is in progress */
} s_mqtt = {
    .state = MQTT_STATE_DISCONNECTED,
    .initialized = false,
    .started = false,
    .wifi_connected = false,
    .paused_for_wifi = false,
    .ping_sent_time_us = 0,
    .latency_ms = MQTT_LATENCY_UNKNOWN,
    .ping_msg_id = -1,
    .latency_measuring = false,
};

/* Forward declarations */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data);
static void mqtt_wifi_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void set_state(mqtt_state_t new_state);

/**
 * @brief Set MQTT state (thread-safe)
 */
static void set_state(mqtt_state_t new_state)
{
    if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (s_mqtt.state != new_state) {
            ESP_LOGI(TAG, "State: %d -> %d", s_mqtt.state, new_state);
            s_mqtt.state = new_state;
        }
        xSemaphoreGive(s_mqtt.state_mutex);
    }
}

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker");
            s_mqtt.stats.connect_count++;
            set_state(MQTT_STATE_CONNECTED);

            /* Record MQTT connection for performance metrics tracking */
            perf_metrics_mqtt_connected();

            /* Publish MQTT connected event to event bus */
            event_publish(EVT_MQTT_CONNECTED, NULL, 0);

#if CONFIG_GW_LED_ENABLED
            /* Update LED status: MQTT connected */
            led_status_manager_set_condition(LED_COND_MQTT_CONNECTED, true);
#endif

            /* Flush any buffered messages from offline period (e.g., during PAIRING) */
            if (!mqtt_buffer_is_empty()) {
                ESP_LOGI(TAG, "Flushing %zu buffered messages after reconnect...",
                         mqtt_buffer_count());
                esp_err_t flush_ret = mqtt_buffer_flush();
                if (flush_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Some buffered messages failed to send");
                }
            }

            /* Call connection callback */
            if (s_mqtt.connection_callback) {
                s_mqtt.connection_callback(true);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            s_mqtt.stats.disconnect_count++;
            set_state(MQTT_STATE_DISCONNECTED);

            /* Record MQTT disconnection for performance metrics tracking */
            perf_metrics_mqtt_disconnected();

            /* Publish MQTT disconnected event to event bus */
            event_publish(EVT_MQTT_DISCONNECTED, NULL, 0);

#if CONFIG_GW_LED_ENABLED
            /* Update LED status: MQTT disconnected */
            led_status_manager_set_condition(LED_COND_MQTT_CONNECTED, false);
#endif

            /* Call connection callback */
            if (s_mqtt.connection_callback) {
                s_mqtt.connection_callback(false);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribed to topic (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Unsubscribed from topic (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Message published (msg_id=%d)", event->msg_id);
            s_mqtt.stats.publish_count++;

            /* Check if this is the PUBACK for our latency ping message */
            if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_mqtt.latency_measuring && event->msg_id == s_mqtt.ping_msg_id) {
                    int64_t now_us = esp_timer_get_time();
                    int64_t latency_us = now_us - s_mqtt.ping_sent_time_us;
                    s_mqtt.latency_ms = (uint32_t)(latency_us / 1000);
                    s_mqtt.latency_measuring = false;
                    s_mqtt.ping_msg_id = -1;
                    ESP_LOGI(TAG, "MQTT latency measured: %lu ms", s_mqtt.latency_ms);
                }
                xSemaphoreGive(s_mqtt.state_mutex);
            }
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received message: topic=%.*s, data_len=%d",
                    event->topic_len, event->topic, event->data_len);

            s_mqtt.stats.receive_count++;

            /* Record MQTT message received for performance metrics */
            perf_metrics_record(PERF_METRIC_MQTT_RECEIVED, 1);

            /* Call message callback */
            if (s_mqtt.message_callback) {
                /* Null-terminate topic and data for callback */
                char topic[MQTT_LONG_STR_MAX_LEN] = {0};
                char data[MQTT_PAYLOAD_MEDIUM] = {0};

                size_t topic_len = event->topic_len < sizeof(topic) - 1 ?
                                  event->topic_len : sizeof(topic) - 1;
                size_t data_len = event->data_len < sizeof(data) - 1 ?
                                 event->data_len : sizeof(data) - 1;

                memcpy(topic, event->topic, topic_len);
                memcpy(data, event->data, data_len);

                s_mqtt.message_callback(topic, data, data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            set_state(MQTT_STATE_ERROR);

            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP transport error: %d", event->error_handle->esp_transport_sock_errno);
            }
            break;

        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "Connecting to MQTT broker...");
            set_state(MQTT_STATE_CONNECTING);
            s_mqtt.stats.reconnect_count++;
            break;

        default:
            break;
    }
}

/**
 * @brief WiFi event handler for MQTT client
 *
 * Pauses MQTT client when WiFi disconnects and resumes when WiFi reconnects.
 * This prevents unnecessary reconnection attempts when there's no network.
 */
static void mqtt_wifi_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "WiFi disconnected - pausing MQTT client");
            s_mqtt.wifi_connected = false;

            /* Only pause if we were started and not already paused */
            if (s_mqtt.started && s_mqtt.client && !s_mqtt.paused_for_wifi) {
                esp_err_t ret = esp_mqtt_client_stop(s_mqtt.client);
                if (ret == ESP_OK) {
                    s_mqtt.paused_for_wifi = true;
                    /* Set state to DISCONNECTED so mqtt_buffer_should_buffer() works.
                     * esp_mqtt_client_stop() may not fire MQTT_EVENT_DISCONNECTED. */
                    s_mqtt.state = MQTT_STATE_DISCONNECTED;
                    ESP_LOGI(TAG, "MQTT client paused (waiting for WiFi)");
                } else {
                    ESP_LOGW(TAG, "Failed to pause MQTT client: %s", esp_err_to_name(ret));
                }
            } else if (s_mqtt.paused_for_wifi) {
                /* Already paused - just make sure state is correct */
                s_mqtt.state = MQTT_STATE_DISCONNECTED;
            }
        } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "WiFi connected with IP - resuming MQTT client");
            s_mqtt.wifi_connected = true;

            /* Resume if we were paused due to WiFi loss */
            if (s_mqtt.paused_for_wifi && s_mqtt.client) {
                esp_err_t ret = esp_mqtt_client_start(s_mqtt.client);
                if (ret == ESP_OK) {
                    s_mqtt.paused_for_wifi = false;
                    ESP_LOGI(TAG, "MQTT client resumed");
                } else {
                    ESP_LOGW(TAG, "Failed to resume MQTT client: %s", esp_err_to_name(ret));
                }
            }
        }
        xSemaphoreGive(s_mqtt.state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex in WiFi event handler");
    }
}

/**
 * @brief Initialize MQTT client
 */
esp_err_t mqtt_client_init(mqtt_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mqtt.initialized) {
        ESP_LOGW(TAG, "MQTT client already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MQTT client...");

    /* Copy configuration */
    memcpy(&s_mqtt.config, config, sizeof(mqtt_config_t));

    /* Create state mutex */
    s_mqtt.state_mutex = xSemaphoreCreateMutex();
    if (!s_mqtt.state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create publish mutex */
    s_mqtt.publish_mutex = xSemaphoreCreateMutex();
    if (!s_mqtt.publish_mutex) {
        ESP_LOGE(TAG, "Failed to create publish mutex");
        vSemaphoreDelete(s_mqtt.state_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Configure MQTT client
     * Note: If URI has a scheme (mqtt://, mqtts://), do NOT set .transport or .port
     * as ESP-IDF derives transport from the scheme and may conflict if both are set.
     * Also, if URI already contains a port (e.g., mqtt://host:1883), don't set .port
     * as ESP-IDF will append it again resulting in :1883:1883 */
    bool uri_has_scheme = (strstr(s_mqtt.config.broker_url, "://") != NULL);
    bool uri_has_port = false;

    if (uri_has_scheme) {
        const char *scheme_end = strstr(s_mqtt.config.broker_url, "://");
        const char *host_start = scheme_end + 3;
        const char *port_colon = strrchr(host_start, ':');
        if (port_colon != NULL && port_colon[1] >= '0' && port_colon[1] <= '9') {
            uri_has_port = true;
            ESP_LOGD(TAG, "URI already contains port, not setting .port");
        }
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt.config.broker_url,
        .broker.address.port = (uri_has_scheme || uri_has_port) ? 0 : s_mqtt.config.port,
        /* Only set transport if URI has no scheme - ESP-IDF derives transport from scheme */
        .broker.address.transport = uri_has_scheme ? MQTT_TRANSPORT_UNKNOWN :
            (s_mqtt.config.use_ssl ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP),
        .credentials.username = s_mqtt.config.username,
        .credentials.authentication.password = s_mqtt.config.password,
        .credentials.client_id = s_mqtt.config.client_id,
        .session.keepalive = s_mqtt.config.keepalive,
        .session.disable_clean_session = !s_mqtt.config.clean_session,
        /* ESP-IDF internal mqtt_task stack size
         * Error handling during WiFi disconnect uses deep call stacks (~6KB measured)
         * 8KB provides safe headroom for transport_base error paths */
        .task.stack_size = GW_TASK_STACK_MQTT,
        .task.priority = 10,  /* Higher than default 5; must process PUBACK/PINGRESP timely on single-core */
        /* Network resilience settings for WiFi/Zigbee coexistence on ESP32-C5
         * Single radio shared between WiFi+Zigbee causes intermittent WiFi
         * beacon loss. Short timeouts ensure fast recovery instead of hanging
         * on a dead socket for minutes. */
        .network.timeout_ms = GW_TIMEOUT_VERY_LONG_MS,  /* 10s - detect dead socket quickly */
        .network.reconnect_timeout_ms = GW_TIMEOUT_EXTENDED_MS, /* 2s between reconnect attempts */
        .network.disable_auto_reconnect = false,  /* Enable auto-reconnect */
        .buffer.size = GW_BUF_SIZE_4096,          /* Receive buffer for large messages */
        .buffer.out_size = GW_BUF_SIZE_4096,      /* Send buffer for large discovery payloads */
        /* Outbox for QoS1/2 message persistence - must absorb reconnect burst
         * (availability + bridge info + device list + 13 bridge discovery + device discovery) */
        .outbox.limit = GW_MQTT_OUTBOX_LIMIT,       /* 32KB outbox */
        /* Last Will and Testament: broker publishes offline state on unclean disconnect */
        .session.last_will = {
            .topic = TOPIC_BRIDGE_STATE,
            .msg = "{\"state\":\"offline\"}",
            .msg_len = 0,
            .qos = 1,
            .retain = true,
        },
    };

    ESP_LOGI(TAG, "MQTT Config: port=%u, ssl=%s, keepalive=%u",
            s_mqtt.config.port,
            s_mqtt.config.use_ssl ? "enabled" : "disabled",
            s_mqtt.config.keepalive);

    /* Create MQTT client */
    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt.client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vSemaphoreDelete(s_mqtt.state_mutex);
        vSemaphoreDelete(s_mqtt.publish_mutex);
        return ESP_FAIL;
    }

    /* Register MQTT event handler */
    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt.client);
        vSemaphoreDelete(s_mqtt.state_mutex);
        vSemaphoreDelete(s_mqtt.publish_mutex);
        return ret;
    }

    /* Register WiFi event handlers for pause/resume coordination */
    ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     &mqtt_wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register WiFi disconnect handler: %s", esp_err_to_name(ret));
        /* Non-fatal - continue without WiFi coordination */
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &mqtt_wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        /* Non-fatal - continue without WiFi coordination */
    }

    /* Assume WiFi is connected if we got this far (initialized after WiFi) */
    s_mqtt.wifi_connected = true;

    /* Initialize statistics */
    memset(&s_mqtt.stats, 0, sizeof(mqtt_stats_t));

    s_mqtt.initialized = true;
    ESP_LOGI(TAG, "MQTT client initialized successfully");
    ESP_LOGI(TAG, "Broker: %s, Client ID: %s", s_mqtt.config.broker_url,
            s_mqtt.config.client_id);

    return ESP_OK;
}

/**
 * @brief Start MQTT client
 */
esp_err_t mqtt_client_start(void)
{
    if (!s_mqtt.initialized) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_FAIL;
    }

    if (s_mqtt.started) {
        ESP_LOGW(TAG, "MQTT client already started");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting MQTT client...");

    /* Start MQTT client */
    esp_err_t err = esp_mqtt_client_start(s_mqtt.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    s_mqtt.started = true;
    s_mqtt.paused_for_wifi = false;
    ESP_LOGI(TAG, "MQTT client started successfully");

    return ESP_OK;
}

/**
 * @brief Stop MQTT client
 */
esp_err_t mqtt_client_stop(void)
{
    if (!s_mqtt.initialized || !s_mqtt.started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping MQTT client...");

    /* Stop MQTT client */
    esp_err_t ret = esp_mqtt_client_stop(s_mqtt.client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop MQTT client: %s", esp_err_to_name(ret));
    }

    /* Reset WiFi-pause state since this is an explicit stop */
    s_mqtt.paused_for_wifi = false;

    s_mqtt.started = false;
    set_state(MQTT_STATE_DISCONNECTED);

    ESP_LOGI(TAG, "MQTT client stopped successfully");
    return ESP_OK;
}

/**
 * @brief Pause MQTT client for pairing phase
 *
 * Stops the MQTT client but keeps paused_for_wifi flag set, so it will
 * automatically restart when WiFi reconnects and gets IP.
 */
esp_err_t mqtt_client_pause_for_pairing(void)
{
    if (!s_mqtt.initialized) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pause");
        return ESP_ERR_TIMEOUT;
    }

    /* Only pause if we were started and not already paused */
    if (s_mqtt.started && s_mqtt.client && !s_mqtt.paused_for_wifi) {
        ESP_LOGI(TAG, "Pausing MQTT client for PAIRING...");

        esp_err_t ret = esp_mqtt_client_stop(s_mqtt.client);
        if (ret == ESP_OK) {
            /* Keep started=true and set paused_for_wifi=true
             * This allows the WiFi event handler to auto-restart when IP comes back */
            s_mqtt.paused_for_wifi = true;
            set_state(MQTT_STATE_DISCONNECTED);
            ESP_LOGI(TAG, "MQTT client paused (will auto-resume when WiFi returns)");
        } else {
            ESP_LOGW(TAG, "Failed to pause MQTT client: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_mqtt.state_mutex);
            return ret;
        }
    } else if (s_mqtt.paused_for_wifi) {
        ESP_LOGI(TAG, "MQTT already paused for WiFi");
    }

    xSemaphoreGive(s_mqtt.state_mutex);
    return ESP_OK;
}

/**
 * @brief Publish MQTT message (direct, no queue)
 *
 * Simplified architecture: Publishes directly to ESP-IDF MQTT client
 * without intermediate queue. More stable and predictable behavior.
 *
 * If MQTT is not connected but buffering is enabled (e.g., during PAIRING phase
 * when WiFi is disconnected), messages are stored in PSRAM and sent when
 * MQTT reconnects.
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data,
                              size_t len, int qos, bool retain)
{
    if (!topic || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0) {
        len = strlen(data);
    }

    /* Check if we should buffer the message (MQTT unavailable but buffering enabled) */
    if (mqtt_buffer_should_buffer()) {
        esp_err_t ret = mqtt_buffer_add(topic, data, len, qos, retain);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Buffered message to %s (%zu bytes)", topic, len);
        } else {
            ESP_LOGW(TAG, "Failed to buffer message to %s: %s", topic, esp_err_to_name(ret));
        }
        return ret;
    }

    if (!s_mqtt.initialized || !s_mqtt.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mqtt.state != MQTT_STATE_CONNECTED) {
        ESP_LOGD(TAG, "Not connected, cannot publish to %s", topic);
        return ESP_ERR_INVALID_STATE;
    }

    /* Direct publish with mutex protection */
    if (xSemaphoreTake(s_mqtt.publish_mutex, pdMS_TO_TICKS(MQTT_CLIENT_PUBLISH_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire publish mutex");
        s_mqtt.stats.publish_fail_count++;
        return ESP_ERR_TIMEOUT;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt.client, topic, data, len, qos, retain);
    xSemaphoreGive(s_mqtt.publish_mutex);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        s_mqtt.stats.publish_fail_count++;
        return ESP_FAIL;
    }

    s_mqtt.stats.publish_count++;
    ESP_LOGD(TAG, "Published to %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

/**
 * @brief Subscribe to MQTT topic
 */
esp_err_t mqtt_client_subscribe(const char *topic, int qos)
{
    if (!topic) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mqtt.initialized || !s_mqtt.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mqtt.state != MQTT_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Not connected, cannot subscribe");
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_subscribe(s_mqtt.client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to topic: %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Subscribed to topic: %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

/**
 * @brief Unsubscribe from MQTT topic
 */
esp_err_t mqtt_client_unsubscribe(const char *topic)
{
    if (!topic) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mqtt.initialized || !s_mqtt.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mqtt.state != MQTT_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Not connected, cannot unsubscribe");
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_unsubscribe(s_mqtt.client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to unsubscribe from topic: %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Unsubscribed from topic: %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

/**
 * @brief Register message callback
 */
esp_err_t mqtt_client_register_callback(mqtt_message_callback_t callback)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mqtt.message_callback = callback;
    ESP_LOGI(TAG, "Message callback registered");
    return ESP_OK;
}

/**
 * @brief Register connection callback
 */
esp_err_t mqtt_client_register_connection_callback(mqtt_connection_callback_t callback)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mqtt.connection_callback = callback;
    ESP_LOGI(TAG, "Connection callback registered");
    return ESP_OK;
}

/**
 * @brief Get MQTT state
 */
mqtt_state_t mqtt_client_get_state(void)
{
    /* Return disconnected if not initialized to avoid mutex NULL pointer */
    if (!s_mqtt.initialized || s_mqtt.state_mutex == NULL) {
        return MQTT_STATE_DISCONNECTED;
    }

    mqtt_state_t state;

    if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = s_mqtt.state;
        xSemaphoreGive(s_mqtt.state_mutex);
    } else {
        state = MQTT_STATE_DISCONNECTED;
    }

    return state;
}

/**
 * @brief Check if MQTT is connected
 */
bool mqtt_client_is_connected(void)
{
    return (mqtt_client_get_state() == MQTT_STATE_CONNECTED);
}

/**
 * @brief Get MQTT statistics
 */
esp_err_t mqtt_client_get_stats(mqtt_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(stats, &s_mqtt.stats, sizeof(mqtt_stats_t));
    return ESP_OK;
}

/**
 * @brief Publish a null-terminated string (auto-calculates length)
 */
esp_err_t mqtt_client_publish_string(const char *topic, const char *payload, int qos, bool retain)
{
    if (!topic || !payload) {
        ESP_LOGE(TAG, "Invalid arguments to mqtt_client_publish_string");
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(payload);
    return mqtt_client_publish(topic, payload, len, qos, retain);
}

/**
 * @brief Publish a cJSON object (serializes and frees the JSON)
 */
esp_err_t mqtt_client_publish_json(const char *topic, cJSON *json, int qos, bool retain)
{
    if (!topic || !json) {
        ESP_LOGE(TAG, "Invalid arguments to mqtt_client_publish_json: topic=%p, json=%p",
                 topic, json);
        if (json) {
            cJSON_Delete(json);
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Serialize JSON to string using buffer pool */
    bool from_pool;
    char *json_str = pool_json_print_unformatted(json, &from_pool);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON to string");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    /* Publish the serialized JSON */
    size_t len = strlen(json_str);
    esp_err_t ret = mqtt_client_publish(topic, json_str, len, qos, retain);

    /* Clean up */
    pool_json_free(json_str, from_pool);
    cJSON_Delete(json);

    return ret;
}

/**
 * @brief Test MQTT client
 */
esp_err_t mqtt_client_test(void)
{
    ESP_LOGI(TAG, "=== MQTT Client Test ===");

    /* Check initialization */
    if (!s_mqtt.initialized) {
        ESP_LOGE(TAG, "Test FAILED: Not initialized");
        return ESP_FAIL;
    }

    /* Print current state */
    mqtt_state_t state = mqtt_client_get_state();
    ESP_LOGI(TAG, "Current state: %d", state);

    /* Print stats */
    mqtt_stats_t stats;
    mqtt_client_get_stats(&stats);
    ESP_LOGI(TAG, "Stats: connects=%lu, disconnects=%lu, reconnects=%lu",
            stats.connect_count, stats.disconnect_count, stats.reconnect_count);
    ESP_LOGI(TAG, "Stats: publish=%lu, publish_fail=%lu, receive=%lu",
            stats.publish_count, stats.publish_fail_count, stats.receive_count);

    ESP_LOGI(TAG, "Test PASSED");
    return ESP_OK;
}

/* ============================================================================
 * MQTT Latency Measurement Implementation
 * ============================================================================ */

/**
 * @brief Start MQTT latency measurement
 *
 * Uses QoS 1 publish and measures time until PUBACK is received.
 * This gives an accurate round-trip latency to the MQTT broker.
 */
esp_err_t mqtt_latency_measure_start(void)
{
    if (!s_mqtt.initialized || !s_mqtt.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mqtt.state != MQTT_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Cannot measure latency: not connected");
        return ESP_ERR_INVALID_STATE;
    }

    /* Protect latency state with state_mutex */
    if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for latency measurement");
        return ESP_ERR_TIMEOUT;
    }

    /* Don't start a new measurement if one is already in progress */
    if (s_mqtt.latency_measuring) {
        xSemaphoreGive(s_mqtt.state_mutex);
        ESP_LOGD(TAG, "Latency measurement already in progress");
        return ESP_OK;
    }

    /* Record the start time */
    s_mqtt.ping_sent_time_us = esp_timer_get_time();
    s_mqtt.latency_measuring = true;
    xSemaphoreGive(s_mqtt.state_mutex);

    /* Create a simple ping payload with timestamp */
    char ping_payload[64];
    snprintf(ping_payload, sizeof(ping_payload), "{\"timestamp\":%lld}",
             s_mqtt.ping_sent_time_us);

    /* Publish directly with QoS 1 to get PUBACK timing
     * We use esp_mqtt_client_publish directly (not the queue) to get
     * the message ID and accurate timing */
    if (xSemaphoreTake(s_mqtt.publish_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        int msg_id = esp_mqtt_client_publish(s_mqtt.client, MQTT_PING_TOPIC,
                                              ping_payload, strlen(ping_payload),
                                              1, false);
        xSemaphoreGive(s_mqtt.publish_mutex);

        /* Update ping_msg_id under state_mutex */
        if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            if (msg_id < 0) {
                ESP_LOGE(TAG, "Failed to publish ping message");
                s_mqtt.latency_measuring = false;
                s_mqtt.ping_msg_id = -1;
                xSemaphoreGive(s_mqtt.state_mutex);
                return ESP_FAIL;
            }
            s_mqtt.ping_msg_id = msg_id;
            xSemaphoreGive(s_mqtt.state_mutex);
            ESP_LOGD(TAG, "Latency measurement started (msg_id=%d)", msg_id);
        }
    } else {
        ESP_LOGW(TAG, "Failed to acquire publish mutex for latency measurement");
        if (xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(MQTT_CLIENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_mqtt.latency_measuring = false;
            xSemaphoreGive(s_mqtt.state_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * @brief Get the last measured MQTT latency (thread-safe)
 */
uint32_t mqtt_latency_get_ms(void)
{
    uint32_t latency = MQTT_LATENCY_UNKNOWN;
    if (s_mqtt.state_mutex &&
        xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        latency = s_mqtt.latency_ms;
        xSemaphoreGive(s_mqtt.state_mutex);
    }
    return latency;
}

/**
 * @brief Check if a latency measurement is in progress (thread-safe)
 */
bool mqtt_latency_is_measuring(void)
{
    bool measuring = false;

    if (!s_mqtt.state_mutex ||
        xSemaphoreTake(s_mqtt.state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    /* Also check for timeout - if measurement has been pending too long, cancel it */
    if (s_mqtt.latency_measuring && s_mqtt.ping_sent_time_us > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_ms = (now_us - s_mqtt.ping_sent_time_us) / 1000;

        if (elapsed_ms > MQTT_LATENCY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Latency measurement timed out after %lld ms", elapsed_ms);
            s_mqtt.latency_measuring = false;
            s_mqtt.ping_msg_id = -1;
            /* Keep the previous latency value, don't update to unknown */
        }
    }

    measuring = s_mqtt.latency_measuring;
    xSemaphoreGive(s_mqtt.state_mutex);

    return measuring;
}
