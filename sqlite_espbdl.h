/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include "esp_blockdev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_ESPBDL_VFS_NAME "espbdl"

/**
 * Register the SQLite BDL VFS and transfer ownership of *handle to it.
 *
 * The complete device is used by one SQLite database.  On success *handle is
 * set to ESP_BLOCKDEV_HANDLE_INVALID.  The VFS releases the device from
 * sqlite_espbdl_deinit().  Call this before opening SQLite databases.
 *
 * If neither redundant on-device metadata record is valid, the device is
 * initialized as an empty SQLite store.
 */
esp_err_t sqlite_espbdl_init(esp_blockdev_handle_t *handle);

/**
 * Unregister the VFS, shut SQLite down, and release all preallocated resources
 * and the owned BDL handle. All SQLite connections must be closed first.
 * Returns ESP_ERR_INVALID_STATE while BDL-backed SQLite files remain open.
 */
esp_err_t sqlite_espbdl_deinit(void);

/** Return the usable main-database and WAL/journal capacities. */
esp_err_t sqlite_espbdl_get_capacity(uint64_t *database_bytes,
                                     uint64_t *wal_bytes);


#ifdef __cplusplus
}
#endif
