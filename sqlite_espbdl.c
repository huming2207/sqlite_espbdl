/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sqlite_espbdl.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sqlite3.h"
#include "sqlite_port.h"

#define BDL_META_MAGIC       UINT64_C(0x314c444250534551) /* "QESPBDL1" */
#define BDL_META_VERSION     1u
#define BDL_META_SLOTS       2u
#define BDL_WAL_HEADER_SIZE  32u
#define BDL_WAL_FRAME_HEADER 24u
#define BDL_WAL_WINDOWS      2u
#define BDL_SHM_LOCKS        8u
#define BDL_SHM_PAGE_SIZE    32768u
#define BDL_SHM_FRAMES_PAGE  4096u
#define BDL_DATABASE_NAME_MAX 64u
#define BDL_META_MAIN_EXISTS 0x01u
#define BDL_META_AUX_EXISTS  0x02u

#ifndef SQLITE_ESPBDL_MUTEX_POOL_SIZE
#define SQLITE_ESPBDL_MUTEX_POOL_SIZE 16
#endif

typedef enum {
    BDL_FILE_MAIN = 0,
    BDL_FILE_WAL,
    BDL_FILE_JOURNAL,
} bdl_file_kind_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t generation;
    uint64_t main_size;
    uint64_t aux_size;
    uint32_t flags;
    uint32_t aux_kind;
    uint64_t reserved[2];
    uint32_t crc32;
    uint32_t reserved2;
} bdl_metadata_t;

typedef struct bdl_file bdl_file_t;

typedef struct {
    esp_blockdev_handle_t dev;
    SemaphoreHandle_t mutex;
    uint64_t disk_end;
    uint64_t data_start;
    uint64_t wal_start;
    uint64_t main_capacity;
    uint64_t wal_capacity;
    uint64_t main_size;
    uint64_t aux_size;
    uint32_t meta_generation;
    uint32_t meta_slot;
    uint32_t meta_record;
    uint32_t meta_flags;
    bdl_file_kind_t aux_kind;
    bool meta_dirty;
    uint8_t *erase_cache;
    uint8_t *read_scratch;
    uint8_t *metadata_scratch;
    size_t metadata_scratch_size;
    uint64_t cache_addr;
    bool cache_valid;
    bool cache_dirty;
    int open_count;
    char database_name[BDL_DATABASE_NAME_MAX];
    bool database_name_set;

    int shared_locks;
    bdl_file_t *reserved_owner;
    bdl_file_t *pending_owner;
    bdl_file_t *exclusive_owner;

    uint8_t *shm_storage;
    int shm_page_capacity;
    int shm_page_count;
    int shm_page_size;
    int shm_refs;
    int shm_shared[BDL_SHM_LOCKS];
    bdl_file_t *shm_exclusive[BDL_SHM_LOCKS];
} bdl_vfs_ctx_t;

struct bdl_file {
    sqlite3_file base;
    bdl_vfs_ctx_t *ctx;
    bdl_file_kind_t kind;
    int lock_level;
    int open_flags;
    uint32_t shm_shared_mask;
    uint32_t shm_exclusive_mask;
    bool has_shared_lock;
    bool shm_attached;
};

static const char *TAG = "sqlite_espbdl";
static bdl_vfs_ctx_t *s_ctx;
static sqlite3_vfs s_vfs;
static bool s_sqlite_mutex_configured;

typedef struct {
    SemaphoreHandle_t handle;
    StaticSemaphore_t storage;
    bool is_static;
    bool in_use;
} bdl_sqlite_mutex_t;

#define BDL_STATIC_MUTEX_FIRST SQLITE_MUTEX_STATIC_MAIN
#define BDL_STATIC_MUTEX_LAST  SQLITE_MUTEX_STATIC_VFS3
#define BDL_STATIC_MUTEX_COUNT \
    (BDL_STATIC_MUTEX_LAST - BDL_STATIC_MUTEX_FIRST + 1)

static bdl_sqlite_mutex_t s_static_mutexes[BDL_STATIC_MUTEX_COUNT];
static bdl_sqlite_mutex_t s_dynamic_mutexes[SQLITE_ESPBDL_MUTEX_POOL_SIZE];
static volatile int s_mutex_init_state;

static int freertos_mutex_init(void)
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
    for (int i = 0; i < BDL_STATIC_MUTEX_COUNT; ++i) {
        s_static_mutexes[i].is_static = true;
        s_static_mutexes[i].handle = xSemaphoreCreateRecursiveMutexStatic(
            &s_static_mutexes[i].storage);
        if (!s_static_mutexes[i].handle) {
            for (int j = 0; j < i; ++j) {
                vSemaphoreDelete(s_static_mutexes[j].handle);
                s_static_mutexes[j].handle = NULL;
            }
            __atomic_store_n(&s_mutex_init_state, -1, __ATOMIC_RELEASE);
            return SQLITE_NOMEM;
        }
    }
    for (int i = 0; i < SQLITE_ESPBDL_MUTEX_POOL_SIZE; ++i) {
        s_dynamic_mutexes[i].is_static = false;
        s_dynamic_mutexes[i].in_use = false;
        s_dynamic_mutexes[i].handle = xSemaphoreCreateRecursiveMutexStatic(
            &s_dynamic_mutexes[i].storage);
        if (!s_dynamic_mutexes[i].handle) {
            for (int j = 0; j < i; ++j) {
                vSemaphoreDelete(s_dynamic_mutexes[j].handle);
                s_dynamic_mutexes[j].handle = NULL;
            }
            for (int j = 0; j < BDL_STATIC_MUTEX_COUNT; ++j) {
                vSemaphoreDelete(s_static_mutexes[j].handle);
                s_static_mutexes[j].handle = NULL;
            }
            __atomic_store_n(&s_mutex_init_state, -1, __ATOMIC_RELEASE);
            return SQLITE_NOMEM;
        }
    }
    __atomic_store_n(&s_mutex_init_state, 2, __ATOMIC_RELEASE);
    return SQLITE_OK;
}

static int freertos_mutex_end(void)
{
    if (__atomic_exchange_n(&s_mutex_init_state, 0, __ATOMIC_ACQ_REL) == 2) {
        for (int i = 0; i < BDL_STATIC_MUTEX_COUNT; ++i) {
            if (s_static_mutexes[i].handle) {
                vSemaphoreDelete(s_static_mutexes[i].handle);
                s_static_mutexes[i].handle = NULL;
            }
        }
        for (int i = 0; i < SQLITE_ESPBDL_MUTEX_POOL_SIZE; ++i) {
            if (s_dynamic_mutexes[i].handle) {
                vSemaphoreDelete(s_dynamic_mutexes[i].handle);
                s_dynamic_mutexes[i].handle = NULL;
                s_dynamic_mutexes[i].in_use = false;
            }
        }
    }
    return SQLITE_OK;
}

static sqlite3_mutex *freertos_mutex_alloc(int type)
{
    if (type >= BDL_STATIC_MUTEX_FIRST && type <= BDL_STATIC_MUTEX_LAST) {
        bdl_sqlite_mutex_t *mutex =
            &s_static_mutexes[type - BDL_STATIC_MUTEX_FIRST];
        return mutex->handle ? (sqlite3_mutex *)mutex : NULL;
    }
    if (type != SQLITE_MUTEX_FAST && type != SQLITE_MUTEX_RECURSIVE) {
        return NULL;
    }
    for (int i = 0; i < SQLITE_ESPBDL_MUTEX_POOL_SIZE; ++i) {
        bool expected = false;
        if (__atomic_compare_exchange_n(&s_dynamic_mutexes[i].in_use,
                                        &expected, true, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return (sqlite3_mutex *)&s_dynamic_mutexes[i];
        }
    }
    return NULL;
}

static void freertos_mutex_free(sqlite3_mutex *sqlite_mutex)
{
    if (!sqlite_mutex) return;
    bdl_sqlite_mutex_t *mutex = (bdl_sqlite_mutex_t *)sqlite_mutex;
    if (!mutex->is_static) {
        __atomic_store_n(&mutex->in_use, false, __ATOMIC_RELEASE);
    }
}

static void freertos_mutex_enter(sqlite3_mutex *sqlite_mutex)
{
    bdl_sqlite_mutex_t *mutex = (bdl_sqlite_mutex_t *)sqlite_mutex;
    configASSERT(xSemaphoreTakeRecursive(mutex->handle, portMAX_DELAY) == pdTRUE);
}

static int freertos_mutex_try(sqlite3_mutex *sqlite_mutex)
{
    bdl_sqlite_mutex_t *mutex = (bdl_sqlite_mutex_t *)sqlite_mutex;
    return xSemaphoreTakeRecursive(mutex->handle, 0) == pdTRUE
         ? SQLITE_OK : SQLITE_BUSY;
}

static void freertos_mutex_leave(sqlite3_mutex *sqlite_mutex)
{
    bdl_sqlite_mutex_t *mutex = (bdl_sqlite_mutex_t *)sqlite_mutex;
    configASSERT(xSemaphoreGiveRecursive(mutex->handle) == pdTRUE);
}

static int freertos_mutex_held(sqlite3_mutex *sqlite_mutex)
{
    bdl_sqlite_mutex_t *mutex = (bdl_sqlite_mutex_t *)sqlite_mutex;
    return xSemaphoreGetMutexHolder(mutex->handle) == xTaskGetCurrentTaskHandle();
}

static int freertos_mutex_notheld(sqlite3_mutex *sqlite_mutex)
{
    return !freertos_mutex_held(sqlite_mutex);
}

static const sqlite3_mutex_methods s_freertos_mutex_methods = {
    .xMutexInit = freertos_mutex_init,
    .xMutexEnd = freertos_mutex_end,
    .xMutexAlloc = freertos_mutex_alloc,
    .xMutexFree = freertos_mutex_free,
    .xMutexEnter = freertos_mutex_enter,
    .xMutexTry = freertos_mutex_try,
    .xMutexLeave = freertos_mutex_leave,
    .xMutexHeld = freertos_mutex_held,
    .xMutexNotheld = freertos_mutex_notheld,
};

static inline void ctx_lock(bdl_vfs_ctx_t *ctx)
{
    configASSERT(xSemaphoreTakeRecursive(ctx->mutex, portMAX_DELAY) == pdTRUE);
}

static inline void ctx_unlock(bdl_vfs_ctx_t *ctx)
{
    configASSERT(xSemaphoreGiveRecursive(ctx->mutex) == pdTRUE);
}

static uint64_t align_up_u64(uint64_t value, size_t alignment)
{
    uint64_t a = alignment;
    if (value > UINT64_MAX - (a - 1)) {
        return UINT64_MAX;
    }
    return ((value + a - 1) / a) * a;
}

static uint64_t align_down_u64(uint64_t value, size_t alignment)
{
    return (value / alignment) * alignment;
}

static uint32_t bdl_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

static bool metadata_valid(const bdl_metadata_t *m,
                           const bdl_vfs_ctx_t *ctx)
{
    if (m->magic != BDL_META_MAGIC || m->version != BDL_META_VERSION) {
        return false;
    }
    bdl_metadata_t copy = *m;
    uint32_t expected = copy.crc32;
    copy.crc32 = 0;
    if (bdl_crc32(&copy, sizeof(copy)) != expected) {
        return false;
    }
    if (m->main_size > ctx->main_capacity || m->aux_size > ctx->wal_capacity) {
        return false;
    }
    return m->aux_kind <= BDL_FILE_JOURNAL;
}

static int raw_read(bdl_vfs_ctx_t *ctx, void *dst, size_t len, uint64_t addr)
{
    const size_t unit = ctx->dev->geometry.read_size;
    uint8_t *out = dst;
    while (len) {
        size_t offset = (size_t)(addr % unit);
        if (offset == 0 && len >= unit) {
            size_t direct = len - (len % unit);
            if (ctx->dev->ops->read(ctx->dev, out, direct, addr, direct) != ESP_OK) {
                return SQLITE_IOERR_READ;
            }
            addr += direct;
            out += direct;
            len -= direct;
            continue;
        }
        uint64_t block = addr - offset;
        if (ctx->dev->ops->read(ctx->dev, ctx->read_scratch, unit,
                                block, unit) != ESP_OK) {
            return SQLITE_IOERR_READ;
        }
        size_t take = unit - offset;
        if (take > len) {
            take = len;
        }
        memcpy(out, ctx->read_scratch + offset, take);
        addr += take;
        out += take;
        len -= take;
    }
    return SQLITE_OK;
}

static int cache_flush(bdl_vfs_ctx_t *ctx)
{
    if (!ctx->cache_valid || !ctx->cache_dirty) {
        return SQLITE_OK;
    }
    const size_t erase_size = ctx->dev->geometry.erase_size;
    if (ctx->dev->ops->erase(ctx->dev, ctx->cache_addr, erase_size) != ESP_OK) {
        return SQLITE_IOERR_WRITE;
    }
    if (ctx->dev->ops->write(ctx->dev, ctx->erase_cache,
                             ctx->cache_addr, erase_size) != ESP_OK) {
        return SQLITE_IOERR_WRITE;
    }
    ctx->cache_dirty = false;
    return SQLITE_OK;
}

static int cache_load(bdl_vfs_ctx_t *ctx, uint64_t block_addr, bool read_old)
{
    if (ctx->cache_valid && ctx->cache_addr == block_addr) {
        return SQLITE_OK;
    }
    int rc = cache_flush(ctx);
    if (rc != SQLITE_OK) {
        return rc;
    }
    if (read_old) {
        rc = raw_read(ctx, ctx->erase_cache,
                      ctx->dev->geometry.erase_size, block_addr);
        if (rc != SQLITE_OK) {
            ctx->cache_valid = false;
            return rc;
        }
    }
    ctx->cache_addr = block_addr;
    ctx->cache_valid = true;
    ctx->cache_dirty = false;
    return SQLITE_OK;
}

static int rmew_write(bdl_vfs_ctx_t *ctx, const void *src,
                      size_t len, uint64_t addr)
{
    const size_t erase_size = ctx->dev->geometry.erase_size;
    const uint8_t *in = src;
    while (len) {
        uint64_t block = align_down_u64(addr, erase_size);
        size_t offset = (size_t)(addr - block);
        size_t take = erase_size - offset;
        if (take > len) {
            take = len;
        }
        bool full_block = offset == 0 && take == erase_size;
        int rc = cache_load(ctx, block, !full_block);
        if (rc != SQLITE_OK) {
            return rc;
        }
        memcpy(ctx->erase_cache + offset, in, take);
        ctx->cache_dirty = true;
        addr += take;
        in += take;
        len -= take;
    }
    return SQLITE_OK;
}

static int rmew_zero(bdl_vfs_ctx_t *ctx, uint64_t addr, uint64_t len)
{
    static const uint8_t zeros[256];
    while (len) {
        size_t take = len > sizeof(zeros) ? sizeof(zeros) : (size_t)len;
        int rc = rmew_write(ctx, zeros, take, addr);
        if (rc != SQLITE_OK) {
            return rc;
        }
        addr += take;
        len -= take;
    }
    return SQLITE_OK;
}

static int physical_read(bdl_vfs_ctx_t *ctx, void *dst,
                         size_t len, uint64_t addr)
{
    const size_t erase_size = ctx->dev->geometry.erase_size;
    uint8_t *out = dst;
    while (len) {
        uint64_t block = align_down_u64(addr, erase_size);
        size_t offset = (size_t)(addr - block);
        size_t take = erase_size - offset;
        if (take > len) {
            take = len;
        }
        if (ctx->cache_valid && ctx->cache_addr == block) {
            memcpy(out, ctx->erase_cache + offset, take);
        } else {
            int rc = raw_read(ctx, out, take, addr);
            if (rc != SQLITE_OK) {
                return rc;
            }
        }
        addr += take;
        out += take;
        len -= take;
    }
    return SQLITE_OK;
}

static uint64_t file_base(const bdl_vfs_ctx_t *ctx, bdl_file_kind_t kind)
{
    return kind == BDL_FILE_MAIN ? ctx->data_start : ctx->wal_start;
}

static uint64_t file_capacity(const bdl_vfs_ctx_t *ctx,
                              bdl_file_kind_t kind)
{
    return kind == BDL_FILE_MAIN ? ctx->main_capacity : ctx->wal_capacity;
}

static uint64_t *file_size_ptr(bdl_vfs_ctx_t *ctx, bdl_file_kind_t kind)
{
    return kind == BDL_FILE_MAIN ? &ctx->main_size : &ctx->aux_size;
}

static int metadata_commit(bdl_vfs_ctx_t *ctx)
{
    int rc = cache_flush(ctx);
    if (rc != SQLITE_OK) {
        return rc;
    }
    if (ctx->dev->ops->sync(ctx->dev) != ESP_OK) {
        return SQLITE_IOERR_FSYNC;
    }
    if (!ctx->meta_dirty) {
        return SQLITE_OK;
    }

    bdl_metadata_t m = {
        .magic = BDL_META_MAGIC,
        .version = BDL_META_VERSION,
        .generation = ctx->meta_generation + 1,
        .main_size = ctx->main_size,
        .aux_size = ctx->aux_size,
        .flags = ctx->meta_flags,
        .aux_kind = ctx->aux_kind,
    };
    m.crc32 = 0;
    m.crc32 = bdl_crc32(&m, sizeof(m));

    const size_t erase_size = ctx->dev->geometry.erase_size;
    const size_t write_size = ctx->dev->geometry.write_size;
    size_t record_len = (size_t)align_up_u64(sizeof(m), write_size);
    const uint32_t records_per_block = (uint32_t)(erase_size / record_len);
    uint32_t new_slot = ctx->meta_slot;
    uint32_t new_record = ctx->meta_record + 1;
    bool rotate = new_record >= records_per_block;
    if (rotate) {
        new_slot = (ctx->meta_slot + 1) % BDL_META_SLOTS;
        new_record = 0;
    }
    const uint64_t addr = (uint64_t)new_slot * erase_size +
                          (uint64_t)new_record * record_len;
    if (record_len > ctx->metadata_scratch_size) return SQLITE_IOERR;
    uint8_t *record = ctx->metadata_scratch;
    memset(record, ctx->dev->device_flags.default_val_after_erase ? 0xff : 0,
           record_len);
    memcpy(record, &m, sizeof(m));

    esp_err_t err = ESP_OK;
    if (rotate) {
        err = ctx->dev->ops->erase(ctx->dev,
                                   (uint64_t)new_slot * erase_size,
                                   erase_size);
    }
    if (err == ESP_OK) {
        err = ctx->dev->ops->write(ctx->dev, record, addr, record_len);
    }
    if (err != ESP_OK || ctx->dev->ops->sync(ctx->dev) != ESP_OK) {
        return SQLITE_IOERR_FSYNC;
    }
    ctx->meta_slot = new_slot;
    ctx->meta_record = new_record;
    ctx->meta_generation = m.generation;
    ctx->meta_dirty = false;
    return SQLITE_OK;
}

static int bdl_close(sqlite3_file *file);
static int bdl_read(sqlite3_file *file, void *dst, int amount,
                    sqlite3_int64 offset);
static int bdl_write(sqlite3_file *file, const void *src, int amount,
                     sqlite3_int64 offset);
static int bdl_truncate(sqlite3_file *file, sqlite3_int64 size);
static int bdl_sync(sqlite3_file *file, int flags);
static int bdl_file_size(sqlite3_file *file, sqlite3_int64 *size);
static int bdl_lock(sqlite3_file *file, int level);
static int bdl_unlock(sqlite3_file *file, int level);
static int bdl_check_reserved(sqlite3_file *file, int *result);
static int bdl_file_control(sqlite3_file *file, int op, void *arg);
static int bdl_sector_size(sqlite3_file *file);
static int bdl_device_characteristics(sqlite3_file *file);
static int bdl_shm_map(sqlite3_file *, int, int, int, void volatile **);
static int bdl_shm_lock(sqlite3_file *, int, int, int);
static void bdl_shm_barrier(sqlite3_file *);
static int bdl_shm_unmap(sqlite3_file *, int);
static int bdl_fetch(sqlite3_file *, sqlite3_int64, int, void **);
static int bdl_unfetch(sqlite3_file *, sqlite3_int64, void *);

static const sqlite3_io_methods s_io_methods = {
    .iVersion = 3,
    .xClose = bdl_close,
    .xRead = bdl_read,
    .xWrite = bdl_write,
    .xTruncate = bdl_truncate,
    .xSync = bdl_sync,
    .xFileSize = bdl_file_size,
    .xLock = bdl_lock,
    .xUnlock = bdl_unlock,
    .xCheckReservedLock = bdl_check_reserved,
    .xFileControl = bdl_file_control,
    .xSectorSize = bdl_sector_size,
    .xDeviceCharacteristics = bdl_device_characteristics,
    .xShmMap = bdl_shm_map,
    .xShmLock = bdl_shm_lock,
    .xShmBarrier = bdl_shm_barrier,
    .xShmUnmap = bdl_shm_unmap,
    .xFetch = bdl_fetch,
    .xUnfetch = bdl_unfetch,
};

static int classify_open(int flags, bdl_file_kind_t *kind)
{
    if (flags & SQLITE_OPEN_MAIN_DB) {
        *kind = BDL_FILE_MAIN;
    } else if (flags & SQLITE_OPEN_WAL) {
        *kind = BDL_FILE_WAL;
    } else if (flags & (SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_SUPER_JOURNAL)) {
        *kind = BDL_FILE_JOURNAL;
    } else {
        return SQLITE_CANTOPEN;
    }
    return SQLITE_OK;
}

static bool auxiliary_name_matches(const bdl_vfs_ctx_t *ctx,
                                   const char *name,
                                   bdl_file_kind_t kind)
{
    if (!ctx->database_name_set || !name) return false;
    const char *suffix = kind == BDL_FILE_WAL ? "-wal" : "-journal";
    size_t base_len = strlen(ctx->database_name);
    return strncmp(name, ctx->database_name, base_len) == 0 &&
           strcmp(name + base_len, suffix) == 0;
}

static int vfs_open(sqlite3_vfs *vfs, sqlite3_filename name,
                    sqlite3_file *file, int flags, int *out_flags)
{
    (void)name;
    bdl_vfs_ctx_t *ctx = vfs->pAppData;
    bdl_file_t *f = (bdl_file_t *)file;
    memset(f, 0, sizeof(*f));
    bdl_file_kind_t kind;
    int rc = classify_open(flags, &kind);
    if (rc != SQLITE_OK) {
        return rc;
    }

    ctx_lock(ctx);
    bool bind_database_name = false;
    if (kind == BDL_FILE_MAIN) {
        if (!name || strlen(name) >= sizeof(ctx->database_name)) {
            ctx_unlock(ctx);
            return SQLITE_CANTOPEN;
        }
        if (ctx->database_name_set) {
            if (strcmp(name, ctx->database_name) != 0) {
                ESP_LOGE(TAG, "database name '%s' aliases active database '%s'",
                         name, ctx->database_name);
                ctx_unlock(ctx);
                return SQLITE_CANTOPEN;
            }
        } else {
            bind_database_name = true;
        }
    } else if (!auxiliary_name_matches(ctx, name, kind)) {
        ctx_unlock(ctx);
        return SQLITE_CANTOPEN;
    }
    if (kind != BDL_FILE_MAIN && (ctx->meta_flags & BDL_META_AUX_EXISTS) &&
        ctx->aux_kind != kind) {
        ctx_unlock(ctx);
        return SQLITE_CANTOPEN;
    }
    uint32_t exists_flag = kind == BDL_FILE_MAIN ? BDL_META_MAIN_EXISTS
                                                  : BDL_META_AUX_EXISTS;
    if (!(ctx->meta_flags & exists_flag)) {
        if (!(flags & SQLITE_OPEN_CREATE)) {
            ctx_unlock(ctx);
            return SQLITE_CANTOPEN;
        }
        ctx->meta_flags |= exists_flag;
        if (kind != BDL_FILE_MAIN) {
            ctx->aux_kind = kind;
            ctx->aux_size = 0;
        }
        ctx->meta_dirty = true;
    }
    if (bind_database_name) {
        memcpy(ctx->database_name, name, strlen(name) + 1);
        ctx->database_name_set = true;
    }
    f->ctx = ctx;
    f->kind = kind;
    f->open_flags = flags;
    f->base.pMethods = &s_io_methods;
    ctx->open_count++;
    ctx_unlock(ctx);
    if (out_flags) {
        *out_flags = flags;
    }
    return SQLITE_OK;
}

static void release_shm_locks(bdl_file_t *f)
{
    bdl_vfs_ctx_t *ctx = f->ctx;
    for (unsigned i = 0; i < BDL_SHM_LOCKS; ++i) {
        uint32_t bit = 1u << i;
        if (f->shm_shared_mask & bit) {
            ctx->shm_shared[i]--;
        }
        if ((f->shm_exclusive_mask & bit) && ctx->shm_exclusive[i] == f) {
            ctx->shm_exclusive[i] = NULL;
        }
    }
    f->shm_shared_mask = 0;
    f->shm_exclusive_mask = 0;
}

static int bdl_close(sqlite3_file *file)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    ctx_lock(ctx);
    bdl_unlock(file, SQLITE_LOCK_NONE);
    release_shm_locks(f);
    if (f->shm_attached && ctx->shm_refs > 0) {
        ctx->shm_refs--;
    }
    int rc = metadata_commit(ctx);
    ctx->open_count--;
    f->base.pMethods = NULL;
    ctx_unlock(ctx);
    return rc;
}

static int bdl_read(sqlite3_file *file, void *dst, int amount,
                    sqlite3_int64 offset)
{
    bdl_file_t *f = (bdl_file_t *)file;
    if (amount < 0 || offset < 0) {
        return SQLITE_IOERR_READ;
    }
    bdl_vfs_ctx_t *ctx = f->ctx;
    ctx_lock(ctx);
    uint64_t size = *file_size_ptr(ctx, f->kind);
    uint64_t off = (uint64_t)offset;
    size_t available = off < size ? (size_t)((size - off) > (uint64_t)amount
                                  ? (uint64_t)amount : (size - off)) : 0;
    int rc = SQLITE_OK;
    if (available) {
        rc = physical_read(ctx, dst, available, file_base(ctx, f->kind) + off);
    }
    if (rc == SQLITE_OK && available != (size_t)amount) {
        memset((uint8_t *)dst + available, 0, (size_t)amount - available);
        rc = SQLITE_IOERR_SHORT_READ;
    }
    ctx_unlock(ctx);
    return rc;
}

static int bdl_write(sqlite3_file *file, const void *src, int amount,
                     sqlite3_int64 offset)
{
    bdl_file_t *f = (bdl_file_t *)file;
    if (amount < 0 || offset < 0 || !(f->open_flags & SQLITE_OPEN_READWRITE)) {
        return SQLITE_IOERR_WRITE;
    }
    bdl_vfs_ctx_t *ctx = f->ctx;
    uint64_t off = (uint64_t)offset;
    uint64_t end = off + (uint64_t)amount;
    if (end < off || end > file_capacity(ctx, f->kind)) {
        ESP_LOGE(TAG, "file %d full: write offset=%llu amount=%d capacity=%llu",
                 (int)f->kind, (unsigned long long)off, amount,
                 (unsigned long long)file_capacity(ctx, f->kind));
        return SQLITE_FULL;
    }
    ctx_lock(ctx);
    uint64_t *size = file_size_ptr(ctx, f->kind);
    int rc = SQLITE_OK;
    if (off > *size) {
        rc = rmew_zero(ctx, file_base(ctx, f->kind) + *size, off - *size);
    }
    if (rc == SQLITE_OK) {
        rc = rmew_write(ctx, src, amount, file_base(ctx, f->kind) + off);
    }
    if (rc == SQLITE_OK && end > *size) {
        *size = end;
        ctx->meta_dirty = true;
    }
    ctx_unlock(ctx);
    return rc;
}

static int bdl_truncate(sqlite3_file *file, sqlite3_int64 new_size)
{
    bdl_file_t *f = (bdl_file_t *)file;
    if (new_size < 0 || (uint64_t)new_size > file_capacity(f->ctx, f->kind)) {
        return SQLITE_FULL;
    }
    bdl_vfs_ctx_t *ctx = f->ctx;
    ctx_lock(ctx);
    uint64_t *size = file_size_ptr(ctx, f->kind);
    int rc = SQLITE_OK;
    if ((uint64_t)new_size > *size) {
        rc = rmew_zero(ctx, file_base(ctx, f->kind) + *size,
                       (uint64_t)new_size - *size);
    }
    if (rc == SQLITE_OK && *size != (uint64_t)new_size) {
        *size = (uint64_t)new_size;
        ctx->meta_dirty = true;
    }
    ctx_unlock(ctx);
    return rc;
}

static int bdl_sync(sqlite3_file *file, int flags)
{
    (void)flags;
    bdl_vfs_ctx_t *ctx = ((bdl_file_t *)file)->ctx;
    ctx_lock(ctx);
    int rc = metadata_commit(ctx);
    ctx_unlock(ctx);
    return rc;
}

static int bdl_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
    bdl_file_t *f = (bdl_file_t *)file;
    ctx_lock(f->ctx);
    *size = (sqlite3_int64)*file_size_ptr(f->ctx, f->kind);
    ctx_unlock(f->ctx);
    return SQLITE_OK;
}

static int bdl_lock(sqlite3_file *file, int level)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    if (f->kind != BDL_FILE_MAIN || level <= f->lock_level) {
        return SQLITE_OK;
    }
    ctx_lock(ctx);
    int rc = SQLITE_OK;
    if (level >= SQLITE_LOCK_SHARED && !f->has_shared_lock) {
        if ((ctx->exclusive_owner && ctx->exclusive_owner != f) ||
            (ctx->pending_owner && ctx->pending_owner != f)) {
            rc = SQLITE_BUSY;
            goto done;
        }
        ctx->shared_locks++;
        f->has_shared_lock = true;
    }
    if (level >= SQLITE_LOCK_RESERVED) {
        if (ctx->reserved_owner && ctx->reserved_owner != f) {
            rc = SQLITE_BUSY;
            goto done;
        }
        ctx->reserved_owner = f;
    }
    if (level >= SQLITE_LOCK_PENDING) {
        if (ctx->pending_owner && ctx->pending_owner != f) {
            rc = SQLITE_BUSY;
            goto done;
        }
        ctx->pending_owner = f;
    }
    if (level >= SQLITE_LOCK_EXCLUSIVE) {
        int own_shared = f->has_shared_lock ? 1 : 0;
        if ((ctx->exclusive_owner && ctx->exclusive_owner != f) ||
            ctx->shared_locks > own_shared) {
            rc = SQLITE_BUSY;
            goto done;
        }
        ctx->exclusive_owner = f;
    }
    f->lock_level = level;
done:
    ctx_unlock(ctx);
    return rc;
}

static int bdl_unlock(sqlite3_file *file, int level)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    if (f->kind != BDL_FILE_MAIN || level >= f->lock_level) {
        return SQLITE_OK;
    }
    ctx_lock(ctx);
    if (level < SQLITE_LOCK_EXCLUSIVE && ctx->exclusive_owner == f) {
        ctx->exclusive_owner = NULL;
    }
    if (level < SQLITE_LOCK_PENDING && ctx->pending_owner == f) {
        ctx->pending_owner = NULL;
    }
    if (level < SQLITE_LOCK_RESERVED && ctx->reserved_owner == f) {
        ctx->reserved_owner = NULL;
    }
    if (level < SQLITE_LOCK_SHARED && f->has_shared_lock) {
        ctx->shared_locks--;
        f->has_shared_lock = false;
    }
    f->lock_level = level;
    ctx_unlock(ctx);
    return SQLITE_OK;
}

static int bdl_check_reserved(sqlite3_file *file, int *result)
{
    bdl_file_t *f = (bdl_file_t *)file;
    ctx_lock(f->ctx);
    *result = f->ctx->reserved_owner != NULL ||
              f->ctx->pending_owner != NULL ||
              f->ctx->exclusive_owner != NULL;
    ctx_unlock(f->ctx);
    return SQLITE_OK;
}

static int bdl_file_control(sqlite3_file *file, int op, void *arg)
{
    (void)file;
    (void)op;
    (void)arg;
    return SQLITE_NOTFOUND;
}

static int bdl_sector_size(sqlite3_file *file)
{
    size_t size = ((bdl_file_t *)file)->ctx->dev->geometry.erase_size;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    return (int)size;
}

static size_t effective_page_size(const esp_blockdev_handle_t dev)
{
    size_t sector = dev->geometry.erase_size;
    if (sector < 512) sector = 512;
    if (sector > 4096) sector = 4096;
    return sector > SQLITE_DEFAULT_PAGE_SIZE ? sector : SQLITE_DEFAULT_PAGE_SIZE;
}

static int bdl_device_characteristics(sqlite3_file *file)
{
    (void)file;
    return 0;
}

static int bdl_shm_map(sqlite3_file *file, int page, int page_size,
                       int is_write, void volatile **out)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    *out = NULL;
    if (page < 0 || page_size <= 0) return SQLITE_IOERR_SHMMAP;
    ctx_lock(ctx);
    if (ctx->shm_page_size && ctx->shm_page_size != page_size) {
        ctx_unlock(ctx);
        return SQLITE_IOERR_SHMMAP;
    }
    if (page >= ctx->shm_page_capacity) {
        ctx_unlock(ctx);
        return is_write ? SQLITE_FULL : SQLITE_OK;
    }
    if (page >= ctx->shm_page_count && is_write) {
        memset(ctx->shm_storage + (size_t)ctx->shm_page_count * page_size, 0,
               (size_t)(page + 1 - ctx->shm_page_count) * page_size);
        ctx->shm_page_count = page + 1;
    }
    if (!f->shm_attached) {
        f->shm_attached = true;
        ctx->shm_refs++;
    }
    *out = page < ctx->shm_page_count
         ? ctx->shm_storage + (size_t)page * page_size : NULL;
    ctx_unlock(ctx);
    return SQLITE_OK;
}

static int bdl_shm_lock(sqlite3_file *file, int offset, int count, int flags)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    if (offset < 0 || count < 1 || offset + count > (int)BDL_SHM_LOCKS) {
        return SQLITE_IOERR_SHMLOCK;
    }
    bool do_lock = (flags & SQLITE_SHM_LOCK) != 0;
    bool shared = (flags & SQLITE_SHM_SHARED) != 0;
    ctx_lock(ctx);
    if (do_lock) {
        for (int i = offset; i < offset + count; ++i) {
            uint32_t bit = 1u << i;
            if (shared) {
                if (ctx->shm_exclusive[i] && ctx->shm_exclusive[i] != f) {
                    ctx_unlock(ctx);
                    return SQLITE_BUSY;
                }
            } else {
                int own_shared = (f->shm_shared_mask & bit) ? 1 : 0;
                if ((ctx->shm_exclusive[i] && ctx->shm_exclusive[i] != f) ||
                    ctx->shm_shared[i] > own_shared) {
                    ctx_unlock(ctx);
                    return SQLITE_BUSY;
                }
            }
        }
    }
    for (int i = offset; i < offset + count; ++i) {
        uint32_t bit = 1u << i;
        if (do_lock && shared && !(f->shm_shared_mask & bit)) {
            f->shm_shared_mask |= bit;
            ctx->shm_shared[i]++;
        } else if (do_lock && !shared) {
            f->shm_exclusive_mask |= bit;
            ctx->shm_exclusive[i] = f;
        } else if (!do_lock && shared && (f->shm_shared_mask & bit)) {
            f->shm_shared_mask &= ~bit;
            ctx->shm_shared[i]--;
        } else if (!do_lock && !shared && (f->shm_exclusive_mask & bit)) {
            f->shm_exclusive_mask &= ~bit;
            if (ctx->shm_exclusive[i] == f) ctx->shm_exclusive[i] = NULL;
        }
    }
    ctx_unlock(ctx);
    return SQLITE_OK;
}

static void bdl_shm_barrier(sqlite3_file *file)
{
    (void)file;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void reset_shm(bdl_vfs_ctx_t *ctx)
{
    memset(ctx->shm_storage, 0,
           (size_t)ctx->shm_page_capacity * BDL_SHM_PAGE_SIZE);
    ctx->shm_page_count = 0;
    ctx->shm_page_size = BDL_SHM_PAGE_SIZE;
    memset(ctx->shm_shared, 0, sizeof(ctx->shm_shared));
    memset(ctx->shm_exclusive, 0, sizeof(ctx->shm_exclusive));
}

static int bdl_shm_unmap(sqlite3_file *file, int delete_flag)
{
    bdl_file_t *f = (bdl_file_t *)file;
    bdl_vfs_ctx_t *ctx = f->ctx;
    ctx_lock(ctx);
    release_shm_locks(f);
    if (f->shm_attached) {
        f->shm_attached = false;
        if (ctx->shm_refs > 0) ctx->shm_refs--;
    }
    if (delete_flag && ctx->shm_refs == 0) reset_shm(ctx);
    ctx_unlock(ctx);
    return SQLITE_OK;
}

static int bdl_fetch(sqlite3_file *file, sqlite3_int64 offset, int amount,
                     void **out)
{
    (void)file; (void)offset; (void)amount;
    *out = NULL;
    return SQLITE_OK;
}

static int bdl_unfetch(sqlite3_file *file, sqlite3_int64 offset, void *ptr)
{
    (void)file; (void)offset; (void)ptr;
    return SQLITE_OK;
}

static bdl_file_kind_t classify_name(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len >= 4 && strcmp(name + len - 4, "-wal") == 0) return BDL_FILE_WAL;
    if (len >= 8 && strcmp(name + len - 8, "-journal") == 0) return BDL_FILE_JOURNAL;
    return BDL_FILE_MAIN;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    (void)sync_dir;
    bdl_vfs_ctx_t *ctx = vfs->pAppData;
    bdl_file_kind_t kind = classify_name(name);
    ctx_lock(ctx);
    if (kind == BDL_FILE_MAIN) {
        ctx->main_size = 0;
        ctx->meta_flags &= ~BDL_META_MAIN_EXISTS;
    } else {
        ctx->aux_size = 0;
        ctx->meta_flags &= ~BDL_META_AUX_EXISTS;
        if (ctx->shm_refs == 0) reset_shm(ctx);
    }
    ctx->meta_dirty = true;
    int rc = metadata_commit(ctx);
    ctx_unlock(ctx);
    return rc;
}

static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
    (void)flags;
    bdl_vfs_ctx_t *ctx = vfs->pAppData;
    bdl_file_kind_t kind = classify_name(name);
    ctx_lock(ctx);
    if (kind == BDL_FILE_MAIN) {
        *out = (ctx->meta_flags & BDL_META_MAIN_EXISTS) != 0;
    } else {
        *out = (ctx->meta_flags & BDL_META_AUX_EXISTS) != 0 &&
               ctx->aux_kind == kind;
    }
    ctx_unlock(ctx);
    return SQLITE_OK;
}

static int vfs_full_pathname(sqlite3_vfs *vfs, const char *name,
                             int out_size, char *out)
{
    (void)vfs;
    if (!name) name = "main.db";
    size_t len = strlen(name);
    if (len + 1 > (size_t)out_size) return SQLITE_CANTOPEN;
    memcpy(out, name, len + 1);
    return SQLITE_OK;
}

static void *vfs_dl_open(sqlite3_vfs *vfs, const char *name)
{ (void)vfs; (void)name; return NULL; }
static void vfs_dl_error(sqlite3_vfs *vfs, int n, char *msg)
{ (void)vfs; if (n > 0) { strncpy(msg, "extensions disabled", (size_t)n); msg[n-1] = 0; } }
static void (*vfs_dl_sym(sqlite3_vfs *vfs, void *h, const char *s))(void)
{ (void)vfs; (void)h; (void)s; return NULL; }
static void vfs_dl_close(sqlite3_vfs *vfs, void *h) { (void)vfs; (void)h; }

static int vfs_randomness(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs;
    if (n <= 0) return 0;
    esp_fill_random(out, (size_t)n);
    return n;
}

static int vfs_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    if (microseconds > 0) usleep((useconds_t)microseconds);
    return microseconds;
}

static int vfs_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
    (void)vfs;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *out = (sqlite3_int64)24405875 * 8640000 +
           (sqlite3_int64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return SQLITE_OK;
}

static int vfs_current_time(sqlite3_vfs *vfs, double *out)
{
    sqlite3_int64 now;
    int rc = vfs_current_time_int64(vfs, &now);
    *out = now / 86400000.0;
    return rc;
}

static int vfs_last_error(sqlite3_vfs *vfs, int n, char *out)
{ (void)vfs; if (n > 0) out[0] = 0; return 0; }

static void setup_vfs(bdl_vfs_ctx_t *ctx)
{
    s_vfs = (sqlite3_vfs) {
        .iVersion = 2,
        .szOsFile = sizeof(bdl_file_t),
        .mxPathname = 64,
        .zName = SQLITE_ESPBDL_VFS_NAME,
        .pAppData = ctx,
        .xOpen = vfs_open,
        .xDelete = vfs_delete,
        .xAccess = vfs_access,
        .xFullPathname = vfs_full_pathname,
        .xDlOpen = vfs_dl_open,
        .xDlError = vfs_dl_error,
        .xDlSym = vfs_dl_sym,
        .xDlClose = vfs_dl_close,
        .xRandomness = vfs_randomness,
        .xSleep = vfs_sleep,
        .xCurrentTime = vfs_current_time,
        .xGetLastError = vfs_last_error,
        .xCurrentTimeInt64 = vfs_current_time_int64,
    };
}

static esp_err_t validate_geometry(esp_blockdev_handle_t dev)
{
    if (!dev || !dev->ops || !dev->ops->read || !dev->ops->write ||
        !dev->ops->erase || !dev->ops->sync || !dev->ops->release ||
        dev->device_flags.read_only || dev->geometry.read_size == 0 ||
        dev->geometry.write_size == 0 || dev->geometry.erase_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t erase = dev->geometry.erase_size;
    if (erase % dev->geometry.read_size || erase % dev->geometry.write_size ||
        erase < sizeof(bdl_metadata_t) ||
        dev->geometry.disk_size < erase * BDL_META_SLOTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static int load_metadata(bdl_vfs_ctx_t *ctx)
{
    const size_t erase_size = ctx->dev->geometry.erase_size;
    const size_t record_len = (size_t)align_up_u64(sizeof(bdl_metadata_t),
                                                   ctx->dev->geometry.write_size);
    const uint32_t records_per_block = (uint32_t)(erase_size / record_len);
    const uint8_t erased = ctx->dev->device_flags.default_val_after_erase
                         ? 0xff : 0x00;
    bdl_metadata_t best = {0};
    bool have_best = false;
    uint32_t best_slot = 0;
    uint32_t highest_occupied[BDL_META_SLOTS] = {0, 0};
    bool any_occupied[BDL_META_SLOTS] = {false, false};

    for (uint32_t block = 0; block < BDL_META_SLOTS; ++block) {
        int rc = raw_read(ctx, ctx->erase_cache, erase_size,
                          (uint64_t)block * erase_size);
        if (rc != SQLITE_OK) return rc;
        for (uint32_t record = 0; record < records_per_block; ++record) {
            const uint8_t *p = ctx->erase_cache + (size_t)record * record_len;
            bool occupied = false;
            for (size_t i = 0; i < record_len; ++i) {
                if (p[i] != erased) { occupied = true; break; }
            }
            if (occupied) {
                any_occupied[block] = true;
                highest_occupied[block] = record;
            }
            bdl_metadata_t candidate;
            memcpy(&candidate, p, sizeof(candidate));
            if (metadata_valid(&candidate, ctx) &&
                (!have_best || (int32_t)(candidate.generation - best.generation) > 0)) {
                best = candidate;
                best_slot = block;
                have_best = true;
            }
        }
    }
    if (!have_best) {
        ctx->meta_slot = BDL_META_SLOTS - 1;
        ctx->meta_record = records_per_block - 1;
        ctx->meta_dirty = true;
        return metadata_commit(ctx);
    }
    ctx->meta_slot = best_slot;
    ctx->meta_record = any_occupied[best_slot]
                     ? highest_occupied[best_slot] : 0;
    ctx->meta_generation = best.generation;
    ctx->main_size = best.main_size;
    ctx->aux_size = best.aux_size;
    ctx->meta_flags = best.flags;
    ctx->aux_kind = (bdl_file_kind_t)best.aux_kind;
    return SQLITE_OK;
}

esp_err_t sqlite_espbdl_init(esp_blockdev_handle_t *handle)
{
    if (!handle || !*handle) return ESP_ERR_INVALID_ARG;
    if (s_ctx) return ESP_ERR_INVALID_STATE;
    esp_blockdev_handle_t dev = *handle;
    esp_err_t err = validate_geometry(dev);
    if (err != ESP_OK) return err;

    bdl_vfs_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->dev = dev;
    const size_t erase = dev->geometry.erase_size;
    ctx->disk_end = align_down_u64(dev->geometry.disk_size, erase);
    ctx->data_start = (uint64_t)BDL_META_SLOTS * erase;
    const size_t page_size = effective_page_size(dev);
    /* Autocheckpoint is triggered after a committing transaction, so one
     * additional checkpoint window is required for transaction headroom. */
    uint64_t wal_required = BDL_WAL_HEADER_SIZE +
        (uint64_t)SQLITE_DEFAULT_WAL_AUTOCHECKPOINT * BDL_WAL_WINDOWS *
        (BDL_WAL_FRAME_HEADER + (uint64_t)page_size);
    ctx->wal_capacity = align_up_u64(wal_required, erase);
    if (ctx->wal_capacity == UINT64_MAX ||
        ctx->disk_end <= ctx->data_start + ctx->wal_capacity + page_size) {
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }
    ctx->wal_start = ctx->disk_end - ctx->wal_capacity;
    ctx->main_capacity = ctx->wal_start - ctx->data_start;
    const size_t metadata_size = (size_t)align_up_u64(
        sizeof(bdl_metadata_t), dev->geometry.write_size);
    const uint64_t wal_frames = ctx->wal_capacity /
        (BDL_WAL_FRAME_HEADER + (uint64_t)page_size);
    const uint64_t shm_pages = 1 + wal_frames / BDL_SHM_FRAMES_PAGE;
    if (shm_pages > INT_MAX || shm_pages > SIZE_MAX / BDL_SHM_PAGE_SIZE) {
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }
    ctx->shm_page_capacity = (int)shm_pages;
    ctx->shm_page_size = BDL_SHM_PAGE_SIZE;
    ctx->erase_cache = malloc(erase);
    ctx->read_scratch = malloc(dev->geometry.read_size);
    ctx->metadata_scratch = malloc(metadata_size);
    ctx->metadata_scratch_size = metadata_size;
    ctx->shm_storage = calloc((size_t)ctx->shm_page_capacity,
                              BDL_SHM_PAGE_SIZE);
    if (!ctx->erase_cache || !ctx->read_scratch || !ctx->metadata_scratch ||
        !ctx->shm_storage) {
        free(ctx->shm_storage);
        free(ctx->metadata_scratch);
        free(ctx->read_scratch);
        free(ctx->erase_cache);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->mutex = xSemaphoreCreateRecursiveMutex();
    if (!ctx->mutex) {
        free(ctx->shm_storage);
        free(ctx->metadata_scratch);
        free(ctx->read_scratch);
        free(ctx->erase_cache);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    int rc = load_metadata(ctx);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "metadata load/initialization failed: SQLite rc=%d", rc);
        vSemaphoreDelete(ctx->mutex);
        free(ctx->shm_storage);
        free(ctx->metadata_scratch);
        free(ctx->read_scratch);
        free(ctx->erase_cache);
        free(ctx);
        return ESP_FAIL;
    }

    if (!s_sqlite_mutex_configured) {
        rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &s_freertos_mutex_methods);
        if (rc != SQLITE_OK) {
            ESP_LOGE(TAG, "FreeRTOS SQLite mutex configuration failed: rc=%d", rc);
            vSemaphoreDelete(ctx->mutex);
            free(ctx->shm_storage);
            free(ctx->metadata_scratch);
            free(ctx->read_scratch);
            free(ctx->erase_cache);
            free(ctx);
            return ESP_ERR_INVALID_STATE;
        }
        s_sqlite_mutex_configured = true;
    }

    setup_vfs(ctx);
    rc = sqlite3_initialize();
    /* sqlite3_os_init() registers this VFS during core initialization so the
     * built-in memdb VFS has a lower VFS to wrap.  Register here only when
     * SQLite had already been initialized by the application. */
    if (rc == SQLITE_OK && sqlite3_vfs_find(SQLITE_ESPBDL_VFS_NAME) != &s_vfs) {
        rc = sqlite3_vfs_register(&s_vfs, 1);
    }
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQLite initialization/VFS registration failed: rc=%d", rc);
        sqlite3_shutdown();
        vSemaphoreDelete(ctx->mutex);
        free(ctx->shm_storage);
        free(ctx->metadata_scratch);
        free(ctx->read_scratch);
        free(ctx->erase_cache);
        free(ctx);
        return ESP_FAIL;
    }
    s_ctx = ctx;
    *handle = ESP_BLOCKDEV_HANDLE_INVALID;
    ESP_LOGI(TAG, "registered: db=%llu bytes, wal=%llu bytes, erase=%u",
             (unsigned long long)ctx->main_capacity,
             (unsigned long long)ctx->wal_capacity, (unsigned)erase);
    return ESP_OK;
}

esp_err_t sqlite_espbdl_deinit(void)
{
    bdl_vfs_ctx_t *ctx = s_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    ctx_lock(ctx);
    if (ctx->open_count != 0) {
        ctx_unlock(ctx);
        return ESP_ERR_INVALID_STATE;
    }
    int rc = metadata_commit(ctx);
    ctx_unlock(ctx);
    if (rc != SQLITE_OK || sqlite3_vfs_unregister(&s_vfs) != SQLITE_OK) {
        return ESP_FAIL;
    }
    if (sqlite3_shutdown() != SQLITE_OK) return ESP_FAIL;
    s_ctx = NULL;
    esp_err_t release_err = ctx->dev->ops->release(ctx->dev);
    vSemaphoreDelete(ctx->mutex);
    free(ctx->shm_storage);
    free(ctx->metadata_scratch);
    free(ctx->read_scratch);
    free(ctx->erase_cache);
    free(ctx);
    memset(&s_vfs, 0, sizeof(s_vfs));
    return release_err;
}

esp_err_t sqlite_espbdl_get_capacity(uint64_t *database_bytes,
                                     uint64_t *wal_bytes)
{
    if (!s_ctx || !database_bytes || !wal_bytes) return ESP_ERR_INVALID_ARG;
    *database_bytes = s_ctx->main_capacity;
    *wal_bytes = s_ctx->wal_capacity;
    return ESP_OK;
}


int sqlite3_os_init(void)
{
    if (s_vfs.pAppData) {
        return sqlite3_vfs_register(&s_vfs, 1);
    }
    return SQLITE_OK;
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
