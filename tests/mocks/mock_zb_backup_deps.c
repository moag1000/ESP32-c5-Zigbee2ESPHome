/**
 * @file mock_zb_backup_deps.c
 * @brief Stubs for everything zb_backup.c links against
 *
 * zb_backup_create() builds the backup entirely in memory — file I/O only
 * happens later in zb_backup_save() — so the collection logic is testable on
 * its own. What stands in the way is linking: the module references twelve
 * symbols across zb_coordinator, zb_network, zb_groups, zb_binding,
 * bridge_response and littlefs_mount, and pulling those in for real would drag
 * the Zigbee stack, MQTT and the filesystem into the test binary.
 *
 * The stubs report an empty, running system: no groups, no bindings, fixed
 * channel and PAN id. That leaves the device collection — the part these tests
 * are about — as the only thing that varies.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esp_err.h"
#include "zigbee/zb_groups.h"
#include "zigbee/zb_binding.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* --- zb_coordinator ------------------------------------------------------ */

bool zb_coordinator_is_running(void)
{
    return true;
}

/* --- zb_network ---------------------------------------------------------- */

uint16_t zb_network_get_pan_id_config(void)
{
    return 0x1A63;
}

uint8_t zb_network_get_channel_config(void)
{
    return 25;
}

/* --- zb_groups ----------------------------------------------------------- */

size_t zb_groups_get_all(zb_group_t *groups, size_t max_count)
{
    (void)groups;
    (void)max_count;
    return 0;
}

esp_err_t zb_groups_create(const char *name, uint16_t *group_id)
{
    (void)name;
    if (group_id != NULL) {
        *group_id = 0;
    }
    return ESP_OK;
}

esp_err_t zb_groups_add_member(uint16_t group_id, uint64_t ieee_addr)
{
    (void)group_id;
    (void)ieee_addr;
    return ESP_OK;
}

/* --- zb_binding ---------------------------------------------------------- */

size_t zb_binding_get_all(zb_binding_entry_t *entries, size_t max_count)
{
    (void)entries;
    (void)max_count;
    return 0;
}

esp_err_t zb_binding_create(uint64_t source_ieee, uint8_t source_ep,
                            uint16_t cluster_id,
                            uint64_t dest_ieee, uint8_t dest_ep)
{
    (void)source_ieee;
    (void)source_ep;
    (void)cluster_id;
    (void)dest_ieee;
    (void)dest_ep;
    return ESP_OK;
}

const char *zb_binding_get_cluster_name(uint16_t cluster_id)
{
    (void)cluster_id;
    return "mock";
}

/* --- bridge_response ----------------------------------------------------- */

esp_err_t bridge_response_publish_error(const char *response_topic,
                                        const char *error_message,
                                        const char *transaction_id)
{
    (void)response_topic;
    (void)error_message;
    (void)transaction_id;
    return ESP_OK;
}

/* --- littlefs ------------------------------------------------------------ */

esp_err_t littlefs_mount(void)
{
    return ESP_OK;
}

esp_err_t littlefs_unmount(void)
{
    return ESP_OK;
}
