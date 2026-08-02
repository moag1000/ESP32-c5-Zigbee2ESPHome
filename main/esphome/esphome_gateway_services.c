/**
 * @file esphome_gateway_services.c
 * @brief Gateway operations exposed to Home Assistant as ESPHome services
 *
 * esphome_services.c is the generic mechanism — registration, encoding,
 * dispatch. This file is the gateway's actual service surface, kept separate so
 * the framework stays reusable and the list of what HA can call lives in one
 * readable place.
 *
 * Everything here was already reachable over MQTT under bridge/request. The
 * point is that ESPHome is the primary integration: without these, managing the
 * Zigbee network from Home Assistant meant falling back to MQTT for operations
 * the user performs by hand — pairing a device, removing one, re-running an
 * interview.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_gateway_services.h"
#include "esphome_services.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_leave_helper.h"
#include "zigbee/zb_interview.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ESPH_SVC";

/** Upper bound for permit-join, mirrors the Zigbee spec's 254s maximum. */
#define PERMIT_JOIN_MAX_SEC 254

/* ============================================================================
 * Helpers
 * ============================================================================ */

/**
 * @brief Resolve a device argument to its registry entry
 *
 * Accepts whatever the user typed into the Home Assistant dialog: a friendly
 * name or an IEEE address string. device_registry_find_by_id() handles both.
 */
static device_t *resolve_device(const esphome_service_arg_value_t *arg)
{
    if (arg == NULL || arg->type != ESPHOME_SERVICE_ARG_STRING) {
        return NULL;
    }
    if (arg->string_value[0] == '\0') {
        return NULL;
    }
    return device_registry_find_by_id(arg->string_value);
}

/* ============================================================================
 * Services
 * ============================================================================ */

/**
 * @brief permit_join(duration: int)
 *
 * Opens the network for new devices. Duration 0 closes it again, which is the
 * documented way to cancel pairing early.
 */
static esp_err_t svc_permit_join(const esphome_service_arg_value_t *args,
                                 size_t arg_count, void *user_data)
{
    (void)user_data;

    if (args == NULL || arg_count < 1 || args[0].type != ESPHOME_SERVICE_ARG_INT) {
        ESP_LOGW(TAG, "permit_join: expected one integer argument");
        return ESP_ERR_INVALID_ARG;
    }

    int32_t duration = args[0].int_value;
    if (duration < 0) {
        duration = 0;
    }
    if (duration > PERMIT_JOIN_MAX_SEC) {
        ESP_LOGW(TAG, "permit_join: clamping %ld s to the %d s maximum",
                 (long)duration, PERMIT_JOIN_MAX_SEC);
        duration = PERMIT_JOIN_MAX_SEC;
    }

    ESP_LOGI(TAG, "permit_join(%ld s) from Home Assistant", (long)duration);
    return zb_coordinator_permit_join((uint8_t)duration);
}

/**
 * @brief remove_device(device: string)
 *
 * Removes a device from the network and the registry. Takes a friendly name or
 * an IEEE address.
 */
static esp_err_t svc_remove_device(const esphome_service_arg_value_t *args,
                                   size_t arg_count, void *user_data)
{
    (void)user_data;

    if (args == NULL || arg_count < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    device_t *dev = resolve_device(&args[0]);
    if (dev == NULL) {
        ESP_LOGW(TAG, "remove_device: '%s' not found", args[0].string_value);
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy before the call: zb_device_leave_cleanup() removes the device, which
     * zeroes the registry slot this pointer refers to. */
    const device_id_t ieee = dev->id;
    const uint16_t short_addr =
        (dev->protocol == DEV_PROTOCOL_ZIGBEE) ? dev->proto.zigbee.short_addr : 0;

    ESP_LOGI(TAG, "remove_device('%s') -> 0x%016llX from Home Assistant",
             args[0].string_value, (unsigned long long)ieee);

    zb_device_leave_cleanup(ieee, short_addr);
    return ESP_OK;
}

/**
 * @brief reconfigure_device(device: string)
 *
 * Re-runs the interview: endpoints, clusters, manufacturer/model and the
 * converter binding derived from them. The fix for a device that joined badly,
 * or one whose converter has since been added to the database.
 */
static esp_err_t svc_reconfigure_device(const esphome_service_arg_value_t *args,
                                        size_t arg_count, void *user_data)
{
    (void)user_data;

    if (args == NULL || arg_count < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    device_t *dev = resolve_device(&args[0]);
    if (dev == NULL) {
        ESP_LOGW(TAG, "reconfigure_device: '%s' not found", args[0].string_value);
        return ESP_ERR_NOT_FOUND;
    }

    if (dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        ESP_LOGW(TAG, "reconfigure_device: '%s' is not a Zigbee device",
                 args[0].string_value);
        return ESP_ERR_INVALID_ARG;
    }

    /* zb_interview_start() talks to the radio and can block — copy first. */
    const device_id_t ieee = dev->id;
    const uint16_t short_addr = dev->proto.zigbee.short_addr;

    ESP_LOGI(TAG, "reconfigure_device('%s') -> 0x%04X from Home Assistant",
             args[0].string_value, short_addr);

    return zb_interview_start(ieee, short_addr);
}

/* ============================================================================
 * Registration
 * ============================================================================ */

/** Register one service, logging and propagating failure. */
static esp_err_t register_one(const char *name,
                              esphome_service_cb_t cb,
                              const char *arg_name,
                              esphome_service_arg_type_t arg_type)
{
    esphome_service_t svc = {
        .callback = cb,
        .user_data = NULL,
        .arg_count = 1,
        .key = 0,  /* auto-generated */
    };
    snprintf(svc.name, sizeof(svc.name), "%s", name);
    snprintf(svc.args[0].name, sizeof(svc.args[0].name), "%s", arg_name);
    svc.args[0].type = arg_type;

    esp_err_t ret = esphome_service_register(&svc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register service '%s': %s", name, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t esphome_gateway_services_register(void)
{
    esp_err_t ret = esphome_services_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Service manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_err_t first_error = ESP_OK;

    struct {
        const char *name;
        esphome_service_cb_t cb;
        const char *arg;
        esphome_service_arg_type_t type;
    } services[] = {
        {"permit_join",        svc_permit_join,        "duration", ESPHOME_SERVICE_ARG_INT},
        {"remove_device",      svc_remove_device,      "device",   ESPHOME_SERVICE_ARG_STRING},
        {"reconfigure_device", svc_reconfigure_device, "device",   ESPHOME_SERVICE_ARG_STRING},
    };

    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        esp_err_t r = register_one(services[i].name, services[i].cb,
                                   services[i].arg, services[i].type);
        if (r != ESP_OK && first_error == ESP_OK) {
            first_error = r;
        }
    }

    ESP_LOGI(TAG, "Registered %zu gateway services for Home Assistant",
             esphome_service_get_count());

    return first_error;
}
