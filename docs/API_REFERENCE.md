# API Reference

Complete API reference for all public modules in the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API).

## Table of Contents

- [Zigbee APIs](#zigbee-apis)
  - [Coordinator Management (zb_coordinator.h)](#coordinator-management)
  - [Network Management (zb_network.h)](#network-management)
  - [Device Handler (zb_device_handler.h)](#device-handler)
  - [Device Interview (zb_interview.h)](#device-interview)
  - [Binding Management (zb_binding.h)](#binding-management)
  - [Group Management (zb_groups.h)](#group-management)
  - [Scene Management (zb_scenes.h)](#scene-management)
  - [OTA Updates (zb_ota.h)](#ota-updates)
  - [Diagnostics (zb_diagnostics.h)](#diagnostics)
- [MQTT APIs](#mqtt-apis)
  - [MQTT Client (gateway_mqtt.h)](#mqtt-client)
  - [Topic Patterns (mqtt_topics.h)](#topic-patterns)
- [ESPHome APIs](#esphome-apis)
  - [ESPHome API Server (esphome_api.h)](#esphome-api-server)
  - [Entity Management (esphome_entities.h)](#entity-management)
  - [BLE Proxy (esphome_ble_proxy.h)](#ble-proxy)
- [Core APIs](#core-apis)
  - [Configuration Manager (config_manager.h)](#configuration-manager)
  - [Home Assistant Discovery (ha_discovery.h)](#home-assistant-discovery)
  - [Bridge Events (bridge_events.h)](#bridge-events)
  - [MQTT Bridge (mqtt_bridge.h)](#mqtt-bridge)
  - [System Monitor](#system-monitor)
- [WiFi Manager API](#wifi-manager-api)
- [OTA Handler API](#ota-handler-api)
- [Utility APIs](#utility-apis)
- [Error Handling](#error-handling)
- [Thread Safety](#thread-safety)

---

## Zigbee APIs

### Coordinator Management

**Header:** `main/zigbee/zb_coordinator.h`

The coordinator module provides core Zigbee coordinator functionality including stack initialization, network formation, and device management.

#### Types

```c
typedef enum {
    ZB_COORD_STATE_UNINITIALIZED = 0,  // Coordinator not initialized
    ZB_COORD_STATE_INITIALIZED,        // Initialized but not started
    ZB_COORD_STATE_STARTING,           // Starting up
    ZB_COORD_STATE_RUNNING,            // Running and network formed
    ZB_COORD_STATE_ERROR,              // Error state
    ZB_COORD_STATE_STOPPED             // Stopped
} zb_coordinator_state_t;

typedef struct {
    bool enabled;                   // Permit join is currently enabled
    uint8_t initial_duration;       // Initial duration requested (seconds)
    uint8_t remaining_time;         // Remaining time (seconds), 255=forever
    bool has_target_device;         // True if permit join is for specific device
    uint8_t target_ieee_addr[8];    // Target device IEEE address
    int64_t start_time_us;          // Start time in microseconds
} zb_permit_join_state_t;

typedef struct {
    uint8_t duration;               // Duration in seconds (0=close, 254=default, 255=forever)
    bool has_target_device;         // True to permit join for specific device only
    uint8_t target_ieee_addr[8];    // Target device IEEE address
} zb_permit_join_options_t;

typedef struct {
    uint16_t pan_id;                // Network PAN ID
    uint8_t channel;                // Current Zigbee channel (11-26)
    uint8_t transmit_power;         // TX power level (dBm)
    bool permit_join_default;       // Default permit join state on startup
    uint8_t max_children;           // Maximum number of child devices
    uint8_t extended_pan_id[8];     // Extended PAN ID (64-bit)
    uint8_t network_key[16];        // Network encryption key (masked in responses)
    uint8_t network_update_id;      // Network update identifier
    bool network_formed;            // True if network is formed
} zb_coordinator_settings_t;

typedef struct {
    char type[32];                  // Coordinator type (e.g., "ESP32-C5")
    char firmware_version[32];      // Firmware version string
    char firmware_build[32];        // Firmware build date/revision
    char zigbee_version[16];        // Zigbee stack version
    char ieee_address[24];          // Coordinator IEEE address string
    uint16_t short_address;         // Coordinator short address
} zb_coordinator_version_t;

typedef struct {
    bool healthy;                   // Overall health status
    bool network_up;                // Network is formed and operational
    bool zigbee_stack_running;      // Zigbee stack is running
    uint32_t uptime_seconds;        // Uptime since coordinator started
    uint32_t joined_devices;        // Number of devices currently joined
    uint32_t message_count_tx;      // Total messages transmitted
    uint32_t message_count_rx;      // Total messages received
    int8_t last_rssi;               // Last received signal strength
    uint8_t last_lqi;               // Last link quality indicator
} zb_coordinator_health_t;

typedef struct {
    uint8_t major;                  // Major version number
    uint8_t minor;                  // Minor version number
    uint8_t patch;                  // Patch version number
    char version_string[16];        // Version string (e.g., "1.6.8")
    uint8_t min_major;              // Minimum required major version
    uint8_t min_minor;              // Minimum required minor version
    uint8_t min_patch;              // Minimum required patch version
    bool meets_minimum;             // True if SDK meets minimum requirements
} zb_sdk_version_t;
```

#### Constants

```c
#define ZB_PERMIT_JOIN_DEFAULT_DURATION     254     // Default permit join duration
#define ZB_COORDINATOR_MAX_CHILDREN         32      // Default max child devices
#define ZB_PERMIT_JOIN_FOREVER              255     // Permit join forever (no timeout)
#define ZB_PERMIT_JOIN_UPDATE_INTERVAL      10      // Timer update interval (seconds)
```

#### Functions

##### zb_coordinator_init
```c
esp_err_t zb_coordinator_init(void);
```
Initialize the Zigbee coordinator. Configures the coordinator role and prepares for network formation.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails
- `ESP_ERR_INVALID_STATE` if already initialized

**Thread Safety:** Must be called from main task before starting other tasks.

**Example:**
```c
esp_err_t ret = zb_coordinator_init();
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Zigbee coordinator initialized");
}
```

---

##### zb_coordinator_start
```c
esp_err_t zb_coordinator_start(void);
```
Start the Zigbee coordinator. Forms a network and starts accepting device joins. Creates the coordinator task.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized

**Thread Safety:** Must be called after `zb_coordinator_init()`.

---

##### zb_coordinator_stop
```c
esp_err_t zb_coordinator_stop(void);
```
Stop the Zigbee coordinator. Closes the network and disconnects all devices.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not running

---

##### zb_coordinator_is_running
```c
bool zb_coordinator_is_running(void);
```
Check if coordinator is operational.

**Returns:** `true` if running, `false` otherwise.

---

##### zb_coordinator_get_state
```c
zb_coordinator_state_t zb_coordinator_get_state(void);
```
Get current coordinator state.

**Returns:** Current coordinator state enum value.

---

##### zb_coordinator_permit_join
```c
esp_err_t zb_coordinator_permit_join(uint8_t duration);
```
Open the network for new device joins.

**Parameters:**
- `duration`: Duration in seconds (0=close, 1-254=timeout, 255=forever)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if coordinator not running
- `ESP_FAIL` on Zigbee stack error

**Thread Safety:** Thread-safe, can be called from any task.

**Example:**
```c
// Enable pairing for 3 minutes
zb_coordinator_permit_join(180);

// Open network forever
zb_coordinator_permit_join(255);

// Close network
zb_coordinator_permit_join(0);
```

---

##### zb_coordinator_permit_join_with_options
```c
esp_err_t zb_coordinator_permit_join_with_options(const zb_permit_join_options_t *options);
```
Open the network with extended options including timer management and optional target device.

**Parameters:**
- `options`: Permit join options (duration, target device)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if options is NULL

---

##### zb_coordinator_get_permit_join_state
```c
esp_err_t zb_coordinator_get_permit_join_state(zb_permit_join_state_t *state);
```
Get current permit join state including remaining time.

**Parameters:**
- `state`: Output structure for current state

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if state is NULL

---

##### zb_coordinator_get_settings
```c
esp_err_t zb_coordinator_get_settings(zb_coordinator_settings_t *settings);
```
Retrieve current coordinator configuration and network settings.

**Parameters:**
- `settings`: Output structure for settings

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if settings is NULL
- `ESP_ERR_INVALID_STATE` if coordinator not initialized

---

##### zb_coordinator_get_version
```c
esp_err_t zb_coordinator_get_version(zb_coordinator_version_t *version);
```
Retrieve coordinator type, firmware version, and Zigbee stack info.

**Parameters:**
- `version`: Output structure for version info

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if version is NULL

---

##### zb_coordinator_health_check
```c
esp_err_t zb_coordinator_health_check(zb_coordinator_health_t *health);
```
Perform coordinator health check including network state and statistics.

**Parameters:**
- `health`: Output structure for health status

**Returns:**
- `ESP_OK` on success (healthy)
- `ESP_ERR_INVALID_ARG` if health is NULL
- `ESP_FAIL` if health check fails (unhealthy)

---

##### zb_coordinator_get_sdk_version
```c
esp_err_t zb_coordinator_get_sdk_version(zb_sdk_version_t *version);
```
Get ESP-Zigbee-SDK version information for runtime feature detection.

**Parameters:**
- `version`: Output structure for SDK version info

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if version is NULL

---

##### zb_coordinator_update_signal_quality
```c
void zb_coordinator_update_signal_quality(int8_t rssi, uint8_t lqi);
```
Update signal quality metrics. Called when a message is received with signal quality info.

**Parameters:**
- `rssi`: Received signal strength indicator (dBm)
- `lqi`: Link quality indicator (0-255)

---

### Network Management

**Header:** `main/zigbee/zb_network.h`

Manages Zigbee network configuration, persistence, and network-level operations including Extended PAN ID management, key rotation, and channel changes.

#### Constants

```c
#define ZB_NETWORK_KEY_LEN              16      // Network key length in bytes
#define ZB_DEFAULT_PAN_ID               0x1A62  // Default PAN ID
#define ZB_DEFAULT_CHANNEL              11      // Default radio channel (11-26)
#define ZB_DEFAULT_MAX_CHILDREN         20      // Default max child devices
#define ZB_DEFAULT_TX_POWER             20      // Default TX power (dBm)
#define ZB_DEFAULT_PERMIT_JOIN_DURATION_SEC 180 // Default permit join (3 minutes)
#define ZB_EXTENDED_PAN_ID_LEN          8       // Extended PAN ID length
```

#### Types

```c
typedef struct {
    uint16_t pan_id;                        // PAN ID
    uint8_t channel;                        // Radio channel (11-26)
    uint8_t extended_pan_id[8];             // Extended PAN ID
    uint8_t network_key[ZB_NETWORK_KEY_LEN]; // Network encryption key
    bool permit_join;                       // Permit join status
    uint8_t device_count;                   // Number of joined devices
    uint16_t short_addr;                    // Coordinator short address (0x0000)
    uint8_t depth;                          // Network depth
    bool network_formed;                    // Network formation status
} zb_network_info_t;

typedef enum {
    ZB_CHANNEL_CHANGE_IDLE = 0,      // No channel change in progress
    ZB_CHANNEL_CHANGE_PENDING,        // Channel change requested, waiting
    ZB_CHANNEL_CHANGE_NOTIFYING,      // Notifying devices of change
    ZB_CHANNEL_CHANGE_SWITCHING,      // Switching to new channel
    ZB_CHANNEL_CHANGE_COMPLETE,       // Channel change completed
    ZB_CHANNEL_CHANGE_FAILED          // Channel change failed
} zb_channel_change_state_t;

typedef void (*zb_channel_change_cb_t)(esp_err_t result, uint8_t old_channel,
                                       uint8_t new_channel, void *user_data);
```

#### Functions

##### zb_network_init
```c
esp_err_t zb_network_init(void);
```
Initialize network manager and load stored configuration from NVS.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails

---

##### zb_network_get_info
```c
esp_err_t zb_network_get_info(zb_network_info_t *info);
```
Retrieve current network configuration and status.

**Parameters:**
- `info`: Pointer to network info structure

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if info is NULL
- `ESP_ERR_INVALID_STATE` if network not formed

---

##### zb_network_save_config
```c
esp_err_t zb_network_save_config(void);
```
Persist current network configuration to NVS for restoration after reboot.

**Returns:**
- `ESP_OK` on success
- `ESP_FAIL` on NVS error

---

##### zb_network_reset
```c
esp_err_t zb_network_reset(void);
```
Erase stored network configuration and force creation of a new network.

**Warning:** This is a destructive operation! All devices will need to re-pair.

**Returns:**
- `ESP_OK` on success
- `ESP_FAIL` on error

---

##### zb_network_change_channel
```c
esp_err_t zb_network_change_channel(uint8_t new_channel,
                                    zb_channel_change_cb_t callback,
                                    void *user_data);
```
Initiate a network-wide channel change. Sends `Mgmt_NWK_Update_req` to all devices.

**Parameters:**
- `new_channel`: New channel (11-26)
- `callback`: Completion callback (can be NULL)
- `user_data`: User data for callback

**Returns:**
- `ESP_OK` if channel change initiated
- `ESP_ERR_INVALID_ARG` if channel is invalid
- `ESP_ERR_INVALID_STATE` if change already in progress
- `ESP_ERR_NOT_SUPPORTED` if network not formed

**Warning:** This operation may cause temporary network disruption.

**Example:**
```c
void channel_change_done(esp_err_t result, uint8_t old_ch, uint8_t new_ch, void *data) {
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Channel changed from %d to %d", old_ch, new_ch);
    } else {
        ESP_LOGE(TAG, "Channel change failed");
    }
}

esp_err_t ret = zb_network_change_channel(15, channel_change_done, NULL);
```

---

##### zb_network_validate_channel
```c
bool zb_network_validate_channel(uint8_t channel);
```
Check if channel is valid (11-26 for 2.4GHz Zigbee).

**Parameters:**
- `channel`: Channel to validate

**Returns:** `true` if channel is valid, `false` otherwise.

---

##### zb_network_get_extended_pan_id
```c
esp_err_t zb_network_get_extended_pan_id(uint8_t ext_pan_id[ZB_EXTENDED_PAN_ID_LEN]);
```
Get the current Extended PAN ID from the Zigbee stack.

**Parameters:**
- `ext_pan_id`: Buffer to store Extended PAN ID (8 bytes)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if ext_pan_id is NULL

---

##### zb_network_set_extended_pan_id
```c
esp_err_t zb_network_set_extended_pan_id(const uint8_t ext_pan_id[ZB_EXTENDED_PAN_ID_LEN]);
```
Set a new Extended PAN ID. Should only be called before network formation.

**Warning:** Changing the Extended PAN ID on an active network will cause all devices to lose connectivity.

---

##### zb_network_format_extended_pan_id
```c
esp_err_t zb_network_format_extended_pan_id(const uint8_t ext_pan_id[ZB_EXTENDED_PAN_ID_LEN],
                                             char *str_buf, size_t buf_len);
```
Format Extended PAN ID as hex string for display/logging. Format: "0x0123456789ABCDEF"

**Parameters:**
- `ext_pan_id`: Extended PAN ID (8 bytes)
- `str_buf`: Output string buffer (min 19 bytes: "0x" + 16 hex + null)
- `buf_len`: Buffer length

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if buffer too small

---

##### zb_network_rotate_key
```c
esp_err_t zb_network_rotate_key(uint32_t *sequence);
```
Initiate network-wide key rotation by broadcasting a new network key.

**Parameters:**
- `sequence`: Optional output for the new sequence number

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if network not formed
- `ESP_FAIL` if key broadcast fails

**Warning:** Key rotation is a sensitive security operation. All devices must successfully receive the new key or they will lose network access.

---

### Device Handler

**Header:** `main/zigbee/zb_device_handler.h`

Manages the registry of Zigbee devices connected to the coordinator, tracks device state, and handles join/leave events.

#### Constants

```c
#define ZB_MAX_DEVICES                  50  // Maximum devices supported
#define ZB_DEVICE_FRIENDLY_NAME_LEN     32  // Max friendly name length
#define ZB_DEVICE_MODEL_LEN             32  // Max model name length
#define ZB_DEVICE_MANUFACTURER_LEN      32  // Max manufacturer name length

// State array limits for specific device types
#define ZB_STATE_MAX_DOOR_LOCK          8   // Max door lock devices tracked
#define ZB_STATE_MAX_COVER              8   // Max cover devices tracked
#define ZB_STATE_MAX_THERMOSTAT         8   // Max thermostat devices tracked
```

#### Types

```c
typedef enum {
    ZB_DEVICE_TYPE_UNKNOWN = 0,
    ZB_DEVICE_TYPE_ON_OFF_LIGHT,
    ZB_DEVICE_TYPE_DIMMABLE_LIGHT,
    ZB_DEVICE_TYPE_COLOR_LIGHT,
    ZB_DEVICE_TYPE_ON_OFF_SWITCH,
    ZB_DEVICE_TYPE_TEMP_SENSOR,
    ZB_DEVICE_TYPE_HUMIDITY_SENSOR,
    ZB_DEVICE_TYPE_MOTION_SENSOR,
    ZB_DEVICE_TYPE_DOOR_SENSOR,
    ZB_DEVICE_TYPE_PLUG,
    ZB_DEVICE_TYPE_WINDOW_COVERING,
    ZB_DEVICE_TYPE_DOOR_LOCK,
    ZB_DEVICE_TYPE_THERMOSTAT,
    ZB_DEVICE_TYPE_FAN,
    // ... additional types
} zb_device_type_t;

typedef struct {
    esp_zb_ieee_addr_t ieee_addr;               // IEEE 64-bit address
    uint16_t short_addr;                        // Network short address
    uint8_t endpoint;                           // Primary endpoint
    char friendly_name[ZB_DEVICE_FRIENDLY_NAME_LEN];
    char model[ZB_DEVICE_MODEL_LEN];
    char manufacturer[ZB_DEVICE_MANUFACTURER_LEN];
    zb_device_type_t device_type;
    bool online;                                // Online status
    time_t last_seen;                           // Last activity timestamp
    uint8_t link_quality;                       // Link quality (LQI)
    uint8_t rssi;                               // Signal strength
    uint16_t cluster_count;                     // Number of supported clusters
    uint16_t clusters[32];                      // Supported cluster IDs
} zb_device_t;
```

#### Functions

##### zb_device_handler_init
```c
esp_err_t zb_device_handler_init(void);
```
Initialize the device registry and associated resources.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails

---

##### zb_device_add
```c
esp_err_t zb_device_add(esp_zb_ieee_addr_t ieee_addr, uint16_t short_addr);
```
Add a new device to the registry. If device exists, updates the short address.

**Parameters:**
- `ieee_addr`: IEEE 64-bit address
- `short_addr`: Network short address

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if device registry is full

---

##### zb_device_remove
```c
esp_err_t zb_device_remove(uint16_t short_addr);
```
Remove device from registry.

**Parameters:**
- `short_addr`: Network short address

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if device not found

---

##### zb_device_get
```c
zb_device_t* zb_device_get(uint16_t short_addr);
```
Get device by short address.

**Parameters:**
- `short_addr`: Network short address

**Returns:** Pointer to device structure or NULL if not found.

**Thread Safety:** Pointer is valid until device is removed.

---

##### zb_device_get_by_ieee
```c
zb_device_t* zb_device_get_by_ieee(esp_zb_ieee_addr_t ieee_addr);
```
Get device by IEEE address.

**Parameters:**
- `ieee_addr`: IEEE 64-bit address

**Returns:** Pointer to device structure or NULL if not found.

---

##### zb_device_get_by_name
```c
zb_device_t* zb_device_get_by_name(const char *friendly_name);
```
Get device by friendly name.

**Parameters:**
- `friendly_name`: Device friendly name

**Returns:** Pointer to device structure or NULL if not found.

---

##### zb_device_set_friendly_name
```c
esp_err_t zb_device_set_friendly_name(uint16_t short_addr, const char *name);
```
Set a user-friendly name for the device.

**Parameters:**
- `short_addr`: Network short address
- `name`: Friendly name (max ZB_DEVICE_FRIENDLY_NAME_LEN-1 chars)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if device not found

---

##### zb_device_get_count
```c
size_t zb_device_get_count(void);
```
Get number of registered devices.

**Returns:** Number of devices in registry.

---

##### zb_device_get_all
```c
size_t zb_device_get_all(zb_device_t *devices, size_t max_count);
```
Get all registered devices.

**Parameters:**
- `devices`: Array to store device copies
- `max_count`: Maximum entries to copy

**Returns:** Number of devices copied.

---

### Device Interview

**Header:** `main/zigbee/zb_interview.h`

Provides device interview functionality to discover all endpoints, clusters, and attributes of newly joined Zigbee devices.

#### Constants

```c
#define ZB_INTERVIEW_MAX_ENDPOINTS      32      // Max endpoints per device
#define ZB_INTERVIEW_MAX_CLUSTERS       64      // Max clusters per endpoint
#define ZB_INTERVIEW_MAX_ATTRIBUTES     32      // Max attributes per cluster
#define ZB_INTERVIEW_MAX_CONCURRENT     5       // Max concurrent interviews
#define ZB_INTERVIEW_TIMEOUT_MS         30000   // Interview timeout (ms)
#define ZB_INTERVIEW_RETRY_COUNT        3       // Interview retry count
```

#### Types

```c
typedef enum {
    ZB_INTERVIEW_STATUS_IDLE = 0,
    ZB_INTERVIEW_STATUS_ACTIVE_ENDPOINTS,   // Discovering active endpoints
    ZB_INTERVIEW_STATUS_SIMPLE_DESC,        // Reading simple descriptor
    ZB_INTERVIEW_STATUS_ATTRIBUTES,         // Reading attribute list
    ZB_INTERVIEW_STATUS_BASIC_INFO,         // Reading basic cluster info
    ZB_INTERVIEW_STATUS_COMPLETE,           // Interview completed
    ZB_INTERVIEW_STATUS_FAILED,             // Interview failed
    ZB_INTERVIEW_STATUS_TIMEOUT             // Interview timed out
} zb_interview_status_t;

typedef struct {
    uint8_t endpoint;                       // Endpoint number
    uint16_t profile_id;                    // Profile ID (e.g., HA=0x0104)
    uint16_t device_id;                     // Device ID
    uint8_t device_version;                 // Device version
    uint16_t *server_clusters;              // Array of server cluster IDs
    uint8_t server_cluster_count;           // Number of server clusters
    uint16_t *client_clusters;              // Array of client cluster IDs
    uint8_t client_cluster_count;           // Number of client clusters
} zb_endpoint_info_t;

typedef struct {
    uint64_t ieee_addr;                 // Device IEEE address (64-bit)
    uint16_t short_addr;                // Device short address
    zb_endpoint_info_t *endpoints;      // Array of endpoint information
    uint8_t endpoint_count;             // Number of endpoints
    char manufacturer[33];              // Manufacturer name (from Basic cluster)
    char model[33];                     // Model identifier (from Basic cluster)
    uint8_t sw_version;                 // Software version (from Basic cluster)
    zb_interview_status_t status;       // Final interview status
    bool interview_complete;            // Interview completion flag
    uint32_t duration_ms;               // Interview duration in milliseconds
} zb_interview_result_t;

typedef void (*zb_interview_complete_cb_t)(const zb_interview_result_t *result);
typedef void (*zb_interview_progress_cb_t)(uint64_t ieee_addr,
                                           zb_interview_status_t status,
                                           uint8_t progress);

typedef struct {
    zb_interview_complete_cb_t complete_cb; // Completion callback (required)
    zb_interview_progress_cb_t progress_cb; // Progress callback (optional)
    uint32_t timeout_ms;                    // Timeout in ms (0=default)
    uint8_t retry_count;                    // Retry count (0=default)
    bool read_attributes;                   // Also read attribute lists
    bool auto_interview_on_join;            // Auto-start on device join
} zb_interview_config_t;
```

#### Functions

##### zb_interview_init
```c
esp_err_t zb_interview_init(const zb_interview_config_t *config);
```
Initialize the interview module with the specified configuration.

**Parameters:**
- `config`: Interview configuration (NULL for defaults)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails
- `ESP_ERR_INVALID_STATE` if already initialized

**Example:**
```c
void on_interview_complete(const zb_interview_result_t *result) {
    if (result->status == ZB_INTERVIEW_STATUS_COMPLETE) {
        ESP_LOGI(TAG, "Device %s - %s interviewed in %lu ms",
                 result->manufacturer, result->model, result->duration_ms);
    }
}

zb_interview_config_t config = {
    .complete_cb = on_interview_complete,
    .auto_interview_on_join = true,
    .timeout_ms = 30000,
};
zb_interview_init(&config);
```

---

##### zb_interview_start
```c
esp_err_t zb_interview_start(uint64_t ieee_addr, uint16_t short_addr);
```
Start the interview process for a device.

**Parameters:**
- `ieee_addr`: Device IEEE address (64-bit)
- `short_addr`: Device short address

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not initialized
- `ESP_ERR_NO_MEM` if max concurrent interviews reached

---

##### zb_interview_cancel
```c
esp_err_t zb_interview_cancel(uint64_t ieee_addr);
```
Cancel an in-progress interview for the specified device.

**Parameters:**
- `ieee_addr`: Device IEEE address

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if no interview in progress

---

##### zb_interview_get_result
```c
const zb_interview_result_t* zb_interview_get_result(uint64_t ieee_addr);
```
Retrieve the cached interview result for a device.

**Parameters:**
- `ieee_addr`: Device IEEE address

**Returns:** Pointer to interview result, or NULL if not found.

---

##### zb_interview_is_active
```c
bool zb_interview_is_active(uint64_t ieee_addr);
```
Check if interview is in progress for a device.

**Returns:** `true` if interview is active, `false` otherwise.

---

### Binding Management

**Header:** `main/zigbee/zb_binding.h`

Provides Zigbee binding management for direct device-to-device communication without coordinator intervention.

#### MQTT Topics
- `zigbee2mqtt/bridge/request/device/bind` - Create binding
- `zigbee2mqtt/bridge/request/device/unbind` - Remove binding
- `zigbee2mqtt/bridge/response/device/bind` - Bind response
- `zigbee2mqtt/bridge/response/device/unbind` - Unbind response

#### Constants

```c
#define ZB_BINDING_MAX_ENTRIES      32  // Maximum binding entries
#define ZB_BINDING_NVS_NAMESPACE    "zb_binding"
#define ZB_BINDING_NAME_MAX_LEN     64  // Maximum friendly name length
```

#### Types

```c
typedef enum {
    ZB_BINDING_STATUS_SUCCESS = 0,
    ZB_BINDING_STATUS_NOT_SUPPORTED,
    ZB_BINDING_STATUS_TABLE_FULL,
    ZB_BINDING_STATUS_NO_ENTRY,
    ZB_BINDING_STATUS_TIMEOUT,
    ZB_BINDING_STATUS_INVALID_EP,
    ZB_BINDING_STATUS_DEVICE_NOT_FOUND,
    ZB_BINDING_STATUS_ERROR
} zb_binding_status_t;

typedef struct {
    uint64_t source_ieee;       // Source device IEEE address (64-bit)
    uint8_t source_endpoint;    // Source device endpoint
    uint16_t cluster_id;        // Cluster ID to bind
    uint64_t dest_ieee;         // Destination device IEEE address (64-bit)
    uint8_t dest_endpoint;      // Destination device endpoint
    bool active;                // Entry is in use
} zb_binding_entry_t;

typedef void (*zb_binding_event_cb_t)(zb_binding_event_type_t event,
                                       const zb_binding_entry_t *entry,
                                       zb_binding_status_t status);

typedef struct {
    bool success;                               // Operation success status
    zb_binding_status_t status;                 // Detailed status code
    char source_name[ZB_BINDING_NAME_MAX_LEN];  // Source device name/IEEE
    char dest_name[ZB_BINDING_NAME_MAX_LEN];    // Destination device name/IEEE
    uint16_t cluster_id;                        // Cluster that was bound
    char error_message[128];                    // Error message if failed
} zb_binding_result_t;
```

#### Functions

##### zb_binding_init
```c
esp_err_t zb_binding_init(void);
```
Initialize binding management, allocate resources, and load existing bindings from NVS.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if memory allocation fails
- `ESP_ERR_INVALID_STATE` if already initialized

---

##### zb_binding_create
```c
esp_err_t zb_binding_create(uint64_t source_ieee, uint8_t source_ep,
                            uint16_t cluster_id,
                            uint64_t dest_ieee, uint8_t dest_ep);
```
Create a binding between source and destination devices for the specified cluster.

**Parameters:**
- `source_ieee`: Source device IEEE address
- `source_ep`: Source device endpoint
- `cluster_id`: Cluster ID to bind
- `dest_ieee`: Destination device IEEE address
- `dest_ep`: Destination device endpoint

**Returns:**
- `ESP_OK` on success (request sent, not yet confirmed)
- `ESP_ERR_NO_MEM` if binding table is full
- `ESP_ERR_NOT_FOUND` if source device not found

**Example:**
```c
// Bind a switch to control a light via On/Off cluster
esp_err_t ret = zb_binding_create(
    switch_ieee, 1,                     // Source: switch endpoint 1
    ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,       // Cluster: On/Off
    light_ieee, 1                        // Dest: light endpoint 1
);
```

---

##### zb_binding_remove
```c
esp_err_t zb_binding_remove(uint64_t source_ieee, uint8_t source_ep,
                            uint16_t cluster_id,
                            uint64_t dest_ieee, uint8_t dest_ep);
```
Remove an existing binding.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if binding not found

---

##### zb_binding_remove_device
```c
esp_err_t zb_binding_remove_device(uint64_t ieee_addr);
```
Remove all bindings where the device is either source or destination.

**Parameters:**
- `ieee_addr`: Device IEEE address

---

##### zb_binding_get_count
```c
size_t zb_binding_get_count(void);
```
Get number of active bindings.

**Returns:** Number of active bindings.

---

##### zb_binding_get_cluster_name
```c
const char* zb_binding_get_cluster_name(uint16_t cluster_id);
```
Get human-readable cluster name for MQTT messages.

**Parameters:**
- `cluster_id`: Cluster ID

**Returns:** Cluster name string (e.g., "genOnOff", "genLevelCtrl").

---

##### zb_binding_process_mqtt_request
```c
esp_err_t zb_binding_process_mqtt_request(const char *topic, const char *payload, size_t len);
```
Process MQTT bind/unbind request.

**Expected JSON payload format:**
```json
{
  "from": "device_name_or_ieee",
  "to": "device_name_or_ieee",
  "clusters": ["genOnOff", "genLevelCtrl"]
}
```

---

### Group Management

**Header:** `main/zigbee/zb_groups.h`

Provides Zigbee group management for controlling multiple devices simultaneously.

#### MQTT Topics
- `zigbee2mqtt/bridge/request/group/add` - Create group
- `zigbee2mqtt/bridge/request/group/remove` - Delete group
- `zigbee2mqtt/bridge/request/group/members/add` - Add device to group
- `zigbee2mqtt/bridge/request/group/members/remove` - Remove device from group
- `zigbee2mqtt/group/[name]/set` - Send command to group

#### Constants

```c
#define ZB_GROUPS_MAX_COUNT     16      // Maximum groups supported
#define ZB_GROUP_MAX_MEMBERS    32      // Maximum members per group
#define ZB_GROUP_NAME_MAX_LEN   32      // Maximum group name length
```

#### Functions

##### zb_groups_init
```c
esp_err_t zb_groups_init(void);
```
Initialize group management module.

---

##### zb_groups_create
```c
esp_err_t zb_groups_create(const char *name, uint16_t *group_id);
```
Create a new Zigbee group.

**Parameters:**
- `name`: Group friendly name
- `group_id`: Output: assigned group ID (can be NULL)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if maximum groups reached

---

##### zb_groups_delete
```c
esp_err_t zb_groups_delete(uint16_t group_id);
```
Delete a group and remove all members.

---

##### zb_groups_add_member
```c
esp_err_t zb_groups_add_member(uint16_t group_id, uint64_t ieee_addr);
```
Add a device to a group.

**Parameters:**
- `group_id`: Group ID
- `ieee_addr`: Device IEEE address (64-bit)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if group not found
- `ESP_ERR_NO_MEM` if group is full

---

##### zb_groups_remove_member
```c
esp_err_t zb_groups_remove_member(uint16_t group_id, uint64_t ieee_addr);
```
Remove a device from a group.

---

##### zb_groups_send_on_off
```c
esp_err_t zb_groups_send_on_off(uint16_t group_id, bool on);
```
Send On/Off command to all devices in a group.

**Parameters:**
- `group_id`: Group ID
- `on`: true for ON, false for OFF

---

##### zb_groups_send_level
```c
esp_err_t zb_groups_send_level(uint16_t group_id, uint8_t level, uint16_t transition_time);
```
Send Level command to all devices in a group.

**Parameters:**
- `group_id`: Group ID
- `level`: Level value (0-254)
- `transition_time`: Transition time in 1/10 seconds

---

### Scene Management

**Header:** `main/zigbee/zb_scenes.h`

Provides Zigbee scene management for storing and recalling device state combinations.

#### MQTT Topics
- `zigbee2mqtt/bridge/request/scene/add` - Create scene
- `zigbee2mqtt/bridge/request/scene/remove` - Delete scene
- `zigbee2mqtt/bridge/request/scene/store` - Store current state in scene
- `zigbee2mqtt/bridge/request/scene/recall` - Recall scene

#### Constants

```c
#define ZB_SCENES_MAX_COUNT         16      // Maximum scenes
#define ZB_SCENE_MAX_DEVICES        16      // Max devices per scene
#define ZB_SCENE_NAME_MAX_LEN       32      // Maximum scene name length
```

#### Types

```c
typedef struct {
    uint64_t ieee_addr;         // Device IEEE address
    uint8_t endpoint;           // Device endpoint
    bool on_off_state;          // On/Off state
    uint8_t level;              // Level (brightness 0-254)
    uint16_t color_x;           // Color X coordinate
    uint16_t color_y;           // Color Y coordinate
    uint16_t color_temp;        // Color temperature in mireds
} zb_scene_device_state_t;

typedef struct {
    uint8_t scene_id;           // Scene ID (1-254)
    uint16_t group_id;          // Associated group ID
    char name[ZB_SCENE_NAME_MAX_LEN];
    zb_scene_device_state_t devices[ZB_SCENE_MAX_DEVICES];
    uint8_t device_count;
    uint16_t transition_time;   // Transition time in 1/10 seconds
    bool active;
} zb_scene_t;
```

#### Functions

##### zb_scenes_init
```c
esp_err_t zb_scenes_init(void);
```
Initialize scene management module.

---

##### zb_scenes_create
```c
esp_err_t zb_scenes_create(uint16_t group_id, const char *name, uint8_t *scene_id);
```
Create a new scene associated with a group.

**Parameters:**
- `group_id`: Group ID (0 for no group)
- `name`: Scene friendly name
- `scene_id`: Output: assigned scene ID (can be NULL)

---

##### zb_scenes_store
```c
esp_err_t zb_scenes_store(uint8_t scene_id);
```
Capture current state of all devices in the associated group and store in the scene.

---

##### zb_scenes_recall
```c
esp_err_t zb_scenes_recall(uint8_t scene_id);
```
Recall a scene and apply stored states to all devices.

---

### OTA Updates

**Header:** `main/zigbee/zb_ota.h`

Implements Zigbee OTA (Over-The-Air) upgrade server functionality for updating end device firmware.

#### MQTT Topics
- `zigbee2mqtt/bridge/request/ota/update/{ieee}` - Trigger OTA for device
- `zigbee2mqtt/bridge/request/ota/check/{ieee}` - Check for device updates
- `zigbee2mqtt/bridge/ota/images` - List available OTA images
- `zigbee2mqtt/bridge/ota/status/{ieee}` - OTA progress status

#### Constants

```c
#define ZB_OTA_CLUSTER_ID               0x0019  // OTA Upgrade Cluster ID
#define ZB_OTA_MAX_IMAGES               8       // Max stored images
#define ZB_OTA_MAX_TRANSFERS            4       // Max simultaneous transfers
#define ZB_OTA_DEFAULT_BLOCK_SIZE       64      // Default block size (bytes)
#define ZB_OTA_MAX_IMAGE_SIZE           (512 * 1024)  // Max image size
```

#### Types

```c
typedef enum {
    ZB_OTA_TRANSFER_IDLE = 0,
    ZB_OTA_TRANSFER_QUERY,              // Device queried for image
    ZB_OTA_TRANSFER_DOWNLOADING,        // Transfer in progress
    ZB_OTA_TRANSFER_COMPLETE,           // Transfer complete
    ZB_OTA_TRANSFER_UPGRADING,          // Device is upgrading
    ZB_OTA_TRANSFER_SUCCESS,            // Upgrade successful
    ZB_OTA_TRANSFER_FAILED,             // Transfer failed
    ZB_OTA_TRANSFER_ABORTED             // Transfer aborted
} zb_ota_transfer_state_t;

typedef struct {
    uint16_t manufacturer_code;         // Manufacturer code
    uint16_t image_type;                // Image type
    uint32_t file_version;              // File version
    uint32_t file_size;                 // File size in bytes
    char file_path[64];                 // File path on SPIFFS
    bool valid;                         // Image is valid
} zb_ota_image_info_t;

typedef void (*zb_ota_progress_cb_t)(const esp_zb_ieee_addr_t ieee_addr,
                                      zb_ota_transfer_state_t state,
                                      uint8_t progress);
```

#### Functions

##### zb_ota_init
```c
esp_err_t zb_ota_init(void);
```
Initialize Zigbee OTA server including SPIFFS mounting and APS fragmentation optimization.

---

##### zb_ota_add_image
```c
esp_err_t zb_ota_add_image(const char *file_path, zb_ota_image_info_t *image_info);
```
Add an OTA image from file.

**Parameters:**
- `file_path`: Path to OTA image file
- `image_info`: Output: Image information (can be NULL)

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if file not found
- `ESP_ERR_INVALID_SIZE` if file too large

---

##### zb_ota_notify_device
```c
esp_err_t zb_ota_notify_device(const esp_zb_ieee_addr_t ieee_addr,
                                uint8_t endpoint,
                                uint8_t payload_type);
```
Send Image Notify command to trigger an OTA update check.

**Parameters:**
- `ieee_addr`: Device IEEE address
- `endpoint`: Device OTA endpoint
- `payload_type`: Notify payload type (0-3)

---

##### zb_ota_get_transfer_status
```c
esp_err_t zb_ota_get_transfer_status(const esp_zb_ieee_addr_t ieee_addr,
                                      zb_ota_transfer_state_t *state,
                                      uint8_t *progress);
```
Get current OTA transfer status for a device.

---

##### zb_ota_abort_transfer
```c
esp_err_t zb_ota_abort_transfer(const esp_zb_ieee_addr_t ieee_addr);
```
Abort an ongoing OTA transfer.

---

### Diagnostics

**Header:** `main/zigbee/zb_diagnostics.h`

Provides network health monitoring and diagnostic information using the Diagnostics Cluster (0x0B05).

#### Types

```c
typedef struct {
    uint32_t rx_bcast;              // Broadcast messages received
    uint32_t tx_bcast;              // Broadcast messages transmitted
    uint32_t rx_ucast;              // Unicast messages received
    uint32_t tx_ucast;              // Unicast messages transmitted
    uint16_t tx_ucast_retry;        // Unicast transmit retries
    uint16_t tx_ucast_fail;         // Unicast transmit failures
} zb_diag_mac_t;

typedef struct {
    uint16_t number_of_resets;      // Number of device resets
    uint16_t persistent_mem_writes; // NVS write count
    zb_diag_mac_t mac;              // MAC Layer stats
    uint8_t last_message_lqi;       // LQI of last received message
    int8_t last_message_rssi;       // RSSI of last received message
} zb_diagnostics_t;
```

#### Functions

##### zb_diagnostics_init
```c
esp_err_t zb_diagnostics_init(void);
```
Initialize diagnostics module.

---

##### zb_diagnostics_get_coordinator
```c
esp_err_t zb_diagnostics_get_coordinator(zb_diagnostics_t *diag);
```
Get diagnostic data for the local coordinator.

**Parameters:**
- `diag`: Output for diagnostic data

---

##### zb_diagnostics_read
```c
esp_err_t zb_diagnostics_read(uint16_t short_addr, zb_diagnostics_t *diag);
```
Read diagnostics from a remote device.

**Parameters:**
- `short_addr`: Device short address
- `diag`: Output for diagnostic data

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_TIMEOUT` if request timed out

---

##### zb_diagnostics_publish_mqtt
```c
esp_err_t zb_diagnostics_publish_mqtt(void);
```
Publish coordinator diagnostics as JSON to MQTT.

---

## MQTT APIs

### MQTT Client

**Header:** `main/mqtt/gateway_mqtt.h`

Provides MQTT client functionality with thread-safe publish/subscribe, automatic reconnection, and message queueing.

#### Constants

```c
#define MQTT_BROKER_URL_MAX_LEN     128
#define MQTT_USERNAME_MAX_LEN       64
#define MQTT_PASSWORD_MAX_LEN       64
#define MQTT_CLIENT_ID_MAX_LEN      64
#define MQTT_CLIENT_QUEUE_SIZE      32
```

#### Types

```c
typedef enum {
    MQTT_STATE_DISCONNECTED,  // Not connected to broker
    MQTT_STATE_CONNECTING,    // Attempting to connect
    MQTT_STATE_CONNECTED,     // Successfully connected
    MQTT_STATE_ERROR          // Error state
} mqtt_state_t;

typedef struct {
    char broker_url[MQTT_BROKER_URL_MAX_LEN];
    uint16_t port;
    char username[MQTT_USERNAME_MAX_LEN];
    char password[MQTT_PASSWORD_MAX_LEN];
    char client_id[MQTT_CLIENT_ID_MAX_LEN];
    bool use_ssl;
    uint16_t keepalive;
    uint8_t qos;
    bool clean_session;
} mqtt_config_t;

typedef void (*mqtt_message_callback_t)(const char *topic, const char *data, size_t data_len);
typedef void (*mqtt_connection_callback_t)(bool connected);
```

#### Functions

##### mqtt_client_init
```c
esp_err_t mqtt_client_init(const mqtt_config_t *config);
```
Initialize MQTT client with specified configuration.

**Parameters:**
- `config`: MQTT configuration structure

**Returns:** `ESP_OK` on success.

**Example:**
```c
mqtt_config_t config = {
    .broker_url = "mqtt://192.168.1.100",
    .port = 1883,
    .qos = 1,
    .keepalive = 120
};
strcpy(config.client_id, "esp32c5_gateway");
mqtt_client_init(&config);
```

---

##### mqtt_client_start
```c
esp_err_t mqtt_client_start(void);
```
Start MQTT client and initiate connection to broker. Requires WiFi to be connected.

---

##### mqtt_client_publish
```c
esp_err_t mqtt_client_publish(const char *topic, const char *data,
                              size_t len, int qos, bool retain);
```
Publish message to specified topic.

**Parameters:**
- `topic`: Topic to publish to
- `data`: Message payload data
- `len`: Length of payload (0 for strlen)
- `qos`: QoS level (0, 1, or 2)
- `retain`: Retain flag

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NO_MEM` if queue full

**Thread Safety:** Thread-safe and ISR-safe. Can be called from any context.

**Example:**
```c
const char *json = "{\"state\":\"ON\",\"brightness\":254}";
mqtt_client_publish("zigbee2mqtt/living_room_light", json, 0, 1, false);
```

---

##### mqtt_client_publish_json
```c
esp_err_t mqtt_client_publish_json(const char *topic, cJSON *json, int qos, bool retain);
```
Serialize cJSON object and publish. The JSON object is freed after publishing.

**Example:**
```c
cJSON *state = cJSON_CreateObject();
cJSON_AddBoolToObject(state, "state", true);
cJSON_AddNumberToObject(state, "brightness", 254);
mqtt_client_publish_json("zigbee2mqtt/living_room_light", state, 1, false);
// json is automatically freed
```

---

##### mqtt_client_subscribe
```c
esp_err_t mqtt_client_subscribe(const char *topic, int qos);
```
Subscribe to MQTT topic.

**Parameters:**
- `topic`: Topic to subscribe (supports wildcards # and +)
- `qos`: QoS level

**Example:**
```c
mqtt_client_subscribe("zigbee2mqtt/#", 1);
mqtt_client_subscribe("zigbee2mqtt/+/set", 1);
```

---

##### mqtt_client_is_connected
```c
bool mqtt_client_is_connected(void);
```
Check MQTT connection status.

**Returns:** `true` if connected.

---

##### mqtt_client_register_callback
```c
esp_err_t mqtt_client_register_callback(mqtt_message_callback_t callback);
```
Register callback for received messages.

---

### Topic Patterns

**Header:** `main/mqtt/mqtt_topics.h`

Defines MQTT topic structure compatible with Zigbee2MQTT conventions.

#### Topic Definitions

```c
// Bridge topics
#define TOPIC_BRIDGE_STATE       "zigbee2mqtt/bridge/state"
#define TOPIC_BRIDGE_INFO        "zigbee2mqtt/bridge/info"
#define TOPIC_BRIDGE_DEVICES     "zigbee2mqtt/bridge/devices"
#define TOPIC_BRIDGE_GROUPS      "zigbee2mqtt/bridge/groups"
#define TOPIC_BRIDGE_LOGGING     "zigbee2mqtt/bridge/logging"
#define TOPIC_BRIDGE_EVENT       "zigbee2mqtt/bridge/event"

// Device command patterns
#define TOPIC_DEVICE_SET_PATTERN "zigbee2mqtt/+/set"
#define TOPIC_DEVICE_GET_PATTERN "zigbee2mqtt/+/get"

// Request topics
#define TOPIC_BRIDGE_REQUEST_PERMIT_JOIN    "zigbee2mqtt/bridge/request/permit_join"
#define TOPIC_BRIDGE_REQUEST_DEVICE_REMOVE  "zigbee2mqtt/bridge/request/device/remove"
#define TOPIC_BRIDGE_REQUEST_DEVICE_RENAME  "zigbee2mqtt/bridge/request/device/rename"
#define TOPIC_BRIDGE_REQUEST_DEVICE_BIND    "zigbee2mqtt/bridge/request/device/bind"
#define TOPIC_BRIDGE_REQUEST_DEVICE_UNBIND  "zigbee2mqtt/bridge/request/device/unbind"

// Buffer size constants
#define MQTT_TOPIC_MAX_LEN      256
#define MQTT_PAYLOAD_SMALL      256
#define MQTT_PAYLOAD_MEDIUM     512
#define MQTT_PAYLOAD_LARGE      1024
```

#### Functions

##### mqtt_topic_device_state
```c
esp_err_t mqtt_topic_device_state(const char *friendly_name, char *topic_buf, size_t buf_len);
```
Build device state topic: `zigbee2mqtt/[friendly_name]`

---

##### mqtt_topic_ha_discovery
```c
esp_err_t mqtt_topic_ha_discovery(const char *component, const char *device_id,
                                  char *topic_buf, size_t buf_len);
```
Build Home Assistant discovery topic: `homeassistant/[component]/[device_id]/config`

---

##### mqtt_topic_matches
```c
bool mqtt_topic_matches(const char *topic, const char *pattern);
```
Check if topic matches pattern with wildcard support (+ and #).

**Example:**
```c
mqtt_topic_matches("zigbee2mqtt/light_1/set", "zigbee2mqtt/+/set");  // true
mqtt_topic_matches("zigbee2mqtt/light_1", "zigbee2mqtt/#");         // true
```

---

## ESPHome APIs

### ESPHome API Server

**Header:** `main/esphome/esphome_api.h`

Implements a TCP server on port 6053 for Home Assistant integration using the ESPHome Native API protocol.

#### Features
- TCP server with configurable port
- Password-based authentication (optional)
- mDNS service announcement for auto-discovery
- Entity state subscription and updates
- Maximum 1-2 concurrent client connections

#### Constants

```c
#define ESPHOME_API_DEFAULT_PORT        6053    // Default TCP port
#define ESPHOME_MAX_NAME_LEN            64      // Max entity name length
#define ESPHOME_MAX_UNIQUE_ID_LEN       64      // Max unique ID length
#define ESPHOME_MAX_ICON_LEN            32      // Max icon name length
#define ESPHOME_MAX_UNIT_LEN            16      // Max unit string length
```

#### Types

```c
typedef struct {
    uint16_t port;                              // TCP server port (default: 6053)
    char password[64];                          // API password (empty for no auth)
    char device_name[ESPHOME_MAX_NAME_LEN];     // Device name for discovery
    char friendly_name[ESPHOME_MAX_NAME_LEN];   // Friendly name for display
    char mac_address[18];                       // MAC address
    uint8_t max_clients;                        // Max concurrent clients (1-2)
    bool use_mdns;                              // Enable mDNS announcement
    uint32_t keepalive_ms;                      // Keepalive interval in ms
} esphome_api_config_t;

typedef void (*esphome_api_connection_cb_t)(uint8_t client_id, bool connected,
                                            bool authenticated);
```

#### Functions

##### esphome_api_init
```c
esp_err_t esphome_api_init(const esphome_api_config_t *config);
```
Initialize ESPHome API server with given configuration.

**Parameters:**
- `config`: Server configuration (NULL for defaults)

---

##### esphome_api_start
```c
esp_err_t esphome_api_start(void);
```
Start TCP server and mDNS announcement. Requires WiFi to be connected.

---

##### esphome_api_stop
```c
esp_err_t esphome_api_stop(void);
```
Stop API server and disconnect all clients.

---

##### esphome_api_broadcast_state
```c
esp_err_t esphome_api_broadcast_state(esphome_entity_type_t entity_type,
                                       esphome_entity_key_t key, const void *state);
```
Broadcast entity state to all subscribed clients.

**Parameters:**
- `entity_type`: Type of entity
- `key`: Entity key
- `state`: Pointer to state structure (type-specific)

---

##### esphome_api_subscribe_ha_state
```c
esp_err_t esphome_api_subscribe_ha_state(const char *entity_id);
```
Subscribe to Home Assistant entity state updates.

**Parameters:**
- `entity_id`: Home Assistant entity ID (e.g., "sensor.temperature")

---

##### esphome_api_call_ha_service
```c
esp_err_t esphome_api_call_ha_service(const char *domain, const char *service,
                                       const char *data_json);
```
Call Home Assistant service via connected clients.

**Parameters:**
- `domain`: Service domain (e.g., "light")
- `service`: Service name (e.g., "turn_on")
- `data_json`: Service data as JSON string (may be NULL)

---

### Entity Management

**Header:** `main/esphome/esphome_entities.h`

Provides entity registration and state management for ESPHome Native API. Supports Sensor, Binary Sensor, Switch, Text Sensor, Number, Button, Select, Light, Cover, Fan, and Climate entity types.

#### Entity Types

##### Sensor Entity
```c
typedef struct {
    esphome_entity_key_t key;
    char name[ESPHOME_MAX_NAME_LEN];
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];
    char icon[ESPHOME_MAX_ICON_LEN];
    char unit_of_measurement[ESPHOME_MAX_UNIT_LEN];
    int32_t accuracy_decimals;
    bool force_update;
    esphome_sensor_device_class_t device_class;
    esphome_state_class_t state_class;
    bool disabled_by_default;
} esphome_sensor_config_t;

typedef struct {
    esphome_entity_key_t key;
    float state;
    bool missing_state;
} esphome_sensor_state_t;
```

##### Switch Entity
```c
typedef esp_err_t (*esphome_switch_command_cb_t)(esphome_entity_key_t key, bool state);

typedef struct {
    esphome_entity_key_t key;
    char name[ESPHOME_MAX_NAME_LEN];
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];
    char icon[ESPHOME_MAX_ICON_LEN];
    bool assumed_state;
    bool disabled_by_default;
    esphome_switch_command_cb_t command_callback;
} esphome_switch_config_t;
```

##### Light Entity
```c
typedef struct {
    bool has_state;
    bool state;
    bool has_brightness;
    float brightness;           // 0.0-1.0
    bool has_color_mode;
    esphome_color_mode_t color_mode;
    bool has_color_temp;
    float color_temp;           // mireds
    bool has_rgb;
    float red, green, blue;     // 0.0-1.0
    bool has_white;
    float white;
    bool has_effect;
    char effect[32];
    bool has_transition_length;
    uint32_t transition_length; // ms
} esphome_light_command_t;

typedef esp_err_t (*esphome_light_command_cb_t)(esphome_entity_key_t key,
                                                 const esphome_light_command_t *command);
```

##### Climate Entity
```c
typedef struct {
    bool has_mode;
    esphome_climate_mode_t mode;
    bool has_target_temperature;
    float target_temperature;
    bool has_target_temperature_low;
    float target_temperature_low;
    bool has_target_temperature_high;
    float target_temperature_high;
    bool has_fan_mode;
    esphome_climate_fan_mode_t fan_mode;
    bool has_swing_mode;
    esphome_climate_swing_mode_t swing_mode;
    bool has_preset;
    esphome_climate_preset_t preset;
} esphome_climate_command_t;
```

#### Functions

##### esphome_entities_init
```c
esp_err_t esphome_entities_init(void);
```
Initialize entity management module.

---

##### esphome_entity_register_sensor
```c
esp_err_t esphome_entity_register_sensor(const esphome_sensor_config_t *config);
```
Register a sensor entity.

---

##### esphome_entity_register_switch
```c
esp_err_t esphome_entity_register_switch(const esphome_switch_config_t *config);
```
Register a switch entity with command callback.

---

##### esphome_entity_register_light
```c
esp_err_t esphome_entity_register_light(const esphome_light_config_t *config);
```
Register a light entity.

---

##### esphome_entity_update_sensor
```c
esp_err_t esphome_entity_update_sensor(esphome_entity_key_t key, float state);
```
Update sensor state and broadcast to clients.

---

##### esphome_entity_update_switch
```c
esp_err_t esphome_entity_update_switch(esphome_entity_key_t key, bool state);
```
Update switch state and broadcast to clients.

---

### BLE Proxy

**Header:** `main/esphome/esphome_ble_proxy.h`

Implements the Bluetooth Proxy protocol for Home Assistant, enabling HA to receive BLE advertisements and perform GATT operations through this device as a proxy.

#### Constants

```c
#define ESPHOME_BLE_PROXY_MAX_CONNECTIONS       3   // Max GATT connections
#define ESPHOME_BLE_PROXY_ADV_QUEUE_SIZE        32  // Advertisement queue size
#define ESPHOME_BLE_PROXY_DEFAULT_RSSI_FILTER   (-90)
```

#### Types

```c
typedef enum {
    ESPHOME_BLE_REQUEST_CONNECT = 0,
    ESPHOME_BLE_REQUEST_DISCONNECT = 1,
    ESPHOME_BLE_REQUEST_PAIR = 2,
    ESPHOME_BLE_REQUEST_UNPAIR = 3,
    ESPHOME_BLE_REQUEST_CLEAR_CACHE = 4,
} esphome_ble_request_type_t;

typedef struct {
    bool enable_advertisements;     // Forward BLE advertisements
    bool enable_connections;        // Allow GATT connections
    int8_t min_rssi;                // Minimum RSSI filter
    uint16_t scan_interval_ms;      // Scan interval
    uint16_t scan_window_ms;        // Scan window
} esphome_ble_proxy_config_t;

typedef struct {
    uint32_t advertisements_received;
    uint32_t advertisements_forwarded;
    uint32_t advertisements_filtered;
    uint32_t advertisements_dropped;
    uint32_t gatt_reads;
    uint32_t gatt_writes;
    uint32_t gatt_errors;
    uint32_t connections;
    uint32_t disconnections;
    uint8_t active_connections;
    uint8_t subscribed_clients;
} esphome_ble_proxy_stats_t;
```

#### Functions

##### esphome_ble_proxy_init
```c
esp_err_t esphome_ble_proxy_init(const esphome_ble_proxy_config_t *config);
```
Initialize BLE Proxy module. Requires BLE manager to be initialized first.

---

##### esphome_ble_proxy_start
```c
esp_err_t esphome_ble_proxy_start(void);
```
Start scanning and advertisement forwarding.

---

##### esphome_ble_proxy_stop
```c
esp_err_t esphome_ble_proxy_stop(void);
```
Stop scanning and disconnect all GATT connections.

---

##### esphome_ble_proxy_get_free_connections
```c
uint8_t esphome_ble_proxy_get_free_connections(void);
```
Get number of free GATT connection slots.

---

##### esphome_ble_proxy_get_stats
```c
esp_err_t esphome_ble_proxy_get_stats(esphome_ble_proxy_stats_t *stats);
```
Get BLE proxy statistics.

---

## Core APIs

### Configuration Manager

**Header:** `main/core/config_manager.h`

Manages all gateway configuration with NVS persistence. Provides runtime configuration changes via MQTT without recompilation.

#### Constants

```c
#define CONFIG_SSID_MAX_LEN         32
#define CONFIG_PASSWORD_MAX_LEN     64
#define CONFIG_URL_MAX_LEN          128
#define CONFIG_ID_MAX_LEN           64
#define CONFIG_VERSION              1
#define CONFIG_NVS_NAMESPACE        "gateway_cfg"
```

#### Types

```c
typedef struct {
    // Network Configuration
    char wifi_ssid[CONFIG_SSID_MAX_LEN];
    char wifi_password[CONFIG_PASSWORD_MAX_LEN];

    // MQTT Configuration
    char mqtt_broker_url[CONFIG_URL_MAX_LEN];
    uint16_t mqtt_port;
    char mqtt_username[CONFIG_ID_MAX_LEN];
    char mqtt_password[CONFIG_PASSWORD_MAX_LEN];
    char mqtt_client_id[CONFIG_ID_MAX_LEN];
    uint16_t mqtt_keepalive;
    uint8_t mqtt_qos;

    // Zigbee Configuration
    uint16_t zigbee_pan_id;
    uint8_t zigbee_channel;
    uint8_t zigbee_max_children;
    bool zigbee_permit_join_on_boot;

    // Gateway Configuration
    uint32_t device_publish_interval_ms;
    bool ha_discovery_enabled;
    bool bridge_logging_enabled;

    // System Configuration
    uint8_t log_level;
    bool ota_enabled;
    char ota_url[CONFIG_URL_MAX_LEN];

    uint32_t config_version;
} gateway_config_t;
```

#### Functions

##### config_manager_init
```c
esp_err_t config_manager_init(void);
```
Initialize configuration manager. Loads configuration from NVS or uses Kconfig defaults.

---

##### config_manager_load
```c
esp_err_t config_manager_load(gateway_config_t *config);
```
Load configuration from NVS.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if no config in NVS
- `ESP_ERR_INVALID_ARG` if config is NULL

---

##### config_manager_save
```c
esp_err_t config_manager_save(const gateway_config_t *config);
```
Save configuration to NVS.

---

##### config_manager_reset_to_defaults
```c
esp_err_t config_manager_reset_to_defaults(void);
```
Reset configuration to Kconfig defaults and erase NVS configuration.

---

##### config_manager_get_config
```c
const gateway_config_t* config_manager_get_config(void);
```
Get pointer to current configuration (read-only).

---

##### config_manager_get
```c
esp_err_t config_manager_get(const char *key, void *value, size_t *len);
```
Get configuration value by key name.

**Parameters:**
- `key`: Configuration key (e.g., "wifi_ssid")
- `value`: Buffer to store value
- `len`: Size of buffer on input, actual size on output

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if key not found
- `ESP_ERR_INVALID_SIZE` if buffer too small

---

##### config_manager_set
```c
esp_err_t config_manager_set(const char *key, const void *value, size_t len);
```
Set configuration value by key and save to NVS.

**Example:**
```c
uint8_t new_channel = 20;
config_manager_set("zigbee_channel", &new_channel, sizeof(new_channel));

const char *new_ssid = "NewNetwork";
config_manager_set("wifi_ssid", new_ssid, strlen(new_ssid) + 1);
```

---

##### config_manager_export_json
```c
esp_err_t config_manager_export_json(char *buffer, size_t buffer_size);
```
Export configuration as JSON string. Passwords are masked for security.

---

##### config_manager_import_json
```c
esp_err_t config_manager_import_json(const char *json);
```
Import configuration from JSON string. Validates all fields before applying.

---

##### config_manager_apply
```c
esp_err_t config_manager_apply(const gateway_config_t *config);
```
Apply configuration changes to running system. May trigger reconnections.

**Returns:**
- `ESP_OK` on success
- `ESP_FAIL` if changes require restart

---

### Home Assistant Discovery

**Header:** `main/core/discovery/ha_discovery.h`

Publishes Home Assistant MQTT discovery messages for automatic device detection.

#### Functions

##### ha_discovery_init
```c
esp_err_t ha_discovery_init(void);
```
Initialize Home Assistant discovery. Must be called before publishing discovery messages.

---

##### ha_discovery_publish_device
```c
esp_err_t ha_discovery_publish_device(const zb_device_t *device);
```
Automatically determine device type and publish appropriate discovery message(s).

**Parameters:**
- `device`: Pointer to Zigbee device structure

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if device is NULL
- `ESP_ERR_NOT_SUPPORTED` if device type not supported

---

##### ha_discovery_publish_light
```c
esp_err_t ha_discovery_publish_light(const zb_device_t *device);
```
Publish Home Assistant light discovery configuration.

---

##### ha_discovery_publish_sensor
```c
esp_err_t ha_discovery_publish_sensor(const zb_device_t *device, const char *sensor_type);
```
Publish sensor discovery. `sensor_type` can be "temperature", "humidity", "battery", etc.

---

##### ha_discovery_publish_binary_sensor
```c
esp_err_t ha_discovery_publish_binary_sensor(const zb_device_t *device, const char *sensor_class);
```
Publish binary sensor discovery. `sensor_class` can be "motion", "door", "window", "occupancy".

---

##### ha_discovery_publish_switch
```c
esp_err_t ha_discovery_publish_switch(const zb_device_t *device);
```
Publish switch discovery configuration.

---

##### ha_discovery_publish_cover
```c
esp_err_t ha_discovery_publish_cover(const zb_device_t *device);
```
Publish cover discovery for window covering devices (blinds, shades, shutters).

---

##### ha_discovery_publish_lock
```c
esp_err_t ha_discovery_publish_lock(const zb_device_t *device);
```
Publish lock discovery for door lock devices.

---

##### ha_discovery_publish_climate
```c
esp_err_t ha_discovery_publish_climate(const zb_device_t *device);
```
Publish climate discovery for thermostat devices.

---

##### ha_discovery_publish_fan
```c
esp_err_t ha_discovery_publish_fan(const zb_device_t *device);
```
Publish fan discovery for fan control devices.

---

##### ha_discovery_publish_energy_meter
```c
esp_err_t ha_discovery_publish_energy_meter(const zb_device_t *device);
```
Publish energy meter discovery (Metering cluster). Creates multiple sensor entities.

---

##### ha_discovery_publish_ias_zone
```c
esp_err_t ha_discovery_publish_ias_zone(const zb_device_t *device, const char *device_class);
```
Publish IAS Zone discovery. Creates main alarm sensor, tamper sensor, and battery low sensor.

---

##### ha_discovery_publish_all
```c
esp_err_t ha_discovery_publish_all(void);
```
Publish discovery for all registered devices. Useful during startup or after Home Assistant restart.

---

##### ha_discovery_remove_device
```c
esp_err_t ha_discovery_remove_device(const zb_device_t *device);
```
Remove device from Home Assistant by publishing empty discovery messages.

---

##### ha_discovery_set_enabled
```c
esp_err_t ha_discovery_set_enabled(bool enable);
```
Enable or disable Home Assistant discovery.

---

### Bridge Events

**Header:** `main/core/bridge/bridge_events.h`

Provides centralized event publishing for all important bridge events to `zigbee2mqtt/bridge/event`.

#### Event Types

```c
typedef enum {
    BRIDGE_EVENT_DEVICE_JOINED = 0,     // Device joined the network
    BRIDGE_EVENT_DEVICE_LEAVE,          // Device left the network
    BRIDGE_EVENT_DEVICE_INTERVIEW,      // Device interview status changed
    BRIDGE_EVENT_DEVICE_ANNOUNCE,       // Device announced itself
    BRIDGE_EVENT_DEVICE_RENAMED,        // Device was renamed
    BRIDGE_EVENT_PERMIT_JOIN,           // Permit join status changed
    BRIDGE_EVENT_NETWORK_STARTED,       // Network started
    BRIDGE_EVENT_NETWORK_STOPPED,       // Network stopped
    BRIDGE_EVENT_DEVICE_BIND,           // Device binding created
    BRIDGE_EVENT_DEVICE_UNBIND,         // Device binding removed
    BRIDGE_EVENT_GROUP_MEMBER_ADDED,    // Member added to group
    BRIDGE_EVENT_GROUP_MEMBER_REMOVED,  // Member removed from group
    BRIDGE_EVENT_NLME_STATUS,           // Network layer status indication
    BRIDGE_EVENT_PARENT_ANNOUNCE,       // Parent announcement (mesh)
    BRIDGE_EVENT_TC_REJOIN,             // Trust Center rejoin complete
    BRIDGE_EVENT_MAX                    // Maximum event type
} bridge_event_type_t;
```

#### Constants

```c
#define BRIDGE_INTERVIEW_STARTED     "started"
#define BRIDGE_INTERVIEW_SUCCESSFUL  "successful"
#define BRIDGE_INTERVIEW_FAILED      "failed"
#define BRIDGE_EVENT_TOPIC           "zigbee2mqtt/bridge/event"
```

#### Functions

##### bridge_events_init
```c
esp_err_t bridge_events_init(void);
```
Initialize the bridge events module. Should be called after MQTT client is ready.

---

##### bridge_events_deinit
```c
esp_err_t bridge_events_deinit(void);
```
Deinitialize the bridge events module.

---

##### bridge_events_publish
```c
esp_err_t bridge_events_publish(bridge_event_type_t type, cJSON *data);
```
Publish an event with custom data. The data cJSON object is consumed and freed.

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_ARG` if type is invalid
- `ESP_ERR_INVALID_STATE` if not initialized or MQTT not connected
- `ESP_ERR_NO_MEM` if memory allocation fails

---

##### bridge_events_get_type_str
```c
const char* bridge_events_get_type_str(bridge_event_type_t type);
```
Get Zigbee2MQTT compatible string name for an event type.

---

##### bridge_event_device_joined
```c
esp_err_t bridge_event_device_joined(uint64_t ieee_addr, const char *friendly_name);
```
Publish device_joined event.

**Example:**
```c
bridge_event_device_joined(0x00158D0001234567, "Living Room Light");
// Publishes: {"type": "device_joined", "data": {"ieee_address": "0x00158d0001234567", "friendly_name": "Living Room Light"}}
```

---

##### bridge_event_device_leave
```c
esp_err_t bridge_event_device_leave(uint64_t ieee_addr, const char *friendly_name);
```
Publish device_leave event.

---

##### bridge_event_device_interview
```c
esp_err_t bridge_event_device_interview(uint64_t ieee_addr, const char *status);
```
Publish device_interview event. Status can be "started", "successful", or "failed".

---

##### bridge_event_device_announce
```c
esp_err_t bridge_event_device_announce(uint64_t ieee_addr);
```
Publish device_announce event when a device announces itself (typically after power cycle).

---

##### bridge_event_device_renamed
```c
esp_err_t bridge_event_device_renamed(uint64_t ieee_addr, const char *from, const char *to);
```
Publish device_renamed event.

---

##### bridge_event_permit_join
```c
esp_err_t bridge_event_permit_join(bool enabled, uint8_t time);
```
Publish permit_join event.

**Parameters:**
- `enabled`: true if permit join is enabled
- `time`: Duration in seconds (0 if disabled, 254/255 for unlimited)

---

##### bridge_event_network_started
```c
esp_err_t bridge_event_network_started(void);
```
Publish network_started event.

---

##### bridge_event_network_stopped
```c
esp_err_t bridge_event_network_stopped(void);
```
Publish network_stopped event.

---

##### bridge_event_device_bind
```c
esp_err_t bridge_event_device_bind(uint64_t source_ieee, const char *source_name,
                                    uint64_t target_ieee, const char *target_name,
                                    const char *cluster_name);
```
Publish device_bind event when a binding is created.

---

##### bridge_event_device_unbind
```c
esp_err_t bridge_event_device_unbind(uint64_t source_ieee, const char *source_name,
                                      uint64_t target_ieee, const char *target_name,
                                      const char *cluster_name);
```
Publish device_unbind event when a binding is removed.

---

##### bridge_event_group_member_added
```c
esp_err_t bridge_event_group_member_added(const char *group_name, uint16_t group_id,
                                           uint64_t ieee_addr, const char *device_name);
```
Publish group_member_added event.

---

##### bridge_events_get_stats
```c
esp_err_t bridge_events_get_stats(uint32_t *total_count, uint32_t *error_count);
```
Get event statistics.

---

### MQTT Bridge

**Header:** `main/core/bridge/mqtt_bridge.h`

Central bridge coordinator between Zigbee and MQTT protocols.

#### Functions

##### mqtt_bridge_init
```c
esp_err_t mqtt_bridge_init(void);
```
Initialize MQTT bridge and sub-modules.

**Prerequisites:** MQTT client and Zigbee coordinator must be initialized first.

---

##### mqtt_bridge_start
```c
esp_err_t mqtt_bridge_start(void);
```
Start bridge operation, subscribe to topics, publish bridge state.

---

##### mqtt_bridge_publish_device_state
```c
esp_err_t mqtt_bridge_publish_device_state(uint16_t short_addr);
```
Publish current state of a Zigbee device to MQTT.

**Parameters:**
- `short_addr`: Device short address

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if device not found

---

##### mqtt_bridge_on_device_join
```c
esp_err_t mqtt_bridge_on_device_join(uint16_t short_addr);
```
Handle device join events. Called by Zigbee coordinator.

**Actions:**
- Publishes device availability
- Triggers Home Assistant discovery
- Updates device list

---

##### mqtt_bridge_on_attribute_change
```c
esp_err_t mqtt_bridge_on_attribute_change(
    uint16_t short_addr,
    uint16_t cluster_id,
    uint16_t attr_id,
    const void *value,
    size_t value_len
);
```
Handle Zigbee attribute changes.

---

### System Monitor

**Header:** `main/core/monitoring/system_monitor.h`

#### Functions

##### system_monitor_init
```c
esp_err_t system_monitor_init(bool enable_mqtt_publish);
```
Initialize system monitoring.

---

##### system_monitor_start
```c
esp_err_t system_monitor_start(uint32_t interval_sec);
```
Start monitoring task.

---

##### system_monitor_get_stats
```c
system_stats_t system_monitor_get_stats(void);
```
Get current system statistics.

**Example:**
```c
system_stats_t stats = system_monitor_get_stats();
printf("Uptime: %llu seconds\n", stats.uptime_seconds);
printf("Free heap: %u bytes\n", stats.free_heap);
printf("WiFi RSSI: %d dBm\n", stats.wifi_rssi);
```

---

## WiFi Manager API

**Header:** `main/wifi/wifi_manager.h`

#### Functions

##### wifi_manager_init
```c
esp_err_t wifi_manager_init(void);
```
Initialize WiFi subsystem.

---

##### wifi_manager_connect
```c
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
```
Connect to WiFi network.

**Parameters:**
- `ssid`: Network SSID (max 32 chars)
- `password`: Network password (max 64 chars)

---

##### wifi_manager_get_ip
```c
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
```
Get current IP address as string.

---

##### wifi_manager_is_connected
```c
bool wifi_manager_is_connected(void);
```
Check WiFi connection status.

---

##### wifi_manager_get_rssi
```c
int8_t wifi_manager_get_rssi(void);
```
Get WiFi signal strength.

**Returns:** RSSI in dBm (-100 to 0)

**Signal Quality:**
- -30 dBm: Excellent
- -50 dBm: Good
- -70 dBm: Fair
- -90 dBm: Poor

---

## OTA Handler API

**Header:** `main/ota/ota_handler.h`

#### Functions

##### ota_handler_init
```c
esp_err_t ota_handler_init(const char *firmware_url);
```
Initialize OTA handler.

---

##### ota_handler_check_for_update
```c
esp_err_t ota_handler_check_for_update(void);
```
Check if firmware update is available.

---

##### ota_handler_start_update
```c
esp_err_t ota_handler_start_update(void);
```
Start firmware update process.

**Note:** Device will reboot after successful update.

---

##### ota_handler_mark_valid
```c
esp_err_t ota_handler_mark_valid(void);
```
Mark current firmware as valid (prevents rollback).

---

## Utility APIs

### JSON Utils

**Header:** `main/utils/json_utils.h`

```c
cJSON* json_create_object(void);
void json_add_string(cJSON *obj, const char *key, const char *value);
void json_add_number(cJSON *obj, const char *key, double value);
char* json_to_string(cJSON *obj);  // Must be freed with cJSON_free()
```

**Example:**
```c
cJSON *json = json_create_object();
json_add_string(json, "state", "ON");
json_add_number(json, "brightness", 128);
char *str = json_to_string(json);
printf("%s\n", str);
cJSON_free(str);
cJSON_Delete(json);
```

### Version

**Header:** `main/utils/version.h`

```c
const char* version_get_number(void);  // e.g., "v1.0.0"
void version_print(void);
```

---

## Error Handling

All APIs use standard ESP-IDF error codes:

| Code | Description |
|------|-------------|
| `ESP_OK` | Success |
| `ESP_FAIL` | General failure |
| `ESP_ERR_NO_MEM` | Out of memory |
| `ESP_ERR_INVALID_ARG` | Invalid argument |
| `ESP_ERR_INVALID_STATE` | Invalid state for operation |
| `ESP_ERR_NOT_FOUND` | Resource not found |
| `ESP_ERR_NOT_SUPPORTED` | Operation not supported |
| `ESP_ERR_TIMEOUT` | Operation timed out |
| `ESP_ERR_INVALID_SIZE` | Buffer too small |
| `ESP_ERR_NVS_NOT_FOUND` | NVS key not found |

**Example:**
```c
esp_err_t ret = function_call();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Function failed: %s", esp_err_to_name(ret));
    // Handle error
}
```

---

## Thread Safety

### Thread-Safe Functions
- All MQTT publish functions (`mqtt_client_publish`, `mqtt_client_publish_json`)
- Coordinator permit join (`zb_coordinator_permit_join`)
- Bridge events (`bridge_events_publish` and convenience functions)
- Configuration manager get functions (`config_manager_get_config`)
- System monitor stats (`system_monitor_get_stats`)

### Non-Thread-Safe Functions
- Initialization functions (must be called from main task)
- Device handler direct pointer access (`zb_device_get`)
- ESPHome entity registration

### ISR-Safe Functions
- `mqtt_client_publish` - Automatically detects ISR context

---

## Usage Examples

### Basic Zigbee Gateway Startup

```c
void app_main(void) {
    // Initialize NVS
    nvs_flash_init();

    // Initialize configuration
    config_manager_init();

    // Initialize WiFi and connect
    wifi_manager_init();
    wifi_manager_start();

    // Initialize MQTT
    mqtt_config_t mqtt_cfg = {
        .broker_url = "mqtt://192.168.1.100",
        .port = 1883,
        .client_id = "esp32c5_gateway",
    };
    mqtt_client_init(&mqtt_cfg);

    // Initialize Zigbee coordinator
    zb_coordinator_init();

    // Initialize device handler
    zb_device_handler_init();

    // Initialize interview module
    zb_interview_config_t interview_cfg = {
        .complete_cb = on_interview_complete,
        .auto_interview_on_join = true,
    };
    zb_interview_init(&interview_cfg);

    // Initialize Home Assistant discovery
    ha_discovery_init();

    // Initialize bridge events
    bridge_events_init();

    // Start MQTT client
    mqtt_client_start();

    // Start Zigbee coordinator
    zb_coordinator_start();

    // Publish all existing devices
    ha_discovery_publish_all();
}
```

### Handling Device Join

```c
void on_device_joined(esp_zb_ieee_addr_t ieee_addr, uint16_t short_addr) {
    // Add to device registry
    zb_device_add(ieee_addr, short_addr);

    // Convert IEEE to uint64_t for event
    uint64_t ieee64 = 0;
    memcpy(&ieee64, ieee_addr, 8);

    // Publish join event
    bridge_event_device_joined(ieee64, NULL);

    // Interview will start automatically if configured
}

void on_interview_complete(const zb_interview_result_t *result) {
    if (result->status == ZB_INTERVIEW_STATUS_COMPLETE) {
        // Update device info
        zb_device_t *device = zb_device_get_by_ieee(result->ieee_addr);
        if (device) {
            // Publish Home Assistant discovery
            ha_discovery_publish_device(device);
        }
    }

    // Publish interview event
    bridge_event_device_interview(result->ieee_addr,
        result->status == ZB_INTERVIEW_STATUS_COMPLETE ?
        BRIDGE_INTERVIEW_SUCCESSFUL : BRIDGE_INTERVIEW_FAILED);
}
```

---

## Memory Management

- Functions returning pointers to static data do **not** need to be freed
- Functions allocating memory will document it clearly
- JSON functions use `cJSON_Delete()` and `cJSON_free()` for cleanup

---

## For More Information

- [Architecture](ARCHITECTURE.md) - System design
- [Development Guide](DEVELOPMENT.md) - Development workflows
- [Usage Guide](USAGE.md) - User-facing functionality

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2026-01 | Comprehensive API documentation with all modules |
| 1.0 | 2025-12 | Initial API documentation |
