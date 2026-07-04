# SQLite ESP Block Device VFS

This component runs one SQLite database directly on one
`esp_blockdev_handle_t`, without a filesystem. `sqlite_espbdl_init()` takes
ownership of the handle and registers `espbdl` as SQLite's default VFS.

## On-device layout

The first two erase blocks hold an append-only, redundant metadata journal.
The main database follows. The WAL/journal region is at the end of the device.
Its capacity is two autocheckpoint windows, calculated from
`SQLITE_DEFAULT_WAL_AUTOCHECKPOINT`, SQLite's effective page size, and the
device erase size. The second window allows the transaction that crosses the
autocheckpoint threshold to commit before checkpointing starts.

Every data update uses a one-erase-block cache and is written as
read/modify/erase/write. Aligned full-block writes avoid the initial read.
`xSync` flushes data, calls the BDL `sync` operation, appends metadata, and
syncs again. The metadata journal amortizes metadata erases over all records
that fit in an erase block.

Only one database is stored on a device. The auxiliary region is shared by
the rollback journal and WAL because SQLite does not use both at once. WAL
shared memory and locks live in RAM and are shared by all connections using
this VFS in the process. SQLite core mutexes and VFS locks use native FreeRTOS
recursive mutexes; the port does not depend on pthread mutexes.

For benchmark comparisons only, `sqlite_esp_fs.c/.h` also provides a FATFS
VFS. Mount FATFS in the application, call `sqlite_fatfs_init()`, and open the
database by its mounted absolute path, for example
`sqlite3_open("/fat_partition/data.db", &db)`. Close every connection, call
`sqlite_fatfs_deinit()`, and then unmount FATFS. This comparison VFS does not
own the FATFS mount and does not change the raw block-device implementation.

The first successfully opened main-database filename becomes the context's
logical database name. Other connections may reuse that exact name, but a
different name is rejected with `SQLITE_CANTOPEN` instead of silently aliasing
the same physical blocks. The binding resets during `sqlite_espbdl_deinit()`.

All VFS working memory is allocated by `sqlite_espbdl_init()`: the erase-block
cache, aligned-read scratch buffer, metadata record buffer, complete WAL-index
shared-memory area, and a fixed pool of SQLite FreeRTOS mutexes. File access,
transactions, metadata commits, and WAL-index mapping do not allocate or free
heap memory. `sqlite_espbdl_deinit()` calls `sqlite3_shutdown()` and releases
the preallocated memory, so every SQLite connection must be closed first.

`SQLITE_ESPBDL_MUTEX_POOL_SIZE` defaults to 16 dynamic SQLite mutexes and can
be overridden at compile time when more concurrent connections are required.


## Usage

```c
#include "sqlite3.h"
#include "sqlite_espbdl.h"

esp_blockdev_handle_t bdl = obtain_block_device();
ESP_ERROR_CHECK(sqlite_espbdl_init(&bdl));
// bdl is now ESP_BLOCKDEV_HANDLE_INVALID.

sqlite3 *db = NULL;
ESP_ERROR_CHECK(sqlite3_open("main.db", &db) == SQLITE_OK
                ? ESP_OK : ESP_FAIL);
sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

sqlite3_close(db);
ESP_ERROR_CHECK(sqlite_espbdl_deinit());
```

Initialization treats a device with no valid metadata record as an empty
store. A single transaction must fit in the reserved WAL capacity; otherwise
SQLite returns `SQLITE_FULL`. Use smaller transactions or increase
`SQLITE_DEFAULT_WAL_AUTOCHECKPOINT` when necessary.
