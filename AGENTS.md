# SQLite ESP-BDL Component Guide

## Scope and source of truth

These instructions apply to `components/sqlite_espbdl`. The component targets
the current ESP-IDF tree at `/home/hu/esp/esp-idf-master`. The Block Device
Layer is new and must not be inferred from older storage APIs: consult the
Espressif documentation MCP first, then the local ESP-IDF headers and source.

Do not mix the legacy `wl_mount()`/`wl_handle_t` path with the BDL path.
`wl_get_blockdev()` accepts a lower `esp_blockdev_handle_t`, normally obtained
with `esp_partition_ptr_get_blockdev()`, and returns an independent WL BDL.

## Implemented component

This component uses the ESP-IDF Block Device Layer directly, without FATFS or
another filesystem. It supports one initialized BDL device and one logical
database name at a time.

Public lifecycle API:

- `sqlite_espbdl_init(esp_blockdev_handle_t *handle)` registers the `espbdl`
  VFS as SQLite's default VFS. Ownership transfers on success and `*handle`
  is set to `ESP_BLOCKDEV_HANDLE_INVALID`.
- `sqlite_espbdl_deinit()` requires all SQLite connections to be closed. It
  flushes metadata, unregisters the VFS, calls `sqlite3_shutdown()`, releases
  all preallocated resources, and calls the owned BDL handle's `release` op.
- `sqlite_espbdl_get_capacity()` reports database and WAL-region capacities.

The SQLite port owns only the handle passed successfully to
`sqlite_espbdl_init()`. When using `wl_get_blockdev(partition_bdl, &wl_bdl)`,
the port releases `wl_bdl`; the application must release `partition_bdl` after
`sqlite_espbdl_deinit()` succeeds.

## Storage layout and durability

One BDL device stores one SQLite database:

1. Two erase blocks contain a redundant append-only metadata journal.
2. The main database occupies the middle region.
3. A fixed WAL/rollback-journal region is reserved at the end.

The first successfully opened main filename is retained in the VFS context.
Further connections must use exactly that name; another name is rejected with
`SQLITE_CANTOPEN` so two logical names cannot silently alias the same blocks.

The auxiliary region is sized as two autocheckpoint windows using
`SQLITE_DEFAULT_WAL_AUTOCHECKPOINT`, SQLite's effective page size, WAL frame
overhead, and BDL erase alignment. A transaction must fit in this region or
SQLite returns `SQLITE_FULL`.

Changing metadata fields, metadata validation, region offsets, WAL sizing, or
the meaning of stored offsets changes the on-device format. Do not make such a
change silently: either preserve compatibility or bump `BDL_META_VERSION` and
provide an explicit migration/format policy.

Metadata records contain generation, logical file sizes, existence flags,
auxiliary-file type, and CRC32. Records append within one metadata erase block
before rotating to and erasing the other block.

All physical modifications use cached read-modify-erase-write, even when the
BDL implementation appears rewritable. Aligned full erase-block writes skip
the initial read. `xSync` flushes the data cache, calls BDL `sync`, persists a
new metadata record, and syncs again.

## WAL and locking

WAL support includes in-RAM WAL-index shared memory, `xShmMap`, shared and
exclusive `xShmLock` handling, barriers, and multiple in-process SQLite
connections.

SQLite core mutexes and the VFS context lock use native FreeRTOS recursive
mutexes. The component does not use pthread mutexes. SQLite mutex methods are
installed through `SQLITE_CONFIG_MUTEX` before `sqlite3_initialize()`.

Do not reintroduce pthread mutexes. Separate connections using the retained
database name share file locks and WAL-index locks. WAL permits concurrent
readers but serializes writers; callers must handle `SQLITE_BUSY`. The current
SQLite defaults select exclusive locking, while the demo explicitly requests
`PRAGMA locking_mode=NORMAL` for multiple connections.

## Allocation policy

All VFS working memory is allocated during `sqlite_espbdl_init()` and released
during `sqlite_espbdl_deinit()`:

- erase-block cache;
- aligned-read scratch buffer;
- metadata record scratch buffer;
- complete WAL-index shared-memory storage;
- VFS context and recursive mutex.

SQLite dynamic mutexes use a fixed, statically backed FreeRTOS mutex pool.
`SQLITE_ESPBDL_MUTEX_POOL_SIZE` defaults to 16 and may be overridden at compile
time. File access, transactions, metadata commits, and WAL shared-memory
mapping do not allocate or free VFS heap memory.

Keep performance instrumentation out of the VFS access path. In particular,
do not add benchmark counters, logging, allocation, or per-operation tracing to
`xRead`, `xWrite`, `xSync`, RMW cache operations, or BDL callbacks. Benchmarks
belong in application/test code and should time public SQLite APIs so results
remain comparable with FATFS and other VFS implementations.

## SQLite configuration changes

`sqlite_port.h` selects `SQLITE_OS_OTHER`, keeps WAL enabled, disables mmap,
keeps JSON and incremental BLOB APIs enabled, and configures embedded
page/cache/journal defaults. `sqlite3.c` includes this configuration before the
amalgamation. The component CMake file depends on `esp_blockdev`, `esp_timer`,
`esp_system`, and `freertos`.

Avoid editing the SQLite amalgamation except for the early `sqlite_port.h`
include required to select this port. Prefer changes in `sqlite_port.h`,
`sqlite_espbdl.c`, and `sqlite_espbdl.h`.

## FATFS comparison VFS

`sqlite_esp_fs.c/.h` is an opt-in comparison VFS over ESP-IDF's POSIX file
API. The application mounts FATFS itself, calls `sqlite_fatfs_init()`, and can
then use an absolute mounted path such as
`sqlite3_open("/fat_partition/data.db", &db)`. After closing all SQLite
connections, call `sqlite_fatfs_deinit()` before unmounting FATFS.

The wrapper does not mount, format, unmount, or own the FATFS volume. It uses
`open`, `pread`, `pwrite`, `ftruncate`, `fsync`, and `unlink`; it provides its
own in-process locks and RAM WAL index because ESP-IDF FATFS treats POSIX file
locks as no-ops. Consequently it is safe only among connections using this VFS
inside one firmware process, not for concurrent access from another system.

FATFS WAL-index memory is allocated at `sqlite_fatfs_init()`. The defaults
support two simultaneous database paths and one 32 KiB WAL-index page per
database. Override `SQLITE_ESP_FS_MAX_DATABASES` or
`SQLITE_ESP_FS_SHM_PAGES` at compile time when a benchmark needs more. Keep
this wrapper separate from `sqlite_espbdl.c`; benchmark-specific behavior must
not alter the production raw-BDL VFS.

## Validation completed

- `idf.py build` passes against `/home/hu/esp/esp-idf-master` for the configured
  ESP32-S31 target.
- A native aligned NOR-style BDL harness verified forced RMW behavior, WAL
  transactions, checkpointing, normal-locking shared memory, two simultaneous
  connections, persistence across deinit/reinit, and handle release.
- The final component archive has no unresolved pthread symbols.

Build command:

```sh
source /home/hu/esp/esp-idf-master/export.sh
idf.py build
```

Before handing off component changes, also run `git diff --check`. For VFS,
WAL, locking, allocation, or persistence changes, exercise the aligned
NOR-style host harness or an equivalent test that rejects writes attempting to
change erased bits without an erase.

## Demo application

`main/hello_world_main.c` expects a data partition labelled `sqlite`. It gets
the partition BDL with `esp_partition_ptr_get_blockdev()`, wraps it with
`wl_get_blockdev()`, transfers the WL BDL into `sqlite_espbdl_init()`, enables
WAL, and demonstrates table creation, prepared inserts, reads, an update, a
delete, JSON-valid telemetry documents, JSON extraction/update/iteration and
aggregation, joins and window queries, bound BLOB packets, incremental BLOB
read/write verification, a second connection, `integrity_check`, and a
truncating checkpoint.
The WL BDL is released by the SQLite port; the demo separately releases the
underlying partition BDL after SQLite deinitialization.

The demo also runs a small watchdog-safe API benchmark entirely in
`main/hello_world_main.c`. It times individual `sqlite3_prepare_v2`, bind,
step, reset, finalize, exec, incremental-BLOB, and WAL-checkpoint calls with
`esp_timer_get_time()`, yielding between samples. No benchmark counters or
instrumentation are built into the BDL VFS, so the same API-level workload can
later be used unchanged with a FATFS VFS.

## Maintenance invariants

- Preserve the caller-responsible BDL alignment contract.
- All physical writes continue to use read-modify-erase-write.
- Data must be flushed before its metadata record is committed.
- Keep redundant CRC-protected metadata and generation selection intact.
- Keep WAL shared memory process-local and shared across connections.
- Reject a second logical main filename with `SQLITE_CANTOPEN`.
- Do not release the lower BDL from the WL BDL wrapper.
- Do not free preallocated VFS memory while SQLite files remain open.
- Keep benchmark code VFS-independent and watchdog-safe.
