# ESP-Zigbee-SDK Migration Guide: v1.5 → v1.6

## Überblick

Dieses Dokument beschreibt die notwendigen Änderungen für die Migration von ESP-Zigbee-SDK v1.5.x auf v1.6.x.

## Breaking Changes

### 1. esp_zigbee_cli entfernt
- **v1.5**: CLI Support verfügbar
- **v1.6**: Komplett entfernt
- **Aktion**: Nicht betroffen (CLI wurde nicht verwendet)

### 2. ZDO Callback Signaturen geändert
- `esp_zb_zdo_ieee_addr_req()` - Neue Callback-Signatur
- `esp_zb_zdo_nwk_addr_req()` - Neue Callback-Signatur
- **Aktion**: Callbacks in `zb_interview.c` prüfen

### 3. Read Report Config Response Struct
- `esp_zb_zcl_cmd_read_report_config_resp_message_t` redesigned
- **Aktion**: Struct-Zugriffe in `zb_reporting.c` prüfen

## Neue APIs (v1.6.8)

### Network Management
| API | Beschreibung |
|-----|-------------|
| `esp_zb_nwk_set_extended_pan_id()` | Extended PAN ID setzen |
| `esp_zb_nwk_get_extended_pan_id()` | Extended PAN ID lesen |

### Security
| API | Beschreibung |
|-----|-------------|
| `esp_zb_aps_set_authenticated()` | APS Auth State setzen |
| `esp_zb_aps_is_authenticated()` | APS Auth State prüfen |
| `esp_zb_secur_broadcast_network_key()` | Network Key broadcasten |
| `esp_zb_secur_broadcast_network_key_switch()` | Key Rotation |

### Power Management
| API | Beschreibung |
|-----|-------------|
| `esp_zb_set_node_descriptor_power_source()` | Power Source setzen |
| `esp_zb_set_node_power_descriptor()` | Power Descriptor setzen |
| `esp_zb_zdo_power_desc_req()` | Power Descriptor anfragen |

### ZCL Erweiterungen
| API | Beschreibung |
|-----|-------------|
| `esp_zb_zcl_scenes_table_set_size()` | Scene Table Größe |

## Neue ZCL Cluster (v1.6.7)

- Alarms Cluster (0x0009)
- Device Temperature Configuration (0x0002)
- Dehumidification Control (0x0203)
- Demand Response (0x0701)
- Poll Control, Binary Output/Value, Multistate I/O

## Thread-Safety Anforderungen

Ab v1.6.x ist Thread-Safety kritisch:

```c
// MQTT Task → Zigbee API
esp_zb_lock_acquire(portMAX_DELAY);
esp_err_t ret = esp_zb_zcl_on_off_cmd_req(&cmd_req);
esp_zb_lock_release();
```

## Action Handler Pattern

```c
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message) {
    switch (callback_id) {
        case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
            // Handle attribute reports
            break;
        default:
            break;
    }
    return ESP_OK;
}

// In init:
esp_zb_core_action_handler_register(zb_action_handler);
```

## Network Configuration (vor esp_zb_init)

```c
esp_zb_overall_network_size_set(100);
esp_zb_io_buffer_size_set(128);
esp_zb_aps_src_binding_table_size_set(32);
esp_zb_aps_dst_binding_table_size_set(32);
esp_zb_zcl_scenes_table_set_size(32);
```

## Referenzen

- [ESP-Zigbee-SDK GitHub](https://github.com/espressif/esp-zigbee-sdk)
- [Release Notes](https://github.com/espressif/esp-zigbee-sdk/blob/main/RELEASE_NOTES.md)
- [API Documentation](https://docs.espressif.com/projects/esp-zigbee-sdk/)
