/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sqlite_esp_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sqlite3.h"

#ifndef SQLITE_ESP_FS_MAX_DATABASES
#define SQLITE_ESP_FS_MAX_DATABASES 2
#endif

#ifndef SQLITE_ESP_FS_SHM_PAGES
#define SQLITE_ESP_FS_SHM_PAGES 1
#endif

#ifndef SQLITE_ESP_FS_MUTEX_POOL_SIZE
#define SQLITE_ESP_FS_MUTEX_POOL_SIZE 16
#endif

#define FS_PATH_MAX 256
#define FS_SHM_PAGE_SIZE 32768
#define FS_SHM_LOCKS SQLITE_SHM_NLOCK

typedef struct fs_file fs_file_t;

typedef struct {
    bool in_use;
    char path[FS_PATH_MAX];
    int main_refs;
    int shm_refs;
    int shared_locks;
    fs_file_t *reserved_owner;
    fs_file_t *pending_owner;
    fs_file_t *exclusive_owner;
    int shm_page_count;
    int shm_shared[FS_SHM_LOCKS];
    fs_file_t *shm_exclusive[FS_SHM_LOCKS];
    uint8_t *shm;
} fs_database_t;

typedef struct {
    SemaphoreHandle_t mutex;
    int open_count;
    fs_database_t databases[SQLITE_ESP_FS_MAX_DATABASES];
    uint8_t *shm_storage;
} fs_vfs_ctx_t;

struct fs_file {
    sqlite3_file base;
    fs_vfs_ctx_t *ctx;
    fs_database_t *database;
    int fd;
    int open_flags;
    int lock_level;
    uint32_t shm_shared_mask;
    uint32_t shm_exclusive_mask;
    bool has_shared_lock;
    bool shm_attached;
    bool delete_on_close;
    char path[FS_PATH_MAX];
};

static const char *TAG = "sqlite_fatfs";
static fs_vfs_ctx_t *s_ctx;
static sqlite3_vfs s_vfs;

typedef struct {
    SemaphoreHandle_t handle;
    StaticSemaphore_t storage;
    bool is_static;
    bool in_use;
} fs_sqlite_mutex_t;

#define FS_STATIC_MUTEX_FIRST SQLITE_MUTEX_STATIC_MAIN
#define FS_STATIC_MUTEX_LAST SQLITE_MUTEX_STATIC_VFS3
#define FS_STATIC_MUTEX_COUNT (FS_STATIC_MUTEX_LAST - FS_STATIC_MUTEX_FIRST + 1)

static fs_sqlite_mutex_t s_static_mutexes[FS_STATIC_MUTEX_COUNT];
static fs_sqlite_mutex_t s_dynamic_mutexes[SQLITE_ESP_FS_MUTEX_POOL_SIZE];
static volatile int s_mutex_init_state;

static int fs_mutex_init(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_mutex_init_state, &expected, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s_mutex_init_state, __ATOMIC_ACQUIRE) == 1) {
            taskYIELD();
        }
        return __atomic_load_n(&s_mutex_init_state, __ATOMIC_ACQUIRE) == 2
             ? SQLITE_OK : SQLITE_ERROR;
    }
    for (int i = 0; i < FS_STATIC_MUTEX_COUNT; ++i) {
        s_static_mutexes[i].is_static = true;
        s_static_mutexes[i].handle = xSemaphoreCreateRecursiveMutexStatic(
            &s_static_mutexes[i].storage);
        if (!s_static_mutexes[i].handle) goto fail;
    }
    for (int i = 0; i < SQLITE_ESP_FS_MUTEX_POOL_SIZE; ++i) {
        s_dynamic_mutexes[i].handle = xSemaphoreCreateRecursiveMutexStatic(
            &s_dynamic_mutexes[i].storage);
        if (!s_dynamic_mutexes[i].handle) goto fail;
    }
    __atomic_store_n(&s_mutex_init_state, 2, __ATOMIC_RELEASE);
    return SQLITE_OK;

fail:
    for (int i = 0; i < FS_STATIC_MUTEX_COUNT; ++i) {
        if (s_static_mutexes[i].handle) {
            vSemaphoreDelete(s_static_mutexes[i].handle);
            s_static_mutexes[i].handle = NULL;
        }
    }
    for (int i = 0; i < SQLITE_ESP_FS_MUTEX_POOL_SIZE; ++i) {
        if (s_dynamic_mutexes[i].handle) {
            vSemaphoreDelete(s_dynamic_mutexes[i].handle);
            s_dynamic_mutexes[i].handle = NULL;
        }
    }
    __atomic_store_n(&s_mutex_init_state, -1, __ATOMIC_RELEASE);
    return SQLITE_NOMEM;
}

static int fs_mutex_end(void)
{
    if (__atomic_exchange_n(&s_mutex_init_state, 0, __ATOMIC_ACQ_REL) == 2) {
        for (int i = 0; i < FS_STATIC_MUTEX_COUNT; ++i) {
            if (s_static_mutexes[i].handle) {
                vSemaphoreDelete(s_static_mutexes[i].handle);
                s_static_mutexes[i].handle = NULL;
            }
        }
        for (int i = 0; i < SQLITE_ESP_FS_MUTEX_POOL_SIZE; ++i) {
            if (s_dynamic_mutexes[i].handle) {
                vSemaphoreDelete(s_dynamic_mutexes[i].handle);
                s_dynamic_mutexes[i].handle = NULL;
            }
            s_dynamic_mutexes[i].in_use = false;
        }
    }
    return SQLITE_OK;
}

static sqlite3_mutex *fs_mutex_alloc(int type)
{
    if (type >= FS_STATIC_MUTEX_FIRST && type <= FS_STATIC_MUTEX_LAST) {
        fs_sqlite_mutex_t *m = &s_static_mutexes[type - FS_STATIC_MUTEX_FIRST];
        return m->handle ? (sqlite3_mutex *)m : NULL;
    }
    if (type != SQLITE_MUTEX_FAST && type != SQLITE_MUTEX_RECURSIVE) return NULL;
    for (int i = 0; i < SQLITE_ESP_FS_MUTEX_POOL_SIZE; ++i) {
        bool expected = false;
        if (__atomic_compare_exchange_n(&s_dynamic_mutexes[i].in_use,
                                        &expected, true, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return (sqlite3_mutex *)&s_dynamic_mutexes[i];
        }
    }
    return NULL;
}

static void fs_mutex_free(sqlite3_mutex *sqlite_mutex)
{
    if (!sqlite_mutex) return;
    fs_sqlite_mutex_t *m = (fs_sqlite_mutex_t *)sqlite_mutex;
    if (!m->is_static) __atomic_store_n(&m->in_use, false, __ATOMIC_RELEASE);
}

static void fs_mutex_enter(sqlite3_mutex *sqlite_mutex)
{
    fs_sqlite_mutex_t *m = (fs_sqlite_mutex_t *)sqlite_mutex;
    configASSERT(xSemaphoreTakeRecursive(m->handle, portMAX_DELAY) == pdTRUE);
}

static int fs_mutex_try(sqlite3_mutex *sqlite_mutex)
{
    fs_sqlite_mutex_t *m = (fs_sqlite_mutex_t *)sqlite_mutex;
    return xSemaphoreTakeRecursive(m->handle, 0) == pdTRUE
         ? SQLITE_OK : SQLITE_BUSY;
}

static void fs_mutex_leave(sqlite3_mutex *sqlite_mutex)
{
    fs_sqlite_mutex_t *m = (fs_sqlite_mutex_t *)sqlite_mutex;
    configASSERT(xSemaphoreGiveRecursive(m->handle) == pdTRUE);
}

static int fs_mutex_held(sqlite3_mutex *sqlite_mutex)
{
    fs_sqlite_mutex_t *m = (fs_sqlite_mutex_t *)sqlite_mutex;
    return xSemaphoreGetMutexHolder(m->handle) == xTaskGetCurrentTaskHandle();
}

static int fs_mutex_notheld(sqlite3_mutex *sqlite_mutex)
{
    return !fs_mutex_held(sqlite_mutex);
}

static const sqlite3_mutex_methods s_mutex_methods = {
    .xMutexInit = fs_mutex_init,
    .xMutexEnd = fs_mutex_end,
    .xMutexAlloc = fs_mutex_alloc,
    .xMutexFree = fs_mutex_free,
    .xMutexEnter = fs_mutex_enter,
    .xMutexTry = fs_mutex_try,
    .xMutexLeave = fs_mutex_leave,
    .xMutexHeld = fs_mutex_held,
    .xMutexNotheld = fs_mutex_notheld,
};

static inline void fs_lock(fs_vfs_ctx_t *ctx)
{
    configASSERT(xSemaphoreTakeRecursive(ctx->mutex, portMAX_DELAY) == pdTRUE);
}

static inline void fs_unlock(fs_vfs_ctx_t *ctx)
{
    configASSERT(xSemaphoreGiveRecursive(ctx->mutex) == pdTRUE);
}

static void release_file_locks(fs_file_t *file)
{
    fs_database_t *db = file->database;
    if (!db) return;
    if (db->exclusive_owner == file) db->exclusive_owner = NULL;
    if (db->pending_owner == file) db->pending_owner = NULL;
    if (db->reserved_owner == file) db->reserved_owner = NULL;
    if (file->has_shared_lock) {
        db->shared_locks--;
        file->has_shared_lock = false;
    }
    file->lock_level = SQLITE_LOCK_NONE;
}

static void release_shm_locks(fs_file_t *file)
{
    fs_database_t *db = file->database;
    if (!db) return;
    for (int i = 0; i < FS_SHM_LOCKS; ++i) {
        uint32_t bit = 1u << i;
        if (file->shm_shared_mask & bit) db->shm_shared[i]--;
        if ((file->shm_exclusive_mask & bit) && db->shm_exclusive[i] == file) {
            db->shm_exclusive[i] = NULL;
        }
    }
    file->shm_shared_mask = 0;
    file->shm_exclusive_mask = 0;
}

static void maybe_release_database(fs_database_t *db)
{
    if (db->main_refs == 0 && db->shm_refs == 0) {
        uint8_t *shm = db->shm;
        memset(db, 0, sizeof(*db));
        db->shm = shm;
        memset(shm, 0, SQLITE_ESP_FS_SHM_PAGES * FS_SHM_PAGE_SIZE);
    }
}

static fs_database_t *get_database(fs_vfs_ctx_t *ctx, const char *path)
{
    fs_database_t *free_slot = NULL;
    for (int i = 0; i < SQLITE_ESP_FS_MAX_DATABASES; ++i) {
        fs_database_t *db = &ctx->databases[i];
        if (db->in_use && strcmp(db->path, path) == 0) return db;
        if (!db->in_use && !free_slot) free_slot = db;
    }
    if (!free_slot) return NULL;
    free_slot->in_use = true;
    memcpy(free_slot->path, path, strlen(path) + 1);
    return free_slot;
}

static int fs_close(sqlite3_file *sqlite_file)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    int rc = SQLITE_OK;
    fs_lock(file->ctx);
    release_file_locks(file);
    release_shm_locks(file);
    if (file->shm_attached) {
        file->shm_attached = false;
        file->database->shm_refs--;
    }
    if (file->database) {
        file->database->main_refs--;
        maybe_release_database(file->database);
    }
    file->ctx->open_count--;
    fs_unlock(file->ctx);
    if (close(file->fd) != 0) rc = SQLITE_IOERR_CLOSE;
    if (file->delete_on_close && unlink(file->path) != 0 && errno != ENOENT) {
        rc = SQLITE_IOERR_DELETE;
    }
    file->base.pMethods = NULL;
    return rc;
}

static int fs_read(sqlite3_file *sqlite_file, void *dst, int amount,
                   sqlite3_int64 offset)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    uint8_t *out = dst;
    int remaining = amount;
    while (remaining > 0) {
        ssize_t n = pread(file->fd, out, (size_t)remaining, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return SQLITE_IOERR_READ;
        }
        if (n == 0) {
            memset(out, 0, (size_t)remaining);
            return SQLITE_IOERR_SHORT_READ;
        }
        out += n;
        offset += n;
        remaining -= (int)n;
    }
    return SQLITE_OK;
}

static int fs_write(sqlite3_file *sqlite_file, const void *src, int amount,
                    sqlite3_int64 offset)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    const uint8_t *in = src;
    int remaining = amount;
    while (remaining > 0) {
        ssize_t n = pwrite(file->fd, in, (size_t)remaining, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        }
        if (n == 0) return SQLITE_FULL;
        in += n;
        offset += n;
        remaining -= (int)n;
    }
    return SQLITE_OK;
}

static int fs_truncate(sqlite3_file *sqlite_file, sqlite3_int64 size)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    if (size < 0) return SQLITE_IOERR_TRUNCATE;
    return ftruncate(file->fd, (off_t)size) == 0
         ? SQLITE_OK : (errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_TRUNCATE);
}

static int fs_sync(sqlite3_file *sqlite_file, int flags)
{
    (void)flags;
    return fsync(((fs_file_t *)sqlite_file)->fd) == 0
         ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int fs_file_size(sqlite3_file *sqlite_file, sqlite3_int64 *size)
{
    struct stat st;
    if (fstat(((fs_file_t *)sqlite_file)->fd, &st) != 0) {
        return SQLITE_IOERR_FSTAT;
    }
    *size = st.st_size;
    return SQLITE_OK;
}

static int fs_file_lock(sqlite3_file *sqlite_file, int level)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    fs_database_t *db = file->database;
    if (!db || level <= file->lock_level) return SQLITE_OK;
    fs_lock(file->ctx);
    int rc = SQLITE_OK;
    bool acquire_shared = level >= SQLITE_LOCK_SHARED && !file->has_shared_lock;
    if (acquire_shared) {
        if ((db->exclusive_owner && db->exclusive_owner != file) ||
            (db->pending_owner && db->pending_owner != file)) {
            rc = SQLITE_BUSY;
            goto done;
        }
    }
    if (level >= SQLITE_LOCK_RESERVED) {
        if (db->reserved_owner && db->reserved_owner != file) {
            rc = SQLITE_BUSY;
            goto done;
        }
    }
    if (level >= SQLITE_LOCK_PENDING) {
        if (db->pending_owner && db->pending_owner != file) {
            rc = SQLITE_BUSY;
            goto done;
        }
    }
    if (level >= SQLITE_LOCK_EXCLUSIVE) {
        int own_shared = file->has_shared_lock ? 1 : 0;
        if ((db->exclusive_owner && db->exclusive_owner != file) ||
            db->shared_locks > own_shared) {
            rc = SQLITE_BUSY;
            goto done;
        }
    }
    if (acquire_shared) {
        db->shared_locks++;
        file->has_shared_lock = true;
    }
    if (level >= SQLITE_LOCK_RESERVED) db->reserved_owner = file;
    if (level >= SQLITE_LOCK_PENDING) db->pending_owner = file;
    if (level >= SQLITE_LOCK_EXCLUSIVE) db->exclusive_owner = file;
    file->lock_level = level;
done:
    fs_unlock(file->ctx);
    return rc;
}

static int fs_file_unlock(sqlite3_file *sqlite_file, int level)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    fs_database_t *db = file->database;
    if (!db || level >= file->lock_level) return SQLITE_OK;
    fs_lock(file->ctx);
    if (level < SQLITE_LOCK_EXCLUSIVE && db->exclusive_owner == file) {
        db->exclusive_owner = NULL;
    }
    if (level < SQLITE_LOCK_PENDING && db->pending_owner == file) {
        db->pending_owner = NULL;
    }
    if (level < SQLITE_LOCK_RESERVED && db->reserved_owner == file) {
        db->reserved_owner = NULL;
    }
    if (level < SQLITE_LOCK_SHARED && file->has_shared_lock) {
        db->shared_locks--;
        file->has_shared_lock = false;
    }
    file->lock_level = level;
    fs_unlock(file->ctx);
    return SQLITE_OK;
}

static int fs_check_reserved(sqlite3_file *sqlite_file, int *result)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    if (!file->database) {
        *result = 0;
        return SQLITE_OK;
    }
    fs_lock(file->ctx);
    *result = file->database->reserved_owner != NULL ||
              file->database->pending_owner != NULL ||
              file->database->exclusive_owner != NULL;
    fs_unlock(file->ctx);
    return SQLITE_OK;
}

static int fs_file_control(sqlite3_file *sqlite_file, int op, void *arg)
{
    (void)sqlite_file;
    if (op == SQLITE_FCNTL_VFSNAME) {
        *(char **)arg = sqlite3_mprintf("%s", SQLITE_ESP_FS_VFS_NAME);
        return *(char **)arg ? SQLITE_OK : SQLITE_NOMEM;
    }
    return SQLITE_NOTFOUND;
}

static int fs_sector_size(sqlite3_file *sqlite_file)
{
    (void)sqlite_file;
    return 512;
}

static int fs_device_characteristics(sqlite3_file *sqlite_file)
{
    (void)sqlite_file;
    return 0;
}

static int fs_shm_map(sqlite3_file *sqlite_file, int page, int page_size,
                      int is_write, void volatile **out)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    fs_database_t *db = file->database;
    *out = NULL;
    if (!db || page < 0 || page_size != FS_SHM_PAGE_SIZE) {
        return SQLITE_IOERR_SHMMAP;
    }
    fs_lock(file->ctx);
    if (page >= SQLITE_ESP_FS_SHM_PAGES) {
        fs_unlock(file->ctx);
        return is_write ? SQLITE_FULL : SQLITE_OK;
    }
    if (page >= db->shm_page_count && is_write) {
        memset(db->shm + (size_t)db->shm_page_count * page_size, 0,
               (size_t)(page + 1 - db->shm_page_count) * page_size);
        db->shm_page_count = page + 1;
    }
    if (!file->shm_attached) {
        file->shm_attached = true;
        db->shm_refs++;
    }
    if (page < db->shm_page_count) *out = db->shm + (size_t)page * page_size;
    fs_unlock(file->ctx);
    return SQLITE_OK;
}

static int fs_shm_lock(sqlite3_file *sqlite_file, int offset, int count,
                       int flags)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    fs_database_t *db = file->database;
    if (!db || offset < 0 || count < 1 || offset + count > FS_SHM_LOCKS) {
        return SQLITE_IOERR_SHMLOCK;
    }
    bool do_lock = (flags & SQLITE_SHM_LOCK) != 0;
    bool shared = (flags & SQLITE_SHM_SHARED) != 0;
    fs_lock(file->ctx);
    if (do_lock) {
        for (int i = offset; i < offset + count; ++i) {
            uint32_t bit = 1u << i;
            if (shared) {
                if (db->shm_exclusive[i] && db->shm_exclusive[i] != file) {
                    fs_unlock(file->ctx);
                    return SQLITE_BUSY;
                }
            } else {
                int own_shared = (file->shm_shared_mask & bit) ? 1 : 0;
                if ((db->shm_exclusive[i] && db->shm_exclusive[i] != file) ||
                    db->shm_shared[i] > own_shared) {
                    fs_unlock(file->ctx);
                    return SQLITE_BUSY;
                }
            }
        }
    }
    for (int i = offset; i < offset + count; ++i) {
        uint32_t bit = 1u << i;
        if (do_lock && shared && !(file->shm_shared_mask & bit)) {
            file->shm_shared_mask |= bit;
            db->shm_shared[i]++;
        } else if (do_lock && !shared) {
            file->shm_exclusive_mask |= bit;
            db->shm_exclusive[i] = file;
        } else if (!do_lock && shared && (file->shm_shared_mask & bit)) {
            file->shm_shared_mask &= ~bit;
            db->shm_shared[i]--;
        } else if (!do_lock && !shared && (file->shm_exclusive_mask & bit)) {
            file->shm_exclusive_mask &= ~bit;
            if (db->shm_exclusive[i] == file) db->shm_exclusive[i] = NULL;
        }
    }
    fs_unlock(file->ctx);
    return SQLITE_OK;
}

static void fs_shm_barrier(sqlite3_file *sqlite_file)
{
    (void)sqlite_file;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int fs_shm_unmap(sqlite3_file *sqlite_file, int delete_flag)
{
    fs_file_t *file = (fs_file_t *)sqlite_file;
    fs_database_t *db = file->database;
    if (!db) return SQLITE_OK;
    fs_lock(file->ctx);
    release_shm_locks(file);
    if (file->shm_attached) {
        file->shm_attached = false;
        db->shm_refs--;
    }
    if (delete_flag && db->shm_refs == 0) {
        memset(db->shm, 0, SQLITE_ESP_FS_SHM_PAGES * FS_SHM_PAGE_SIZE);
        db->shm_page_count = 0;
    }
    fs_unlock(file->ctx);
    return SQLITE_OK;
}

static int fs_fetch(sqlite3_file *file, sqlite3_int64 offset, int amount,
                    void **out)
{
    (void)file; (void)offset; (void)amount;
    *out = NULL;
    return SQLITE_OK;
}

static int fs_unfetch(sqlite3_file *file, sqlite3_int64 offset, void *ptr)
{
    (void)file; (void)offset; (void)ptr;
    return SQLITE_OK;
}

static const sqlite3_io_methods s_io_methods = {
    .iVersion = 3,
    .xClose = fs_close,
    .xRead = fs_read,
    .xWrite = fs_write,
    .xTruncate = fs_truncate,
    .xSync = fs_sync,
    .xFileSize = fs_file_size,
    .xLock = fs_file_lock,
    .xUnlock = fs_file_unlock,
    .xCheckReservedLock = fs_check_reserved,
    .xFileControl = fs_file_control,
    .xSectorSize = fs_sector_size,
    .xDeviceCharacteristics = fs_device_characteristics,
    .xShmMap = fs_shm_map,
    .xShmLock = fs_shm_lock,
    .xShmBarrier = fs_shm_barrier,
    .xShmUnmap = fs_shm_unmap,
    .xFetch = fs_fetch,
    .xUnfetch = fs_unfetch,
};

static int make_temp_name(char *out, size_t size)
{
    for (int i = 0; i < 16; ++i) {
        int n = snprintf(out, size, "/tmp/sqlite-%08lx.tmp",
                         (unsigned long)esp_random());
        if (n < 0 || (size_t)n >= size) return SQLITE_CANTOPEN;
        if (access(out, F_OK) != 0) return SQLITE_OK;
    }
    return SQLITE_CANTOPEN;
}

static int fs_open(sqlite3_vfs *vfs, sqlite3_filename name,
                   sqlite3_file *sqlite_file, int flags, int *out_flags)
{
    fs_vfs_ctx_t *ctx = vfs->pAppData;
    fs_file_t *file = (fs_file_t *)sqlite_file;
    memset(file, 0, sizeof(*file));
    file->fd = -1;
    file->ctx = ctx;
    const char *path = name;
    if (!path) {
        int rc = make_temp_name(file->path, sizeof(file->path));
        if (rc != SQLITE_OK) return rc;
        path = file->path;
        flags |= SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE;
    } else {
        size_t len = strlen(path);
        if (len == 0 || len >= sizeof(file->path)) return SQLITE_CANTOPEN;
        memcpy(file->path, path, len + 1);
        path = file->path;
    }

    int oflags = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
    if (flags & SQLITE_OPEN_CREATE) oflags |= O_CREAT;
    if ((flags & (SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE)) ==
        (SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE)) {
        oflags |= O_EXCL;
    }
    int fd = open(path, oflags, 0666);
    int actual_flags = flags;
    if (fd < 0 && !(flags & (SQLITE_OPEN_READONLY | SQLITE_OPEN_CREATE))) {
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            actual_flags &= ~SQLITE_OPEN_READWRITE;
            actual_flags |= SQLITE_OPEN_READONLY;
        }
    }
    if (fd < 0) return SQLITE_CANTOPEN;

    file->fd = fd;
    file->open_flags = actual_flags;
    file->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    if (flags & SQLITE_OPEN_MAIN_DB) {
        fs_lock(ctx);
        file->database = get_database(ctx, path);
        if (file->database) file->database->main_refs++;
        fs_unlock(ctx);
        if (!file->database) {
            close(fd);
            file->fd = -1;
            return SQLITE_CANTOPEN;
        }
    }
    fs_lock(ctx);
    ctx->open_count++;
    fs_unlock(ctx);
    file->base.pMethods = &s_io_methods;
    if (out_flags) *out_flags = actual_flags;
    return SQLITE_OK;
}

static int fs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    (void)vfs; (void)sync_dir;
    if (unlink(name) == 0 || errno == ENOENT) return SQLITE_OK;
    return SQLITE_IOERR_DELETE;
}

static int fs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
    (void)vfs;
    int mode = flags == SQLITE_ACCESS_READWRITE ? W_OK : F_OK;
    *out = access(name, mode) == 0;
    return SQLITE_OK;
}

static int fs_full_pathname(sqlite3_vfs *vfs, const char *name,
                            int out_size, char *out)
{
    (void)vfs;
    if (!name || !*name || strlen(name) + 1 > (size_t)out_size) {
        return SQLITE_CANTOPEN;
    }
    memcpy(out, name, strlen(name) + 1);
    return SQLITE_OK;
}

static void *fs_dl_open(sqlite3_vfs *vfs, const char *name)
{ (void)vfs; (void)name; return NULL; }
static void fs_dl_error(sqlite3_vfs *vfs, int n, char *msg)
{ (void)vfs; if (n > 0) { strncpy(msg, "extensions disabled", n); msg[n - 1] = 0; } }
static void (*fs_dl_sym(sqlite3_vfs *vfs, void *h, const char *s))(void)
{ (void)vfs; (void)h; (void)s; return NULL; }
static void fs_dl_close(sqlite3_vfs *vfs, void *h) { (void)vfs; (void)h; }

static int fs_randomness(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs;
    if (n <= 0) return 0;
    esp_fill_random(out, (size_t)n);
    return n;
}

static int fs_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    if (microseconds > 0) usleep((useconds_t)microseconds);
    return microseconds;
}

static int fs_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
    (void)vfs;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *out = (sqlite3_int64)24405875 * 8640000 +
           (sqlite3_int64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return SQLITE_OK;
}

static int fs_current_time(sqlite3_vfs *vfs, double *out)
{
    sqlite3_int64 now;
    int rc = fs_current_time_int64(vfs, &now);
    *out = now / 86400000.0;
    return rc;
}

static int fs_last_error(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs;
    if (n > 0) out[0] = 0;
    return errno;
}

static void setup_vfs(fs_vfs_ctx_t *ctx)
{
    s_vfs = (sqlite3_vfs) {
        .iVersion = 2,
        .szOsFile = sizeof(fs_file_t),
        .mxPathname = FS_PATH_MAX,
        .zName = SQLITE_ESP_FS_VFS_NAME,
        .pAppData = ctx,
        .xOpen = fs_open,
        .xDelete = fs_delete,
        .xAccess = fs_access,
        .xFullPathname = fs_full_pathname,
        .xDlOpen = fs_dl_open,
        .xDlError = fs_dl_error,
        .xDlSym = fs_dl_sym,
        .xDlClose = fs_dl_close,
        .xRandomness = fs_randomness,
        .xSleep = fs_sleep,
        .xCurrentTime = fs_current_time,
        .xGetLastError = fs_last_error,
        .xCurrentTimeInt64 = fs_current_time_int64,
    };
}

esp_err_t sqlite_fatfs_init(void)
{
    if (s_ctx) return ESP_ERR_INVALID_STATE;
    if (sqlite3_vfs_find("espbdl") != NULL) return ESP_ERR_INVALID_STATE;

    fs_vfs_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    size_t shm_size = (size_t)SQLITE_ESP_FS_MAX_DATABASES *
                      SQLITE_ESP_FS_SHM_PAGES * FS_SHM_PAGE_SIZE;
    ctx->shm_storage = calloc(1, shm_size);
    if (!ctx->shm_storage) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < SQLITE_ESP_FS_MAX_DATABASES; ++i) {
        ctx->databases[i].shm = ctx->shm_storage +
            (size_t)i * SQLITE_ESP_FS_SHM_PAGES * FS_SHM_PAGE_SIZE;
    }
    ctx->mutex = xSemaphoreCreateRecursiveMutex();
    if (!ctx->mutex) {
        free(ctx->shm_storage);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    int rc = sqlite3_shutdown();
    if (rc == SQLITE_OK) rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &s_mutex_methods);
    setup_vfs(ctx);
    if (rc == SQLITE_OK) rc = sqlite3_initialize();
    if (rc == SQLITE_OK) rc = sqlite3_vfs_register(&s_vfs, 1);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQLite FATFS VFS initialization failed: rc=%d", rc);
        sqlite3_shutdown();
        vSemaphoreDelete(ctx->mutex);
        free(ctx->shm_storage);
        free(ctx);
        memset(&s_vfs, 0, sizeof(s_vfs));
        return rc == SQLITE_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    s_ctx = ctx;
    ESP_LOGI(TAG, "registered VFS '%s'", SQLITE_ESP_FS_VFS_NAME);
    return ESP_OK;
}

esp_err_t sqlite_fatfs_deinit(void)
{
    fs_vfs_ctx_t *ctx = s_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    fs_lock(ctx);
    bool files_open = ctx->open_count != 0;
    fs_unlock(ctx);
    if (files_open) return ESP_ERR_INVALID_STATE;
    if (sqlite3_vfs_unregister(&s_vfs) != SQLITE_OK) return ESP_FAIL;
    if (sqlite3_shutdown() != SQLITE_OK) return ESP_FAIL;
    s_ctx = NULL;
    vSemaphoreDelete(ctx->mutex);
    free(ctx->shm_storage);
    free(ctx);
    memset(&s_vfs, 0, sizeof(s_vfs));
    return ESP_OK;
}
