/**
 * @file bridge_events.c
 * @brief Bridge Event System Implementation
 *
 * Implements centralized event publishing for Zigbee2MQTT bridge events.
 * All events are published to "zigbee2mqtt/bridge/event" topic.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "bridge_events.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/memory/buffer_pool_helpers.h"
#include "gateway_mqtt.h"  /* For mqtt_client_is_connected() check */
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "BRIDGE_EVT";

/* ============================================================================
 * Module State
 * ============================================================================ */

static bool s_initialized = false;
static uint32_t s_total_events = 0;
static uint32_t s_error_count = 0;

/* ============================================================================
 * Early Exit Macro for Convenience Functions
 * ============================================================================
 * Checks initialization and MQTT connection before doing any work.
 * Returns ESP_ERR_INVALID_STATE if not ready to publish.
 */
#define BRIDGE_EVENTS_CHECK_READY() \
    do { \
        if (!s_initialized) { \
            ESP_LOGD(TAG, "Events not initialized - skipping"); \
            return ESP_ERR_INVALID_STATE; \
        } \
        if (!mqtt_client_is_connected()) { \
            ESP_LOGD(TAG, "MQTT not connected - skipping event"); \
            return ESP_ERR_INVALID_STATE; \
        } \
    } while (0)

/* ============================================================================
 * Event Type String Mapping
 * ============================================================================ */

/**
 * @brief Event type to string mapping table
 *
 * Must match bridge_event_type_t enumeration order.
 */
static const char *s_event_type_strings[] = {
    "device_joined",        /* BRIDGE_EVENT_DEVICE_JOINED */
    "device_leave",         /* BRIDGE_EVENT_DEVICE_LEAVE */
    "device_interview",     /* BRIDGE_EVENT_DEVICE_INTERVIEW */
    "device_announce",      /* BRIDGE_EVENT_DEVICE_ANNOUNCE */
    "device_renamed",       /* BRIDGE_EVENT_DEVICE_RENAMED */
    "permit_join",          /* BRIDGE_EVENT_PERMIT_JOIN */
    "network_started",      /* BRIDGE_EVENT_NETWORK_STARTED */
    "network_stopped",      /* BRIDGE_EVENT_NETWORK_STOPPED */
    "device_bind",          /* BRIDGE_EVENT_DEVICE_BIND */
    "device_unbind",        /* BRIDGE_EVENT_DEVICE_UNBIND */
    "group_member_added",   /* BRIDGE_EVENT_GROUP_MEMBER_ADDED */
    "group_member_removed", /* BRIDGE_EVENT_GROUP_MEMBER_REMOVED */
    /* API-009: Enhanced signal events */
    "nlme_status",          /* BRIDGE_EVENT_NLME_STATUS */
    "parent_announce",      /* BRIDGE_EVENT_PARENT_ANNOUNCE */
    "tc_rejoin"             /* BRIDGE_EVENT_TC_REJOIN */
};

static_assert(sizeof(s_event_type_strings) / sizeof(s_event_type_strings[0]) == BRIDGE_EVENT_MAX,
              "Event type string array size mismatch");

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Format IEEE address as hex string
 *
 * @param[in] ieee_addr IEEE address (64-bit)
 * @param[out] buffer Output buffer (must be at least 19 bytes: "0x" + 16 hex + null)
 */
static void format_ieee_addr(uint64_t ieee_addr, char *buffer)
{
    snprintf(buffer, 19, "0x%016" PRIx64, ieee_addr);
}

/**
 * @brief Get friendly name or formatted IEEE address
 *
 * @param[in] ieee_addr IEEE address
 * @param[in] friendly_name Optional friendly name (can be NULL)
 * @param[out] buffer Output buffer for IEEE address if needed
 * @return Pointer to name string (either friendly_name or buffer)
 */
static const char* get_display_name(uint64_t ieee_addr, const char *friendly_name, char *buffer)
{
    if (friendly_name != NULL && friendly_name[0] != '\0') {
        return friendly_name;
    }
    format_ieee_addr(ieee_addr, buffer);
    return buffer;
}

/**
 * @brief Publish event JSON to MQTT via event bus with ownership transfer
 *
 * Internal function that publishes bridge events via the event bus.
 * The mqtt_event_handler.c handles the actual MQTT publishing, which
 * decouples this module from direct MQTT dependencies.
 *
 * Ownership transfer protocol:
 * - Both topic and payload_json are strdup'd before placing in the event.
 * - On success, the handler (publish_ha_discovery in mqtt_event_handler.c)
 *   takes ownership and frees both strings.
 * - On failure, this function frees both copies.
 *
 * @param[in] json_str JSON string to publish (caller retains ownership)
 * @return ESP_OK on success
 */
static esp_err_t publish_event_json(const char *json_str)
{
    /* strdup both strings: handler will free() them after publishing */
    char *topic_copy = strdup(BRIDGE_EVENT_TOPIC);
    char *json_copy = strdup(json_str);

    if (!topic_copy || !json_copy) {
        ESP_LOGE(TAG, "Failed to allocate event strings");
        free(topic_copy);
        free(json_copy);
        s_error_count++;
        return ESP_ERR_NO_MEM;
    }

    /* Publish via event bus - mqtt_event_handler.c will handle actual MQTT publish.
     * This decouples bridge_events from direct MQTT dependencies and allows
     * buffering/retry logic to be handled centrally in mqtt_event_handler. */
    evt_ha_discovery_publish_t evt = {
        .topic = topic_copy,
        .payload_json = json_copy,
        .qos = 0,
        .retain = false
    };

    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish event to bus: %s", esp_err_to_name(ret));
        /* Ownership was NOT transferred - we must free */
        free(topic_copy);
        free(json_copy);
        s_error_count++;
        return ret;
    }

    s_total_events++;
    ESP_LOGD(TAG, "Event published to bus: %s", json_str);
    return ESP_OK;
}

/* ============================================================================
 * Event Bus Handler
 * ============================================================================ */

/**
 * @brief Handle events from the internal event bus
 *
 * Subscribes to device join/leave/interview events and publishes
 * corresponding MQTT bridge events for external subscribers.
 */
static void bridge_event_bus_handler(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)data_size;
    (void)user_ctx;

    switch (type) {
        case EVT_DEVICE_JOINED: {
            if (data != NULL) {
                const evt_device_joined_t *evt = (const evt_device_joined_t *)data;
                /* Look up actual friendly_name from registry */
                const char *name = NULL;
                device_t *dev = device_registry_get(evt->ieee_addr);
                if (dev && dev->friendly_name[0]) {
                    name = dev->friendly_name;
                }
                bridge_event_device_joined(evt->ieee_addr, name);
            }
            break;
        }

        case EVT_DEVICE_LEFT: {
            if (data != NULL) {
                const evt_device_left_t *evt = (const evt_device_left_t *)data;
                const char *name = NULL;
                device_t *dev = device_registry_get(evt->ieee_addr);
                if (dev && dev->friendly_name[0]) {
                    name = dev->friendly_name;
                }
                bridge_event_device_leave(evt->ieee_addr, name);
            }
            break;
        }

        case EVT_DEVICE_INTERVIEWED: {
            if (data != NULL) {
                const evt_device_interviewed_t *evt = (const evt_device_interviewed_t *)data;
                const char *status = evt->success ? BRIDGE_INTERVIEW_SUCCESSFUL : BRIDGE_INTERVIEW_FAILED;
                bridge_event_device_interview(evt->ieee_addr, status);
            }
            break;
        }

        case EVT_ZB_PERMIT_JOIN: {
            if (data != NULL) {
                const evt_permit_join_t *evt = (const evt_permit_join_t *)data;
                bridge_event_permit_join(evt->enabled, evt->duration);
            }
            break;
        }

        case EVT_NETWORK_READY:
            bridge_event_network_started();
            break;

        default:
            break;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t bridge_events_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Bridge events already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_total_events = 0;
    s_error_count = 0;
    s_initialized = true;

    /* Subscribe to internal event bus for device events */
    event_subscribe(EVT_DEVICE_JOINED, bridge_event_bus_handler, NULL);
    event_subscribe(EVT_DEVICE_LEFT, bridge_event_bus_handler, NULL);
    event_subscribe(EVT_DEVICE_INTERVIEWED, bridge_event_bus_handler, NULL);
    event_subscribe(EVT_ZB_PERMIT_JOIN, bridge_event_bus_handler, NULL);
    event_subscribe(EVT_NETWORK_READY, bridge_event_bus_handler, NULL);

    ESP_LOGI(TAG, "Bridge events module initialized (event bus subscribed)");
    return ESP_OK;
}

esp_err_t bridge_events_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unsubscribe all event bus handlers */
    event_unsubscribe(EVT_DEVICE_JOINED, bridge_event_bus_handler);
    event_unsubscribe(EVT_DEVICE_LEFT, bridge_event_bus_handler);
    event_unsubscribe(EVT_DEVICE_INTERVIEWED, bridge_event_bus_handler);
    event_unsubscribe(EVT_ZB_PERMIT_JOIN, bridge_event_bus_handler);
    event_unsubscribe(EVT_NETWORK_READY, bridge_event_bus_handler);

    s_initialized = false;
    ESP_LOGI(TAG, "Bridge events module deinitialized (total: %" PRIu32 ", errors: %" PRIu32 ")",
             s_total_events, s_error_count);
    return ESP_OK;
}

const char* bridge_events_get_type_str(bridge_event_type_t type)
{
    if (type >= BRIDGE_EVENT_MAX) {
        return "unknown";
    }
    return s_event_type_strings[type];
}

esp_err_t bridge_events_publish(bridge_event_type_t type, cJSON *data)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Bridge events not initialized");
        if (data != NULL) {
            cJSON_Delete(data);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (type >= BRIDGE_EVENT_MAX) {
        ESP_LOGE(TAG, "Invalid event type: %d", type);
        if (data != NULL) {
            cJSON_Delete(data);
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Build event JSON */
    cJSON *event = cJSON_CreateObject();
    if (event == NULL) {
        ESP_LOGE(TAG, "Failed to create event JSON");
        if (data != NULL) {
            cJSON_Delete(data);
        }
        s_error_count++;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(event, "type", s_event_type_strings[type]);

    /* Add data object (or empty object if NULL) */
    if (data != NULL) {
        cJSON_AddItemToObject(event, "data", data);
    } else {
        cJSON *empty_data = cJSON_CreateObject();
        if (empty_data != NULL) {
            cJSON_AddItemToObject(event, "data", empty_data);
        }
    }

    /* Convert to string using buffer pool for reduced fragmentation */
    bool from_pool;
    char *json_str = pool_json_print_unformatted(event, &from_pool);
    cJSON_Delete(event);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize event JSON");
        s_error_count++;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Publishing event: %s", s_event_type_strings[type]);

    esp_err_t ret = publish_event_json(json_str);
    pool_json_free(json_str, from_pool);

    return ret;
}

/* ============================================================================
 * Convenience Functions Implementation
 * ============================================================================ */

esp_err_t bridge_event_device_joined(uint64_t ieee_addr, const char *friendly_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char ieee_str[19];
    char name_buf[19];

    format_ieee_addr(ieee_addr, ieee_str);
    const char *name = get_display_name(ieee_addr, friendly_name, name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "friendly_name", name);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    ESP_LOGI(TAG, "Device joined: %s (%s)", name, ieee_str);
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_JOINED, data);
}

esp_err_t bridge_event_device_leave(uint64_t ieee_addr, const char *friendly_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char ieee_str[19];
    char name_buf[19];

    format_ieee_addr(ieee_addr, ieee_str);
    const char *name = get_display_name(ieee_addr, friendly_name, name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "friendly_name", name);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    ESP_LOGI(TAG, "Device leave: %s (%s)", name, ieee_str);
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_LEAVE, data);
}

esp_err_t bridge_event_device_interview(uint64_t ieee_addr, const char *status)
{
    BRIDGE_EVENTS_CHECK_READY();

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char ieee_str[19];
    format_ieee_addr(ieee_addr, ieee_str);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "friendly_name", ieee_str);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);
    cJSON_AddStringToObject(data, "status", status);

    ESP_LOGI(TAG, "Device interview %s: %s", status, ieee_str);
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_INTERVIEW, data);
}

esp_err_t bridge_event_device_announce(uint64_t ieee_addr)
{
    /* Delegate to full version with unknown network address */
    /* Note: Check is done in bridge_event_device_announce_full() */
    return bridge_event_device_announce_full(ieee_addr, 0xFFFF);
}

esp_err_t bridge_event_device_announce_full(uint64_t ieee_addr, uint16_t nwk_addr)
{
    BRIDGE_EVENTS_CHECK_READY();

    char ieee_str[19];
    format_ieee_addr(ieee_addr, ieee_str);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "friendly_name", ieee_str);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    /* Add network address if valid */
    if (nwk_addr != 0xFFFF) {
        char nwk_str[7];
        snprintf(nwk_str, sizeof(nwk_str), "0x%04X", nwk_addr);
        cJSON_AddStringToObject(data, "nwk_address", nwk_str);
    }

    ESP_LOGD(TAG, "Device announce: %s (nwk: 0x%04X)", ieee_str, nwk_addr);
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_ANNOUNCE, data);
}

esp_err_t bridge_event_device_renamed(uint64_t ieee_addr, const char *from, const char *to)
{
    BRIDGE_EVENTS_CHECK_READY();

    if (from == NULL || to == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char ieee_str[19];
    format_ieee_addr(ieee_addr, ieee_str);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "from", from);
    cJSON_AddStringToObject(data, "to", to);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    ESP_LOGI(TAG, "Device renamed: %s -> %s (%s)", from, to, ieee_str);
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_RENAMED, data);
}

esp_err_t bridge_event_permit_join(bool enabled, uint8_t time)
{
    BRIDGE_EVENTS_CHECK_READY();

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(data, "value", enabled);
    cJSON_AddNumberToObject(data, "time", time);

    ESP_LOGI(TAG, "Permit join: %s (time: %d)", enabled ? "enabled" : "disabled", time);
    return bridge_events_publish(BRIDGE_EVENT_PERMIT_JOIN, data);
}

esp_err_t bridge_event_network_started(void)
{
    BRIDGE_EVENTS_CHECK_READY();

    ESP_LOGI(TAG, "Network started");
    return bridge_events_publish(BRIDGE_EVENT_NETWORK_STARTED, NULL);
}

esp_err_t bridge_event_network_stopped(void)
{
    BRIDGE_EVENTS_CHECK_READY();

    ESP_LOGI(TAG, "Network stopped");
    return bridge_events_publish(BRIDGE_EVENT_NETWORK_STOPPED, NULL);
}

esp_err_t bridge_event_device_bind(uint64_t source_ieee, const char *source_name,
                                    uint64_t target_ieee, const char *target_name,
                                    const char *cluster_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char source_ieee_str[19];
    char target_ieee_str[19];
    char source_name_buf[19];
    char target_name_buf[19];

    format_ieee_addr(source_ieee, source_ieee_str);
    format_ieee_addr(target_ieee, target_ieee_str);

    const char *src_name = get_display_name(source_ieee, source_name, source_name_buf);
    const char *tgt_name = get_display_name(target_ieee, target_name, target_name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "from", src_name);
    cJSON_AddStringToObject(data, "from_ieee", source_ieee_str);
    cJSON_AddStringToObject(data, "to", tgt_name);
    cJSON_AddStringToObject(data, "to_ieee", target_ieee_str);
    if (cluster_name != NULL) {
        cJSON_AddStringToObject(data, "cluster", cluster_name);
    }

    ESP_LOGI(TAG, "Device bind: %s -> %s (%s)", src_name, tgt_name,
             cluster_name ? cluster_name : "all");
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_BIND, data);
}

esp_err_t bridge_event_device_unbind(uint64_t source_ieee, const char *source_name,
                                      uint64_t target_ieee, const char *target_name,
                                      const char *cluster_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char source_ieee_str[19];
    char target_ieee_str[19];
    char source_name_buf[19];
    char target_name_buf[19];

    format_ieee_addr(source_ieee, source_ieee_str);
    format_ieee_addr(target_ieee, target_ieee_str);

    const char *src_name = get_display_name(source_ieee, source_name, source_name_buf);
    const char *tgt_name = get_display_name(target_ieee, target_name, target_name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "from", src_name);
    cJSON_AddStringToObject(data, "from_ieee", source_ieee_str);
    cJSON_AddStringToObject(data, "to", tgt_name);
    cJSON_AddStringToObject(data, "to_ieee", target_ieee_str);
    if (cluster_name != NULL) {
        cJSON_AddStringToObject(data, "cluster", cluster_name);
    }

    ESP_LOGI(TAG, "Device unbind: %s -> %s (%s)", src_name, tgt_name,
             cluster_name ? cluster_name : "all");
    return bridge_events_publish(BRIDGE_EVENT_DEVICE_UNBIND, data);
}

esp_err_t bridge_event_group_member_added(const char *group_name, uint16_t group_id,
                                           uint64_t ieee_addr, const char *device_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char ieee_str[19];
    char name_buf[19];

    format_ieee_addr(ieee_addr, ieee_str);
    const char *name = get_display_name(ieee_addr, device_name, name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (group_name != NULL) {
        cJSON_AddStringToObject(data, "group", group_name);
    }
    cJSON_AddNumberToObject(data, "group_id", group_id);
    cJSON_AddStringToObject(data, "device", name);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    ESP_LOGI(TAG, "Group member added: %s -> %s", name, group_name ? group_name : "(unknown)");
    return bridge_events_publish(BRIDGE_EVENT_GROUP_MEMBER_ADDED, data);
}

esp_err_t bridge_event_group_member_removed(const char *group_name, uint16_t group_id,
                                             uint64_t ieee_addr, const char *device_name)
{
    BRIDGE_EVENTS_CHECK_READY();

    char ieee_str[19];
    char name_buf[19];

    format_ieee_addr(ieee_addr, ieee_str);
    const char *name = get_display_name(ieee_addr, device_name, name_buf);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (group_name != NULL) {
        cJSON_AddStringToObject(data, "group", group_name);
    }
    cJSON_AddNumberToObject(data, "group_id", group_id);
    cJSON_AddStringToObject(data, "device", name);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);

    ESP_LOGI(TAG, "Group member removed: %s <- %s", name, group_name ? group_name : "(unknown)");
    return bridge_events_publish(BRIDGE_EVENT_GROUP_MEMBER_REMOVED, data);
}

/* ============================================================================
 * API-009: Enhanced Signal Handler Events
 * ============================================================================ */

/**
 * @brief NLME status code to string helper
 */
const char* nlme_status_to_str(uint8_t status)
{
    /* NWK command status codes per Zigbee spec (esp_zb_nwk_command_status_t) */
    switch (status) {
        case 0x00: return "no_route_available";
        case 0x01: return "tree_link_failure";
        case 0x02: return "non_tree_link_failure";
        case 0x03: return "low_battery_level";
        case 0x04: return "no_routing_capacity";
        case 0x05: return "no_indirect_capacity";
        case 0x06: return "indirect_transaction_expiry";
        case 0x07: return "target_device_unavailable";
        case 0x08: return "target_address_unallocated";
        case 0x09: return "parent_link_failure";
        case 0x0A: return "validate_route";
        case 0x0B: return "source_route_failure";
        case 0x0C: return "many_to_one_route_failure";
        case 0x0D: return "address_conflict";
        case 0x0E: return "verify_address";
        case 0x0F: return "pan_identifier_update";
        case 0x10: return "network_address_update";
        case 0x11: return "bad_frame_counter";
        case 0x12: return "bad_key_sequence_number";
        case 0x13: return "unknown_command";
        case 0xCC: return "route_discovery_failed";
        case 0xCD: return "route_error";
        default:   return "unknown";
    }
}

esp_err_t bridge_event_nlme_status(uint16_t nwk_addr, uint8_t status)
{
    BRIDGE_EVENTS_CHECK_READY();

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create NLME status event JSON");
        return ESP_ERR_NO_MEM;
    }

    char addr_str[7];
    snprintf(addr_str, sizeof(addr_str), "0x%04X", nwk_addr);
    cJSON_AddStringToObject(data, "network_address", addr_str);
    cJSON_AddNumberToObject(data, "status_code", status);
    cJSON_AddStringToObject(data, "status", nlme_status_to_str(status));

    ESP_LOGD(TAG, "NLME status event: addr=%s status=%s (0x%02X)",
             addr_str, nlme_status_to_str(status), status);
    return bridge_events_publish(BRIDGE_EVENT_NLME_STATUS, data);
}

esp_err_t bridge_event_parent_announce(void)
{
    BRIDGE_EVENTS_CHECK_READY();

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create parent announce event JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Add timestamp for topology tracking */
    cJSON_AddNumberToObject(data, "timestamp", (double)(esp_timer_get_time() / 1000000));

    ESP_LOGD(TAG, "Parent announcement event");
    return bridge_events_publish(BRIDGE_EVENT_PARENT_ANNOUNCE, data);
}

esp_err_t bridge_event_tc_rejoin_done(bool success)
{
    BRIDGE_EVENTS_CHECK_READY();

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create TC rejoin event JSON");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(data, "success", success);
    cJSON_AddStringToObject(data, "status", success ? "successful" : "failed");

    ESP_LOGI(TAG, "TC rejoin event: %s", success ? "successful" : "failed");
    return bridge_events_publish(BRIDGE_EVENT_TC_REJOIN, data);
}

esp_err_t bridge_events_get_stats(uint32_t *total_count, uint32_t *error_count)
{
    if (total_count != NULL) {
        *total_count = s_total_events;
    }
    if (error_count != NULL) {
        *error_count = s_error_count;
    }
    return ESP_OK;
}

esp_err_t bridge_events_test(void)
{
    ESP_LOGI(TAG, "Running bridge events self-test...");

    /* Test event type string mapping */
    for (int i = 0; i < BRIDGE_EVENT_MAX; i++) {
        const char *str = bridge_events_get_type_str((bridge_event_type_t)i);
        if (str == NULL || strcmp(str, "unknown") == 0) {
            ESP_LOGE(TAG, "Invalid event type string for type %d", i);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Event type %d: %s", i, str);
    }

    /* Test invalid event type */
    const char *invalid = bridge_events_get_type_str(BRIDGE_EVENT_MAX);
    if (strcmp(invalid, "unknown") != 0) {
        ESP_LOGE(TAG, "Invalid event type should return 'unknown'");
        return ESP_FAIL;
    }

    /* Test IEEE address formatting */
    char ieee_buf[19];
    format_ieee_addr(0x00124B00AABBCCDDULL, ieee_buf);
    if (strcmp(ieee_buf, "0x00124b00aabbccdd") != 0) {
        ESP_LOGE(TAG, "IEEE format mismatch: %s", ieee_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Bridge events self-test PASSED");
    return ESP_OK;
}
