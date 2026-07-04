/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_ESP_FS_VFS_NAME "esp-fatfs"

/**
 * Register a SQLite VFS which uses ESP-IDF's POSIX file API.
 *
 * The application must mount FATFS before calling this function. After it
 * succeeds, normal absolute FATFS paths can be passed to sqlite3_open(), for
 * example sqlite3_open("/fat_partition/data.db", &db).
 *
 * This VFS becomes SQLite's default VFS. It does not mount, unmount, format,
 * or take ownership of the FATFS volume.
 */
esp_err_t sqlite_fatfs_init(void);

/**
 * Unregister the FATFS VFS and shut SQLite down.
 *
 * All SQLite connections opened through this VFS must be closed first. The
 * FATFS volume remains mounted and is still owned by the application.
 */
esp_err_t sqlite_fatfs_deinit(void);

#ifdef __cplusplus
}
#endif
