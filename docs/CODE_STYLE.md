# Code Style Guidelines

<!-- staleness-banner -->
> **Stand 2026-08-05.** Die BLE-Beispiele beziehen sich auf abgeschalteten Code. Die Stilregeln selbst
> gelten unveraendert.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


> **Authoritative Source**: This document is the comprehensive reference for all coding standards in the ESP32-C5 Unified Gateway project. [CLAUDE.md](../CLAUDE.md) provides a quick reference summary.

This document defines the coding standards and best practices for the ESP32-C5 Zigbee2MQTT Gateway project. All contributors must adhere to these guidelines to maintain code consistency and quality.

## Table of Contents

1. [Naming Conventions](#naming-conventions)
2. [Error Handling Patterns](#error-handling-patterns)
3. [Logging Best Practices](#logging-best-practices)
4. [Memory Management Rules](#memory-management-rules)
5. [Thread Safety Guidelines](#thread-safety-guidelines)
6. [Code Formatting](#code-formatting)
7. [Documentation](#documentation)
8. [Code Quality Rules](#code-quality-rules)

---

## Naming Conventions

Consistent naming conventions improve code readability and reduce bugs. Follow these rules strictly:

### Functions

Use **snake_case** for all function names. Include a module prefix to denote the module or component.

```c
// Correct
esp_err_t mqtt_bridge_init(const mqtt_config_t *config);
void zb_coordinator_start(void);
uint16_t ble_scanner_get_device_count(void);

// Incorrect
esp_err_t MqttBridgeInit(const mqtt_config_t *config);  // camelCase
void Zb_Coordinator_Start(void);                         // Mixed case
```

### Macros and Constants

Use **UPPER_CASE** with underscores to separate words. Include a module prefix for context.

```c
// Correct
#define ZB_MAX_DEVICES 50
#define GW_BUFFER_SIZE 256
#define MQTT_CLIENT_QUEUE_SIZE 32
#define SYSMON_TASK_PRIORITY 3

// Incorrect
#define zbMaxDevices 50         // camelCase
#define max_devices 50          // lowercase
#define ZB_MAX_DEVICES_COUNT 50 // Redundant suffix
```

### Types (typedef)

Use **snake_case** with a `_t` suffix for all typedef'd types.

```c
// Correct
typedef struct {
    uint16_t device_id;
    char device_name[64];
} zb_device_t;

typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
} mqtt_state_t;

// Incorrect
typedef struct {
    uint16_t deviceId;
    char deviceName[64];
} ZBDevice;  // All caps without _t

typedef enum {
    MQTT_DISCONNECTED = 0,     // No _t suffix
} mqtt_state;
```

### Static and File-Scope Variables

Use the **`s_`** prefix for all static variables at file scope. This indicates the variable is local to the file.

**IMPORTANT**: Do NOT use `g_` prefix for global static variables. Always use `s_`.

```c
// Correct
static bool s_initialized = false;
static SemaphoreHandle_t s_state_mutex = NULL;
static esp_timer_handle_t s_heartbeat_timer = NULL;
static mqtt_config_t s_mqtt_config = {0};

// Incorrect
static bool g_initialized = false;     // g_ is not allowed
static bool initialized = false;       // Missing prefix
static bool _initialized = false;      // Wrong prefix
```

### Enumerations

Use **UPPER_CASE** with an appropriate module prefix. Group related values logically.

```c
// Correct
typedef enum {
    ZB_COORD_STATE_INIT = 0,
    ZB_COORD_STATE_STARTING,
    ZB_COORD_STATE_RUNNING,
    ZB_COORD_STATE_STOPPING,
} zb_coordinator_state_t;

typedef enum {
    BLE_SCANNER_STATE_IDLE = 0,
    BLE_SCANNER_STATE_SCANNING,
    BLE_SCANNER_STATE_STOPPED,
} ble_scanner_state_t;

// Incorrect
typedef enum {
    INIT = 0,              // No prefix
    STARTING,              // No prefix
    zb_running,            // Lowercase
    Zb_CoordState,         // camelCase
} zb_coordinator_state_t;
```

---

## Error Handling Patterns

Proper error handling is critical for system stability and debugging. These patterns are mandatory.

### Checking esp_err_t Returns

Always check the return value of ESP-IDF functions. Use the `esp_err_to_name()` utility for descriptive error logging.

```c
// Correct - Check all returns
esp_err_t ret = nvs_flash_init();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
    return ret;
}

// Also correct - Variable return code stored for cleanup
esp_err_t ret = mqtt_client_start();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(ret));
    goto cleanup;
}

// Incorrect - Ignoring return value
nvs_flash_init();  // Error silently ignored
mqtt_client_start();
```

### Using ESP_ERROR_CHECK()

Only use `ESP_ERROR_CHECK()` for truly fatal errors that should halt the system.

```c
// Correct - Fatal error, must halt
ESP_ERROR_CHECK(nvs_flash_init());

// Incorrect - Recoverable error
esp_err_t ret = mqtt_client_connect();
ESP_ERROR_CHECK(ret);  // Should check and handle gracefully instead
```

### Checking cJSON Allocations

All cJSON allocations **must** be checked for NULL before use.

```c
// Correct
cJSON *root = cJSON_CreateObject();
if (root == NULL) {
    ESP_LOGE(TAG, "Failed to create JSON object");
    return ESP_ERR_NO_MEM;
}

cJSON *device = cJSON_CreateObject();
if (device == NULL) {
    ESP_LOGE(TAG, "Failed to create device object");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
}

cJSON_AddItemToObject(root, "device", device);
// ... use root ...
cJSON_Delete(root);

// Incorrect
cJSON *root = cJSON_CreateObject();
cJSON_AddStringToObject(root, "name", "gateway");  // No NULL check
```

### Goto Cleanup Pattern

Use the **goto cleanup** pattern for functions that allocate multiple resources. This ensures all resources are properly freed on error paths.

```c
// Correct - Multiple resources
esp_err_t mqtt_bridge_init(const mqtt_config_t *config) {
    esp_err_t ret = ESP_OK;
    char *buffer = NULL;
    cJSON *config_json = NULL;

    // Allocate buffer
    buffer = malloc(256);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // Create JSON object
    config_json = cJSON_CreateObject();
    if (config_json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // Process configuration
    ret = process_mqtt_config(config, buffer, config_json);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process config: %s", esp_err_to_name(ret));
        goto cleanup;
    }

cleanup:
    if (config_json != NULL) {
        cJSON_Delete(config_json);
    }
    if (buffer != NULL) {
        free(buffer);
    }
    return ret;
}

// Incorrect - No cleanup, resource leak on error
esp_err_t mqtt_bridge_init(const mqtt_config_t *config) {
    char *buffer = malloc(256);
    cJSON *config_json = cJSON_CreateObject();

    if (config_json == NULL) {
        return ESP_ERR_NO_MEM;  // buffer leaked!
    }

    esp_err_t ret = process_mqtt_config(config, buffer, config_json);
    if (ret != ESP_OK) {
        return ret;  // Both allocations leaked!
    }

    cJSON_Delete(config_json);
    free(buffer);
    return ESP_OK;
}
```

---

## Logging Best Practices

Comprehensive logging enables efficient debugging and system monitoring.

### Module TAG Definition

Define a static const TAG at the top of each module. Use uppercase with underscores.

```c
// File: main/mqtt/mqtt_bridge.c
static const char *TAG = "MQTT_BRIDGE";

// File: main/zigbee/zb_coordinator.c
static const char *TAG = "ZB_COORDINATOR";

// File: main/bluetooth/ble_scanner.c
static const char *TAG = "BLE_SCANNER";
```

### Log Levels

Use appropriate log levels to control output verbosity and severity:

| Level | Function | Use Case |
|-------|----------|----------|
| ERROR | `ESP_LOGE()` | Fatal errors, operation failures, data corruption |
| WARN  | `ESP_LOGW()` | Recoverable issues, unusual conditions, deprecated usage |
| INFO  | `ESP_LOGI()` | Normal operation milestones, state changes, initialization |
| DEBUG | `ESP_LOGD()` | Detailed flow information, function entry/exit, variable values |
| VERBOSE | `ESP_LOGV()` | Very detailed debugging, protocol traces, loops |

### Logging with esp_err_to_name()

Always use `esp_err_to_name()` when logging ESP-IDF error codes for clarity.

```c
// Correct - Descriptive error message
esp_err_t ret = esp_zb_zcl_on_off_cmd_req(&cmd_req);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to send ON/OFF command to device 0x%04x: %s",
             device_id, esp_err_to_name(ret));
    return ret;
}

// Correct - Informational log on success
ESP_LOGI(TAG, "Device 0x%04x state changed to %s", device_id,
         state ? "ON" : "OFF");

// Correct - Debug log for flow tracking
ESP_LOGD(TAG, "Processing attribute report from device 0x%04x, cluster 0x%04x",
         device_id, cluster_id);

// Incorrect - No error description
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error");  // Too vague
    return ret;
}

// Incorrect - Raw error code instead of name
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error code: %d", ret);  // Hard to read
    return ret;
}
```

### Conditional Compilation for Logging

For verbose logging that should be disabled in production builds, use compiler conditionals:

```c
#if CONFIG_LOG_LEVEL_DEBUG
    ESP_LOGD(TAG, "Protocol packet: type=%u, length=%u", type, length);
#endif
```

---

## Memory Management Rules

Careful memory management is essential on embedded systems with limited resources.

### Allocating Memory

Always check the return value of malloc/calloc. The ESP32-C5 has 384KB SRAM and 8MB PSRAM.

```c
// Correct - malloc with NULL check
char *buffer = malloc(256);
if (buffer == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed");
    return ESP_ERR_NO_MEM;
}

// Correct - calloc with NULL check (automatically zeroed)
device_data_t *dev = calloc(1, sizeof(device_data_t));
if (dev == NULL) {
    ESP_LOGE(TAG, "Failed to allocate device structure");
    return ESP_ERR_NO_MEM;
}

// Incorrect - No NULL check
char *buffer = malloc(256);
strcpy(buffer, "data");  // Crash if malloc returned NULL

// Incorrect - Using stack for large buffers
void process_data(void) {
    char large_buffer[4096];  // 4KB on stack, wasteful
    // ...
}
```

### Freeing Memory

Free all dynamically allocated memory on both success and error paths. Use NULL assignments after freeing to prevent double-free bugs.

```c
// Correct - Proper cleanup
void cleanup_device(device_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->buffer != NULL) {
        free(ctx->buffer);
        ctx->buffer = NULL;
    }

    if (ctx->config != NULL) {
        free(ctx->config);
        ctx->config = NULL;
    }

    free(ctx);
}

// Correct - NULL check before freeing (defensive)
cJSON *obj = cJSON_CreateObject();
if (obj == NULL) {
    return ESP_ERR_NO_MEM;
}

// ... use obj ...

if (obj != NULL) {
    cJSON_Delete(obj);
}

// Incorrect - No NULL assignment after free
void cleanup_device(device_context_t *ctx) {
    free(ctx->buffer);  // Doesn't set to NULL
    free(ctx->config);
    free(ctx);
}

// Incorrect - Ignoring cleanup
device_context_t *ctx = create_device_context();
if (ctx == NULL) {
    return;  // No cleanup!
}
```

### Deleting Timers and Semaphores

Always delete/destroy FreeRTOS objects when cleaning up.

```c
// Correct - Timer deletion
static esp_timer_handle_t s_heartbeat_timer = NULL;

esp_err_t start_heartbeat(void) {
    esp_timer_create_args_t timer_args = {
        .callback = heartbeat_callback,
        .arg = NULL,
        .name = "heartbeat_timer",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_heartbeat_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    return esp_timer_start_periodic(s_heartbeat_timer, 10000);  // 10ms
}

void stop_heartbeat(void) {
    if (s_heartbeat_timer != NULL) {
        esp_timer_delete(s_heartbeat_timer);
        s_heartbeat_timer = NULL;
    }
}

// Correct - Semaphore deletion
static SemaphoreHandle_t s_state_mutex = NULL;

esp_err_t init_state_mutex(void) {
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void destroy_state_mutex(void) {
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
}

// Incorrect - Resource leak
esp_err_t start_heartbeat(void) {
    esp_timer_create(&timer_args, &s_heartbeat_timer);
    return esp_timer_start_periodic(s_heartbeat_timer, 10000);
}

void cleanup(void) {
    // s_heartbeat_timer not deleted!
}
```

### Closing NVS Handles

Always close NVS handles to free internal resources.

```c
// Correct - Open and close NVS
esp_err_t read_nvs_value(const char *key, uint32_t *value) {
    nvs_handle_t nvs_handle;

    esp_err_t ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_get_u32(nvs_handle, key, value);

    nvs_close(nvs_handle);  // Always close, even on error

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read NVS value: %s", esp_err_to_name(ret));
    }

    return ret;
}

// Incorrect - NVS handle leak
esp_err_t read_nvs_value(const char *key, uint32_t *value) {
    nvs_handle_t nvs_handle;

    esp_err_t ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_u32(nvs_handle, key, value);
    return ret;  // nvs_handle not closed!
}
```

---

## Thread Safety Guidelines

The gateway uses FreeRTOS with multiple concurrent tasks. Thread safety is mandatory.

### Using Mutexes for Shared State

Protect all shared state with mutexes. Verify mutex creation succeeded.

```c
// Correct - Mutex-protected state
static SemaphoreHandle_t s_device_mutex = NULL;
static device_list_t s_devices = {0};

esp_err_t init_device_list(void) {
    s_device_mutex = xSemaphoreCreateMutex();
    if (s_device_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create device mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint16_t get_device_count(void) {
    uint16_t count = 0;

    if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        count = s_devices.count;
        xSemaphoreGive(s_device_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire device mutex");
    }

    return count;
}

esp_err_t add_device(const zb_device_t *device) {
    if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire device mutex");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (s_devices.count >= ZB_MAX_DEVICES) {
        ESP_LOGW(TAG, "Device list full");
        ret = ESP_ERR_NO_MEM;
    } else {
        memcpy(&s_devices.devices[s_devices.count], device, sizeof(zb_device_t));
        s_devices.count++;
    }

    xSemaphoreGive(s_device_mutex);
    return ret;
}

// Incorrect - No mutex protection
static device_list_t s_devices = {0};

uint16_t get_device_count(void) {
    return s_devices.count;  // Race condition!
}

void add_device(const zb_device_t *device) {
    s_devices.devices[s_devices.count] = *device;  // Race condition!
    s_devices.count++;
}
```

### Timeout Constants with pdMS_TO_TICKS()

Always use named constants with `pdMS_TO_TICKS()` for timeouts. Never hardcode milliseconds.

```c
// In gateway_defaults.h
#define GW_DEFAULT_MUTEX_TIMEOUT_MS 1000
#define GW_MQTT_LOCK_TIMEOUT_MS 2000
#define GW_ZB_LOCK_TIMEOUT_MS 3000

// Correct - Named constant with pdMS_TO_TICKS
if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GW_DEFAULT_MUTEX_TIMEOUT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "Mutex acquisition timeout");
    return ESP_ERR_TIMEOUT;
}

// Incorrect - Magic number without conversion
if (xSemaphoreTake(s_mutex, 1000) != pdTRUE) {
    ESP_LOGE(TAG, "Mutex acquisition timeout");
    return ESP_ERR_TIMEOUT;
}

// Incorrect - No timeout constant
if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
}
```

### Zigbee API Lock Acquisition

All Zigbee ZCL API calls from MQTT/WiFi tasks must be protected with locks. This prevents stack conflicts.

```c
// Correct - Zigbee API call with lock
esp_err_t send_on_off_command(uint16_t device_id, bool on) {
    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_hdr.frame_type = ZCL_FRAME_TYPE_CLUSTER_SPECIFIC,
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP,
        .endpoint = 1,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        .commandID = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON : ESP_ZB_ZCL_CMD_ON_OFF_OFF,
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send ON/OFF command: %s", esp_err_to_name(ret));
    }

    return ret;
}

// Correct - Attribute write with lock
esp_err_t write_attribute(uint16_t device_id, uint8_t endpoint,
                          uint16_t cluster_id, uint16_t attr_id,
                          uint8_t attr_type, uint8_t *attr_value) {
    esp_zb_zcl_write_attr_cmd_t cmd = {
        .zcl_basic_hdr.frame_type = ZCL_FRAME_TYPE_GENERAL,
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP,
        .u.std_cmd = {
            .src_endpoint = ESP_ZB_DEFAULT_ENDP,
            .dst_endpoint = endpoint,
            .cluster_id = cluster_id,
        },
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_write_attr_cmd_req(&cmd);
    esp_zb_lock_release();

    return ret;
}

// Incorrect - Zigbee API call without lock
esp_err_t send_on_off_command(uint16_t device_id, bool on) {
    esp_zb_zcl_on_off_cmd_t cmd = { /* ... */ };
    return esp_zb_zcl_on_off_cmd_req(&cmd);  // Not thread-safe!
}
```

---

## Code Formatting

Consistent formatting improves readability and reduces merge conflicts.

### Indentation and Line Length

- Use **4 spaces** per indentation level (no tabs)
- Limit lines to **100 characters** maximum
- Use automatic formatting tool: `clang-format`

### Using clang-format

The project uses Google-based code style via `.clang-format` configuration.

```bash
# Format a single file
clang-format -i main/core/mqtt_bridge.c

# Format all C and H files in a directory
find main/core -name "*.c" -o -name "*.h" | xargs clang-format -i

# Format entire main directory
find main -name "*.c" -o -name "*.h" | xargs clang-format -i

# Check formatting without modifying (CI usage)
clang-format --dry-run --Werror main/core/mqtt_bridge.c
```

### Formatting Examples

```c
// Correct - Properly indented and within 100 chars
esp_err_t mqtt_bridge_init(const mqtt_config_t *config) {
    esp_err_t ret = ESP_OK;

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Long parameter list breaks across lines, aligned
    ret = mqtt_client_connect(config->broker_addr,
                              config->broker_port,
                              config->username,
                              config->password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT connection failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

// Long conditional statement
if (device->type == ZB_DEVICE_LIGHT &&
    device->state.on == true &&
    device->battery_level < 20) {
    ESP_LOGW(TAG, "Low battery warning for light 0x%04x", device->id);
}

// Function pointer with proper formatting
typedef esp_err_t (*device_command_handler_t)(const zb_device_t *device,
                                               const command_payload_t *cmd);
```

---

## Documentation

Clear documentation helps other developers understand code intent and usage.

### Doxygen Style Comments for Public Functions

All public functions (non-static) must have Doxygen-style documentation.

```c
/**
 * @brief Initialize the MQTT bridge
 *
 * Initializes the MQTT bridge and connects to the configured broker.
 * Must be called once during startup before publishing any messages.
 *
 * @param config Pointer to MQTT configuration structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if config is NULL
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_TIMEOUT if connection times out
 *
 * @note This function is thread-safe and can be called from any task
 *
 * @see mqtt_bridge_publish(), mqtt_bridge_deinit()
 */
esp_err_t mqtt_bridge_init(const mqtt_config_t *config);

/**
 * @brief Publish a message to MQTT topic
 *
 * Publishes a JSON payload to the specified MQTT topic with the given QoS level.
 * The function is asynchronous; the message is queued for transmission.
 *
 * @param[in] topic MQTT topic path (e.g., "home/kitchen/light")
 * @param[in] payload JSON payload (will be deep-copied)
 * @param[in] qos Quality of service (0, 1, or 2)
 * @return ESP_OK on successful queuing
 * @return ESP_ERR_INVALID_ARG if topic or payload is NULL
 * @return ESP_ERR_NO_MEM if message queue is full
 *
 * @warning QoS level 2 is not recommended for high-frequency updates
 */
esp_err_t mqtt_bridge_publish(const char *topic, const cJSON *payload, int qos);

/**
 * @brief Enumerate all Zigbee devices
 *
 * Iterates through all registered Zigbee devices and calls the provided
 * callback for each device. Useful for state reporting and device discovery.
 *
 * @param[in] callback Function pointer to call for each device
 * @param[in] user_data Optional pointer passed to callback
 * @return ESP_OK if enumeration completed
 * @return ESP_ERR_INVALID_ARG if callback is NULL
 *
 * @note Callback is called under device mutex lock; keep processing minimal
 * @note Callback must not call device functions that acquire the mutex
 */
esp_err_t zb_device_enumerate(device_callback_t callback, void *user_data);
```

### Function Comment Guidelines

For each public function, document:
- **@brief**: One-line summary of purpose
- **@param[in]**: Input parameters and their meanings
- **@param[out]**: Output parameters (pointers modified by function)
- **@return**: All possible return values and their meanings
- **@note**: Important usage notes or warnings
- **@warning**: Dangerous behaviors or common mistakes
- **@see**: Related functions

### Comments for Complex Code Sections

Explain the "why", not the "what":

```c
// Correct - Explains intent
// Use exponential backoff to reduce MQTT broker load during connection storms
uint32_t backoff_ms = MIN_BACKOFF_MS << attempt_count;
if (backoff_ms > MAX_BACKOFF_MS) {
    backoff_ms = MAX_BACKOFF_MS;
}
vTaskDelay(pdMS_TO_TICKS(backoff_ms));

// Incorrect - States the obvious
// Delay for backoff_ms milliseconds
vTaskDelay(pdMS_TO_TICKS(backoff_ms));

// Correct - Complex algorithm explanation
// Sort devices by RSSI (signal strength) to optimize scanning order:
// Strong signals are scanned more frequently to reduce latency for
// responsive devices, while weak signals are scanned less often to
// conserve power
qsort(scan_queue, scan_count, sizeof(ble_scan_device_t),
      ble_device_rssi_comparator);
```

---

## Code Quality Rules

These rules enforce consistency and prevent common bugs.

### No Magic Numbers

Use named constants for all numeric literals. This makes code self-documenting and easier to maintain.

```c
// In gateway_defaults.h
#define GW_BUFFER_JSON_MAX 4096
#define GW_BUFFER_MQTT_TOPIC 256
#define ZB_BATTERY_LOW_THRESHOLD 20
#define BLE_RSSI_THRESHOLD -70
#define MQTT_RECONNECT_DELAY_MS 5000

// Correct - Uses named constants
char topic_buf[GW_BUFFER_MQTT_TOPIC];
if (device->battery_level < ZB_BATTERY_LOW_THRESHOLD) {
    ESP_LOGW(TAG, "Low battery");
}
if (rssi < BLE_RSSI_THRESHOLD) {
    return false;  // Device too weak
}
vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));

// Incorrect - Magic numbers
char topic_buf[256];
if (device->battery_level < 20) {
    ESP_LOGW(TAG, "Low battery");
}
if (rssi < -70) {
    return false;
}
vTaskDelay(pdMS_TO_TICKS(5000));
```

### No Code Duplication

Extract common patterns into helper functions to improve maintainability.

```c
// Correct - Common pattern extracted
static esp_err_t zb_zcl_send_cmd_with_lock(esp_zb_zcl_cmd_t *cmd) {
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_cmd_req(cmd);
    esp_zb_lock_release();
    return ret;
}

esp_err_t turn_on_light(uint16_t device_id) {
    esp_zb_zcl_on_off_cmd_t cmd = {
        .commandID = ESP_ZB_ZCL_CMD_ON_OFF_ON,
        // ... rest of config
    };
    return zb_zcl_send_cmd_with_lock((esp_zb_zcl_cmd_t *)&cmd);
}

esp_err_t set_brightness(uint16_t device_id, uint8_t level) {
    esp_zb_zcl_level_cmd_t cmd = {
        .commandID = ESP_ZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL,
        // ... rest of config
    };
    return zb_zcl_send_cmd_with_lock((esp_zb_zcl_cmd_t *)&cmd);
}

// Incorrect - Duplicated pattern
esp_err_t turn_on_light(uint16_t device_id) {
    esp_zb_zcl_on_off_cmd_t cmd = { /* ... */ };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_cmd_req((esp_zb_zcl_cmd_t *)&cmd);
    esp_zb_lock_release();
    return ret;
}

esp_err_t set_brightness(uint16_t device_id, uint8_t level) {
    esp_zb_zcl_level_cmd_t cmd = { /* ... */ };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_cmd_req((esp_zb_zcl_cmd_t *)&cmd);
    esp_zb_lock_release();
    return ret;
}
```

### Resource Management on All Paths

Ensure resources are cleaned up on both success and failure paths.

```c
// Correct - All paths cleaned up
esp_err_t load_device_config(const char *config_file) {
    FILE *fp = NULL;
    cJSON *config = NULL;
    char *file_data = NULL;

    fp = fopen(config_file, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open config file");
        return ESP_ERR_NOT_FOUND;
    }

    // Read file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    file_data = malloc(file_size + 1);
    if (file_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_size = fread(file_data, 1, file_size, fp);
    fclose(fp);
    fp = NULL;

    if (read_size != (size_t)file_size) {
        ESP_LOGE(TAG, "Failed to read file completely");
        free(file_data);
        return ESP_ERR_INVALID_STATE;
    }

    // Parse JSON
    file_data[file_size] = '\0';
    config = cJSON_Parse(file_data);
    free(file_data);
    file_data = NULL;

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid JSON in config file");
        return ESP_ERR_INVALID_ARG;
    }

    // Use config
    esp_err_t ret = process_config(config);
    cJSON_Delete(config);

    return ret;
}

// Incorrect - Resource leak on error
esp_err_t load_device_config(const char *config_file) {
    FILE *fp = fopen(config_file, "r");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *file_data = malloc(file_size + 1);
    if (file_data == NULL) {
        return ESP_ERR_NO_MEM;  // fp not closed!
    }

    fread(file_data, 1, file_size, fp);
    fclose(fp);

    cJSON *config = cJSON_Parse(file_data);
    free(file_data);
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    process_config(config);
    cJSON_Delete(config);
}
```

---

## Summary Checklist

Before submitting code for review, verify:

- [ ] All functions use `snake_case` naming
- [ ] All constants use `UPPER_CASE` naming
- [ ] All static variables use `s_` prefix (not `g_`)
- [ ] All `esp_err_t` returns are checked
- [ ] All malloc/cJSON allocations are checked for NULL
- [ ] Error paths properly free resources (goto cleanup pattern)
- [ ] All shared state protected by mutexes
- [ ] All Zigbee API calls protected by `esp_zb_lock_acquire()`
- [ ] Module TAG defined and used correctly
- [ ] Timeout constants used with `pdMS_TO_TICKS()`
- [ ] All public functions documented with Doxygen
- [ ] Code formatted with `clang-format`
- [ ] No magic numbers (use named constants)
- [ ] No code duplication (extract to helpers)
- [ ] Comments explain "why", not "what"

---

## Related Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - System architecture and module design
- [API_REFERENCE.md](API_REFERENCE.md) - Public API documentation
- [DEVELOPMENT.md](DEVELOPMENT.md) - Development environment and build process
