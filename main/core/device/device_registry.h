/**
 * @file device_registry.h
 * @brief Device Registry for ESP32-C5 Zigbee2MQTT NG
 *
 * Provides a unified device registry with PSRAM-backed storage for managing
 * devices across protocols (Zigbee, BLE, virtual). Features include:
 *   - O(1) lookup by device_id using hash table
 *   - O(n) lookup by friendly_name and short_addr
 *   - Thread-safe operations with mutex protection
 *   - Per-device cJSON state storage
 *   - Memory-efficient PSRAM allocation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "esp_err.h"
#include "unified_device.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/**
 * @brief Maximum number of devices in the registry
 *
 * Override via CONFIG_DEVICE_REGISTRY_MAX_DEVICES in Kconfig.
 */
#ifndef CONFIG_DEVICE_REGISTRY_MAX_DEVICES
#define DEVICE_REGISTRY_MAX_DEVICES     64
#else
#define DEVICE_REGISTRY_MAX_DEVICES     CONFIG_DEVICE_REGISTRY_MAX_DEVICES
#endif

/**
 * @brief Hash table size for O(1) device lookup
 *
 * Should be power of 2 for fast modulo operation.
 */
#define DEVICE_REGISTRY_HASH_SIZE       128

/**
 * @brief Mutex timeout in milliseconds
 */
#define DEVICE_REGISTRY_MUTEX_TIMEOUT_MS  100

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Device iterator callback function type
 *
 * Called for each device during iteration. Return false to stop iteration.
 *
 * @param[in] dev Pointer to device (do not modify directly)
 * @param[in,out] ctx User-provided context
 * @return true to continue iteration, false to stop
 */
typedef bool (*device_iterator_fn)(device_t *dev, void *ctx);

/**
 * @brief Device registry statistics
 */
typedef struct {
    size_t device_count;        /**< Current number of devices */
    size_t max_devices;         /**< Maximum devices allowed */
    size_t memory_used;         /**< Bytes used for registry + states */
    size_t zigbee_count;        /**< Number of Zigbee devices */
    size_t ble_count;           /**< Number of BLE devices */
    size_t online_count;        /**< Number of online devices */
} device_registry_stats_t;

/* ============================================================================
 * Initialization / Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize the device registry
 *
 * Allocates device array and hash table in PSRAM, creates mutex.
 * Must be called before any other registry functions.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t device_registry_init(void);

/**
 * @brief Deinitialize the device registry
 *
 * Frees all devices, states, and internal structures.
 * Safe to call even if not initialized.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t device_registry_deinit(void);

/**
 * @brief Clear all devices from the registry
 *
 * Removes all devices from the registry without deinitializing.
 * Frees all device states, clears hash table, resets device count.
 * The registry remains initialized and ready for new devices.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_clear_all(void);

/**
 * @brief Check if registry is initialized
 *
 * @return true if initialized, false otherwise
 */
bool device_registry_is_initialized(void);

/* ============================================================================
 * Device Management
 * ============================================================================ */

/**
 * @brief Add a new device to the registry
 *
 * Creates a new device with the given ID and protocol. The device is
 * initialized with default values; caller should populate other fields.
 *
 * @param[in] id Unique device identifier
 * @param[in] proto Device protocol (Zigbee, BLE, virtual)
 * @return Pointer to newly created device, or NULL on failure
 *
 * @note Returns NULL if:
 *       - Registry is full (max devices reached)
 *       - Device with same ID already exists
 *       - Memory allocation fails
 *       - Mutex timeout
 */
device_t *device_registry_add(device_id_t id, device_protocol_t proto);

/**
 * @brief Get device by ID (O(1) lookup)
 *
 * @param[in] id Device identifier
 * @return Pointer to device, or NULL if not found
 *
 * @note Do not free the pointer — it points into the registry's device array.
 *
 * @warning The mutex is only held for the lookup itself, not for your use of
 *          the result. The pointer stays valid memory (the array is allocated
 *          once and never freed), but the *contents* are not yours:
 *
 *          - device_registry_remove() zeroes the slot. Called from
 *            zb_leave_helper.c on the Zigbee task.
 *          - the slot is later reused for a different device, at which point
 *            your pointer silently refers to that other device instead.
 *
 *          Holding one of these across a blocking call, an event dispatch or a
 *          task switch is therefore unsafe. Either use it immediately, or
 *          re-fetch by ID after any blocking operation. Snapshot IDs with
 *          device_registry_snapshot_ids() when you need to walk the registry.
 *
 *          The proper fix is a handle-based API that returns values instead of
 *          interior pointers; that is not done yet — there are ~127 call sites.
 */
device_t *device_registry_get(device_id_t id);

/**
 * @brief Get device by friendly name (O(n) lookup)
 *
 * @param[in] friendly_name Device name to search for
 * @return Pointer to first matching device, or NULL if not found
 *
 * @note String comparison is case-sensitive.
 */
device_t *device_registry_get_by_name(const char *friendly_name);

/**
 * @brief Find device by friendly_name or IEEE address string
 *
 * Searches for a device by trying:
 *   1. friendly_name lookup
 *   2. IEEE address string parsing (format: "0x00124b001234abcd")
 *
 * @param[in] id_str Device identifier (friendly_name or IEEE string)
 * @return Pointer to matching device, or NULL if not found
 */
device_t *device_registry_find_by_id(const char *id_str);

/**
 * @brief Thread-safe copy of device friendly_name
 *
 * Copies the friendly_name of a device to the provided buffer.
 *
 * @param[in] id Device identifier
 * @param[out] buf Buffer to copy friendly_name into
 * @param[in] buf_len Size of buffer
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if buf is NULL or buf_len is 0
 *      - ESP_ERR_NOT_FOUND if device not in registry
 */
esp_err_t device_registry_get_friendly_name(device_id_t id, char *buf, size_t buf_len);

/**
 * @brief Get Zigbee device by short address (O(n) lookup)
 *
 * @param[in] short_addr Zigbee network short address
 * @return Pointer to matching Zigbee device, or NULL if not found
 *
 * @note Only searches Zigbee devices.
 */
device_t *device_registry_get_by_short_addr(uint16_t short_addr);

/**
 * @brief Remove a device from the registry
 *
 * Frees the device and its associated state JSON.
 *
 * @param[in] id Device identifier
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_remove(device_id_t id);

/* ============================================================================
 * State Management
 * ============================================================================ */

/**
 * @brief Get device state JSON
 *
 * Returns the current state JSON object for a device. The state contains
 * dynamic values like sensor readings, switch states, etc.
 *
 * @param[in] id Device identifier
 * @return Pointer to state cJSON object, or NULL if not found
 *
 * @note Do NOT free or delete the returned cJSON object. It is owned
 *       by the registry.
 *
 * @warning The returned pointer is only safe while no other task can replace
 *          or remove this device's state. device_registry_set_state() and
 *          device_registry_remove() both call cJSON_Delete() on the old
 *          object, which turns any pointer another task is still holding into
 *          a dangling one.
 *
 *          Only use this from the same task that owns state updates (the
 *          Zigbee task). From event handlers, the ESPHome server or MQTT
 *          callbacks, use device_registry_state_dup() instead.
 */
cJSON *device_registry_get_state(device_id_t id);

/**
 * @brief Get an owned copy of a device's state JSON
 *
 * Duplicates the state while the registry mutex is held, so the caller ends up
 * with an object nobody else can free underneath them. This is the safe
 * variant for cross-task readers.
 *
 * @param[in] id Device identifier
 * @return Newly allocated cJSON object, or NULL if the device has no state.
 *         The caller owns it and must cJSON_Delete() it.
 */
cJSON *device_registry_state_dup(device_id_t id);

/**
 * @brief Set device state JSON (takes ownership)
 *
 * Replaces the device's state JSON with the provided object.
 * The registry takes ownership of the cJSON object.
 *
 * @param[in] id Device identifier
 * @param[in] state New state JSON (ownership transferred to registry)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if state is NULL
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 *
 * @note The previous state is automatically freed.
 * @note Do NOT use the state pointer after calling this function.
 */
esp_err_t device_registry_set_state(device_id_t id, cJSON *state);

/**
 * @brief Update a single key in device state
 *
 * Adds or updates a single key-value pair in the device's state JSON.
 *
 * @param[in] id Device identifier
 * @param[in] key State key to update
 * @param[in] value New value (ownership transferred to registry)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if key or value is NULL
 *      - ESP_ERR_NO_MEM if state object creation fails
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 *
 * @note Creates state object if it doesn't exist.
 */
esp_err_t device_registry_update_state(device_id_t id, const char *key, cJSON *value);

/**
 * @brief Merge partial update into device state
 *
 * Merges keys from the partial JSON object into the device's state.
 * Existing keys are updated, new keys are added.
 *
 * @param[in] id Device identifier
 * @param[in] partial Partial state to merge (NOT consumed, caller must free)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if partial is NULL
 *      - ESP_ERR_NO_MEM if state object creation fails
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_merge_state(device_id_t id, cJSON *partial);

/* ============================================================================
 * Iteration
 * ============================================================================ */

/**
 * @brief Iterate over all devices with callback
 *
 * Calls the iterator function for each device in the registry.
 * Iteration can be stopped early by returning false from the callback.
 *
 * WARNING: Callback is invoked with registry mutex held.
 * Callbacks MUST NOT block on external I/O (MQTT, NVS, etc.)
 * or call functions that acquire other mutexes (deadlock risk).
 * For I/O-bound operations, use device_registry_collect_ids() instead.
 *
 * @param[in] fn Iterator callback function
 * @param[in,out] ctx User context passed to callback
 *
 * @note Holds mutex for entire iteration. Keep callbacks fast.
 * @note The registry mutex is recursive, so calling other registry functions
 *       from the callback is safe. Taking a DIFFERENT module's mutex from in
 *       here is not — that is a real lock-order inversion.
 */
void device_registry_iterate(device_iterator_fn fn, void *ctx);

/**
 * @brief Iterate over Zigbee devices only
 *
 * Calls the iterator function for each Zigbee device in the registry.
 * Iteration can be stopped early by returning false from the callback.
 *
 * WARNING: Callback is invoked with registry mutex held.
 * Callbacks MUST NOT block on external I/O (MQTT, NVS, etc.)
 * or call functions that acquire other mutexes (deadlock risk).
 * For I/O-bound operations, use device_registry_collect_ids() instead.
 *
 * @param[in] fn Iterator callback function
 * @param[in,out] ctx User context passed to callback
 * @return Number of Zigbee devices iterated
 *
 * @note Holds mutex for entire iteration. Keep callbacks fast.
 * @note The registry mutex is recursive, so calling other registry functions
 *       from the callback is safe. Taking a DIFFERENT module's mutex from in
 *       here is not — that is a real lock-order inversion.
 */
size_t device_registry_iterate_zigbee(device_iterator_fn fn, void *ctx);

/**
 * @brief Collect device IDs without holding mutex during callback
 *
 * Copies active device IDs into caller's array, then releases mutex.
 * Use this instead of device_registry_iterate() when callbacks may block
 * on external I/O (MQTT publish, NVS writes, etc.).
 *
 * Typical usage:
 * @code
 *   device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
 *   size_t count = 0;
 *   device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &count);
 *   for (size_t i = 0; i < count; i++) {
 *       device_t *dev = device_registry_get(ids[i]);
 *       if (dev) {
 *           // Safe to do blocking I/O here
 *       }
 *   }
 * @endcode
 *
 * @param[out] ids Array to fill with device IDs
 * @param[in] max_ids Maximum number of IDs to collect
 * @param[out] count Number of IDs actually collected
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if ids or count is NULL
 *      - ESP_ERR_INVALID_STATE if registry not initialized
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_collect_ids(device_id_t *ids, size_t max_ids, size_t *count);

/**
 * @brief Take a heap snapshot of all active device IDs
 *
 * Convenience wrapper around device_registry_collect_ids() that allocates the
 * ID buffer from PSRAM instead of the caller's stack. A full ID array is 512
 * bytes, which is a lot to put on a 4KB task stack.
 *
 * This is the correct way to walk the registry when the loop body does
 * blocking work (NVS, MQTT, LittleFS). Do NOT use
 * device_registry_count() together with device_registry_get_by_index():
 * the count is read without the mutex and each get_by_index() takes it
 * separately, so a concurrent add or remove shifts the dense indices and the
 * loop silently skips or repeats devices.
 *
 * Devices may still disappear between the snapshot and the lookup — that is
 * expected. device_registry_get() then returns NULL and the entry is skipped.
 *
 * @param[out] out_count Number of IDs in the returned array
 * @return Array of device IDs, or NULL on allocation failure or empty registry.
 *         Release with device_registry_release_ids().
 *
 * Typical usage:
 * @code
 *   size_t n = 0;
 *   device_id_t *ids = device_registry_snapshot_ids(&n);
 *   for (size_t i = 0; i < n; i++) {
 *       device_t *dev = device_registry_get(ids[i]);
 *       if (!dev) continue;
 *       // blocking work is fine here
 *   }
 *   device_registry_release_ids(ids);
 * @endcode
 */
device_id_t *device_registry_snapshot_ids(size_t *out_count);

/**
 * @brief Release an ID array returned by device_registry_snapshot_ids()
 * @param[in] ids Array to free (NULL is allowed)
 */
void device_registry_release_ids(device_id_t *ids);

/**
 * @brief Get number of devices in registry
 *
 * @return Device count
 */
size_t device_registry_count(void);

/**
 * @brief Get device by index (O(1) lookup)
 *
 * Returns device at the specified index. Index range is 0 to device_registry_count()-1.
 * Note: Indices are not stable across add/remove operations.
 *
 * @param[in] index Device index (0-based)
 * @return Pointer to device, or NULL if index out of range
 *
 * @note The returned pointer is valid until the device is removed.
 *       Do not free the pointer.
 */
device_t *device_registry_get_by_index(size_t index);

/* ============================================================================
 * Availability Updates
 * ============================================================================ */

/**
 * @brief Set device availability status
 *
 * @param[in] id Device identifier
 * @param[in] avail New availability status
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_set_availability(device_id_t id, device_availability_t avail);

/**
 * @brief Update device last_seen timestamp to current time
 *
 * @param[in] id Device identifier
 *
 * @note Silently fails if device not found.
 */
void device_registry_update_last_seen(device_id_t id);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * @brief Get registry statistics
 *
 * @param[out] stats Statistics structure to fill
 *
 * @note stats must not be NULL
 */
void device_registry_get_stats(device_registry_stats_t *stats);

/**
 * @brief Print registry statistics to log
 */
void device_registry_print_stats(void);

/* ============================================================================
 * Converter Management (Zigbee devices)
 * ============================================================================ */

/**
 * @brief Set converter for a Zigbee device
 *
 * Associates a converter definition with a Zigbee device. The converter
 * defines how to translate between Zigbee ZCL and JSON/MQTT.
 *
 * @param[in] id Device identifier
 * @param[in] converter Pointer to zb_converter_def_t (not copied, must remain valid)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if device is not Zigbee protocol
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 *
 * @note The converter pointer is stored directly; the caller must ensure
 *       the converter definition remains valid for the device lifetime.
 */
esp_err_t device_registry_set_converter(device_id_t id, const void *converter);

/**
 * @brief Get converter for a Zigbee device
 *
 * @param[in] id Device identifier
 * @return Pointer to converter definition, or NULL if not set or device not found
 */
const void *device_registry_get_converter(device_id_t id);

/* ============================================================================
 * Device Attribute Setters
 * ============================================================================ */

/**
 * @brief Set device friendly name
 *
 * Updates the friendly_name field of the specified device.
 *
 * @param[in] id Device identifier
 * @param[in] name New friendly name (max 31 chars, will be truncated)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if name is NULL
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_set_friendly_name(device_id_t id, const char *name);

/**
 * @brief Add a cluster to a Zigbee device
 *
 * Adds a cluster ID to the device's cluster list. Ignores duplicates.
 *
 * @param[in] id Device identifier
 * @param[in] cluster_id Cluster ID to add
 * @return
 *      - ESP_OK on success (including if cluster already exists)
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if device is not Zigbee protocol
 *      - ESP_ERR_NO_MEM if cluster list is full
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_add_cluster(device_id_t id, uint16_t cluster_id);

/**
 * @brief Set device online status
 *
 * Convenience function that sets availability to ONLINE or OFFLINE.
 *
 * @param[in] id Device identifier
 * @param[in] online True for online, false for offline
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_TIMEOUT if mutex acquisition fails
 */
esp_err_t device_registry_set_online(device_id_t id, bool online);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REGISTRY_H */
