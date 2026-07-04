# SQLite VFS for ESP-IDF Block Devices and FATFS

This component contains SQLite itself and two selectable VFS backends:

- `espbdl` runs one SQLite database directly on an
  `esp_blockdev_handle_t`, without a filesystem. This is the primary backend.
- `esp-fatfs` uses ESP-IDF's POSIX file API on an already-mounted FATFS volume.
  It exists for performance comparisons with the raw block-device backend.

Only initialize one backend at a time. Both become SQLite's default VFS, so a
normal `sqlite3_open()` call uses whichever backend was initialized.

## Raw block-device backend

Include `sqlite_espbdl.h` and pass an owned block-device handle to
`sqlite_espbdl_init()`:

```c
#include "sqlite3.h"
#include "sqlite_espbdl.h"

esp_blockdev_handle_t sqlite_bdl = obtain_block_device();
ESP_ERROR_CHECK(sqlite_espbdl_init(&sqlite_bdl));

// Ownership transferred successfully.
assert(sqlite_bdl == ESP_BLOCKDEV_HANDLE_INVALID);

sqlite3 *db = NULL;
int rc = sqlite3_open("main.db", &db);
if (rc != SQLITE_OK) {
    // sqlite3_close() is valid even when sqlite3_open() returns an error.
    sqlite3_close(db);
    db = NULL;
}

if (db) {
    sqlite3_exec(db, "PRAGMA locking_mode=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS samples("
        "id INTEGER PRIMARY KEY, payload BLOB, metadata JSON)",
        NULL, NULL, NULL);
    sqlite3_close(db);
}

ESP_ERROR_CHECK(sqlite_espbdl_deinit());
```

`sqlite_espbdl_init()` takes ownership only on success and sets the caller's
handle to `ESP_BLOCKDEV_HANDLE_INVALID`. `sqlite_espbdl_deinit()` releases that
handle. Every SQLite connection must be closed before deinitialization;
otherwise it returns `ESP_ERR_INVALID_STATE`.

### ESP partition with wear levelling

For a partition-backed device, create the lower partition BDL and then wrap it
with the wear-levelling BDL:

```c
const esp_partition_t *partition = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "sqlite");

esp_blockdev_handle_t partition_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
esp_blockdev_handle_t wl_bdl = ESP_BLOCKDEV_HANDLE_INVALID;

ESP_ERROR_CHECK(esp_partition_ptr_get_blockdev(partition, &partition_bdl));
ESP_ERROR_CHECK(wl_get_blockdev(partition_bdl, &wl_bdl));
ESP_ERROR_CHECK(sqlite_espbdl_init(&wl_bdl));

sqlite3 *db = NULL;
ESP_ERROR_CHECK(sqlite3_open("main.db", &db) == SQLITE_OK
                ? ESP_OK : ESP_FAIL);

// Use the database, then release resources in ownership order.
sqlite3_close(db);
ESP_ERROR_CHECK(sqlite_espbdl_deinit());       // Releases wl_bdl.
ESP_ERROR_CHECK(partition_bdl->ops->release(partition_bdl));
```

Do not combine the legacy `wl_mount()`/`wl_handle_t` path with
`wl_get_blockdev()`. The latter accepts a lower `esp_blockdev_handle_t` and
returns another block-device handle.

### Raw-device layout

The block device is divided into:

1. Two erase blocks containing a redundant append-only metadata journal.
2. The main SQLite database region.
3. A fixed auxiliary region shared by the WAL and rollback journal.

The auxiliary capacity is two autocheckpoint windows calculated from
`SQLITE_DEFAULT_PAGE_SIZE`, `SQLITE_DEFAULT_WAL_AUTOCHECKPOINT`, WAL frame
overhead, and the device's erase alignment. Query the usable sizes after
initialization:

```c
uint64_t database_bytes;
uint64_t wal_bytes;
ESP_ERROR_CHECK(sqlite_espbdl_get_capacity(&database_bytes, &wal_bytes));
```

A transaction that cannot fit in the reserved auxiliary region returns
`SQLITE_FULL`. Split unusually large transactions or increase the configured
autocheckpoint capacity before formatting a new device.

The first main-database filename opened after initialization becomes the
logical name bound to the device. Further connections may use that exact name.
A different filename is rejected with `SQLITE_CANTOPEN` instead of aliasing the
same physical blocks.

### Flash writes and durability

The VFS never assumes that a block-device write can transparently change
previously programmed bits. Physical changes use read-modify-erase-write, even
when the lower device appears directly writable. Aligned complete erase-block
writes can skip the initial read.

On `xSync`, dirty data is flushed and the BDL is synchronized before a new
CRC-protected metadata record is committed. The device is synchronized again
after the metadata commit.

### WAL and concurrency

WAL mode is supported. WAL-index shared memory and locks are held in RAM and
shared by connections inside this process. Readers can run concurrently;
writers remain serialized by SQLite and may receive `SQLITE_BUSY`. Configure a
busy timeout when contention is expected:

```c
sqlite3_busy_timeout(db, 3000);
```

SQLite core mutexes and both VFS implementations use native FreeRTOS recursive
mutexes, not pthread mutexes.

### Allocation behavior

Raw-VFS working memory is allocated during `sqlite_espbdl_init()` and released
during `sqlite_espbdl_deinit()`. This includes the erase cache, aligned-read
scratch buffer, metadata buffer, complete WAL-index area, context mutex, and
SQLite mutex pool. File access, transactions, metadata commits, and WAL-index
mapping do not allocate or free VFS heap memory.

`SQLITE_ESPBDL_MUTEX_POOL_SIZE` defaults to 16 dynamic SQLite mutexes and can
be overridden at compile time.

## FATFS comparison backend

The FATFS wrapper does not mount, format, unmount, or own a volume. Mount FATFS
first, then call `sqlite_fatfs_init()` and pass the mounted file path directly
to `sqlite3_open()`:

```c
#include "sqlite3.h"
#include "sqlite_esp_fs.h"

// Mount FATFS at /fat_partition before initializing SQLite.
ESP_ERROR_CHECK(sqlite_fatfs_init());

sqlite3 *db = NULL;
int rc = sqlite3_open("/fat_partition/data.db", &db);
if (rc != SQLITE_OK) {
    sqlite3_close(db);
    db = NULL;
}

if (db) {
    sqlite3_exec(db, "PRAGMA locking_mode=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    // Run the same SQL workload used by the raw-BDL benchmark.

    sqlite3_close(db);
}

ESP_ERROR_CHECK(sqlite_fatfs_deinit());
// Unmount FATFS after SQLite has been deinitialized.
```

The wrapper maps SQLite file operations to `open`, `pread`, `pwrite`,
`ftruncate`, `fsync`, `unlink`, and related ESP-IDF VFS calls. Database, WAL,
and journal files are ordinary files on the mounted volume.

ESP-IDF FATFS treats POSIX record-lock requests as no-ops. Therefore this
wrapper supplies process-local database locks and a RAM WAL index. Concurrent
connections in the same firmware process are supported, but concurrent access
from another process, host, or device is not supported.

FATFS comparison-VFS memory is preallocated by `sqlite_fatfs_init()` and freed
by `sqlite_fatfs_deinit()`. Defaults support two simultaneously active database
paths and one 32 KiB WAL-index page per database. These compile-time settings
can be adjusted:

- `SQLITE_ESP_FS_MAX_DATABASES`
- `SQLITE_ESP_FS_SHM_PAGES`
- `SQLITE_ESP_FS_MUTEX_POOL_SIZE`

All SQLite connections must be closed before `sqlite_fatfs_deinit()`. The
FATFS mount remains owned by the application and must be unmounted separately.

## Benchmark comparison

Benchmark public SQLite APIs rather than adding counters to either VFS. Mount
or initialize one backend, run the same SQL workload, close the database, and
fully deinitialize it before switching backends. Useful timings include:

- `sqlite3_prepare_v2()` and `sqlite3_finalize()`;
- `sqlite3_bind_*()`, `sqlite3_step()`, and `sqlite3_reset()`;
- transaction-level `sqlite3_exec()` calls;
- incremental BLOB reads and writes;
- `sqlite3_wal_checkpoint_v2()`.

Yield between benchmark samples on ESP-IDF so a long run does not starve the
idle tasks or trigger the task watchdog. The demo in `main/hello_world_main.c`
contains the current watchdog-safe API benchmark.


## License

WTFPL