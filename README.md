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

// Mount FATFS at /fat before initializing SQLite.
ESP_ERROR_CHECK(sqlite_fatfs_init());

sqlite3 *db = NULL;
int rc = sqlite3_open("/fat/data.db", &db);
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

The demo executes two sequential phases against the partition labelled
`sqlite`:

1. It runs the complete demo and benchmark directly through the raw `espbdl`
   VFS.
2. It deinitializes the raw VFS, recreates the partition/WL BDL stack, and
   calls `esp_vfs_fat_bdl_mount()` with `format_if_mount_failed=true`. Because
   the raw layout is not FATFS, this reformats and mounts the same partition at
   `/fat`. It then opens `/fat/data.db` through
   `sqlite_fatfs` and runs the same workload and timings.

This comparison is destructive: after the firmware finishes, the original
raw-BDL database has been replaced by the FATFS volume. SQLite is shut down
before the FATFS unmount, and the FATFS wrapper does not own either BDL handle.

The short `/fat` mountpoint is intentional. The current ESP-IDF FATFS VFS
stores mount paths in a 15-byte array but copies one byte less; a 14-character
mountpoint such as `/fat_partition` is truncated internally and subsequently
cannot be found by `esp_vfs_fat_info()` or the BDL unmount helper.

### Example benchmark result

Here's the outcome of the benchmark on a ESP32-S31 that write to a NOR flash partition, with build configuration set below:

```kconfig
CONFIG_IDF_TARGET="esp32s31"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600
CONFIG_ESP_MAIN_TASK_STACK_SIZE=65536
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
```

The ESP-IDF version is v6.2 master branch, see: https://github.com/espressif/esp-idf/commit/fa8039b5cadb6e85dd830ff8c2c4bd73b6538aee

| Measurement | ESP-BDL | FATFS | ESP-BDL result |
| --- | ---: | ---: | ---: |
| Sum of timed SQLite APIs | 1.004 s | 1.731 s | 42% less time, 1.72x faster |
| Timed benchmark wall time | 1.37 s | 2.32 s | 41% less time, 1.69x faster |
| Complete demo workload | 6.80 s | 9.64 s | 29% less time, 1.42x faster |

The largest storage-related differences in that run were:

| SQLite operation | FATFS time / ESP-BDL time |
| --- | ---: |
| `sqlite3_step()` inserts | 57.6x |
| `sqlite3_exec(COMMIT)` | 1.71x |
| `sqlite3_step()` selects | 1.95x |
| `sqlite3_step()` deletes | 1.93x |
| `sqlite3_blob_open()` | 10.7x |
| `sqlite3_blob_write()` | 3.65x |
| `sqlite3_blob_read()` | 6.17x |
| `sqlite3_blob_close()` | 2.48x |
| WAL checkpoint | 1.51x |

Prepare, bind, and finalize timings were effectively equal, indicating that
the advantage was concentrated in filesystem and persistence work. The FATFS
insert result included one 155 ms outlier; its minimum insert latency was close
to ESP-BDL. Treat these figures as one observed run rather than a performance
guarantee. For stable results, collect multiple runs and compare medians and
tail latency under the same flash, clock, build, and durability settings.


## License

WTFPL
