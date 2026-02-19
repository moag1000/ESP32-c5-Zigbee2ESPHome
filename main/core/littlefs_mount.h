/**
 * @file littlefs_mount.h
 * @brief Shared LittleFS Mount Utility
 */
#ifndef LITTLEFS_MOUNT_H
#define LITTLEFS_MOUNT_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief LittleFS mount point */
#define LITTLEFS_MOUNT_POINT "/littlefs"

/** @brief LittleFS partition label (reuses existing "spiffs" partition) */
#define LITTLEFS_PARTITION_LABEL "spiffs"

/**
 * @brief Mount LittleFS filesystem
 * Uses reference counting - safe to call multiple times.
 * @return ESP_OK on success
 */
esp_err_t littlefs_mount(void);

/**
 * @brief Unmount LittleFS filesystem
 * Decrements reference counter; actual unmount when counter reaches 0.
 * @return ESP_OK on success
 */
esp_err_t littlefs_unmount(void);

/**
 * @brief Check if LittleFS is mounted
 * @return true if mounted
 */
bool littlefs_is_mounted(void);

/**
 * @brief Get filesystem usage info
 * @param[out] total Total bytes
 * @param[out] used Used bytes
 * @return ESP_OK on success
 */
esp_err_t littlefs_get_info(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif

#endif /* LITTLEFS_MOUNT_H */
