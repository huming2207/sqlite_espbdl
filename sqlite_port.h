#pragma once

/*
 * We provide our own sqlite3_os_init(), sqlite3_os_end(), and sqlite3_vfs.
 * No Unix/Windows/VxWorks OS backend.
 */
#define SQLITE_OS_OTHER                      1

/*
 * Keep disk I/O enabled. Your "disk" is the custom BDL VFS.
 */
#undef SQLITE_OMIT_DISKIO

/*
 * Do not use mmap on ESP32 / raw BDL.
 */
#define SQLITE_DEFAULT_MMAP_SIZE             0
#define SQLITE_MAX_MMAP_SIZE                 0

/*
 * ESP32/SD card offsets are well below 2 GiB in many applications.
 * If you may use DB/WAL regions above 2 GiB, remove this.
 */
#define SQLITE_DISABLE_LFS                   1

/*
 * Directory sync is filesystem-specific. Your VFS xSync() should flush the
 * BDL device and your A/B metadata instead.
 */
#define SQLITE_DISABLE_DIRSYNC               1


/* -------------------------------------------------------------------------- */
/* Threading / mutexes                                                        */
/* -------------------------------------------------------------------------- */

/*
 * Serialized threading mode by default.
 *
 * This keeps SQLite's mutex subsystem compiled in and allows multiple
 * FreeRTOS tasks to use SQLite safely, assuming you install working mutex
 * methods before sqlite3_initialize().
 */
#define SQLITE_THREADSAFE                    1

/*
 * sqlite_espbdl_init() installs native FreeRTOS mutex methods with
 * SQLITE_CONFIG_MUTEX before sqlite3_initialize().
 */
#undef SQLITE_MUTEX_APPDEF


/* -------------------------------------------------------------------------- */
/* Memory allocation                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Use SQLite's system malloc hooks initially. On ESP-IDF, this maps to the
 * normal C allocator unless you override sqlite3_mem_methods later.
 *
 * Later you can replace this with SQLITE_CONFIG_MALLOC if you want heap_caps
 * routing, PSRAM routing, tracing, or hard memory limits.
 */
#define SQLITE_SYSTEM_MALLOC                 1

/*
 * ESP32 malloc is at least 4-byte aligned. This can reduce SQLite's alignment
 * assumptions on platforms where 8-byte alignment is not guaranteed.
 */
#define SQLITE_4_BYTE_ALIGNED_MALLOC         1

/*
 * Disable runtime memory-status accounting to reduce overhead.
 */
#define SQLITE_DEFAULT_MEMSTATUS             0

/*
 * Use smaller stack frames where SQLite supports it.
 */
#define SQLITE_SMALL_STACK                   1

/*
 * Keep lookaside enabled for now. It is useful on embedded targets.
 * Tune later based on sqlite3_db_status().
 *
 * Format: SQLITE_DEFAULT_LOOKASIDE size,count
 */
#define SQLITE_DEFAULT_LOOKASIDE             128,128

/*
 * Initial page-cache allocation size. Conservative.
 */
#define SQLITE_DEFAULT_PCACHE_INITSZ         8


/* -------------------------------------------------------------------------- */
/* Temporary storage                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Force temporary tables/files into memory.
 *
 * This is important for raw BDL, because you probably only want to support:
 *   - main.db
 *   - main.db-wal
 *
 * and not arbitrary temp files on SD.
 */
#define SQLITE_TEMP_STORE                    3

/*
 * Do NOT omit temp DB during first bring-up. Some SQL operations may need
 * temp storage. With SQLITE_TEMP_STORE=3, it stays in memory.
 */
#undef SQLITE_OMIT_TEMPDB

/*
 * Keep in-memory DB support during bring-up. Useful for tests.
 */
#undef SQLITE_OMIT_MEMORYDB


/* -------------------------------------------------------------------------- */
/* WAL / journaling                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Critical: do not define SQLITE_OMIT_WAL, even to 0.
 * For OMIT-style options, being defined can be enough to omit the feature.
 */
#undef SQLITE_OMIT_WAL

/*
 * Default locking mode = EXCLUSIVE.
 *
 * Still run this explicitly before enabling WAL:
 *
 *   PRAGMA locking_mode=EXCLUSIVE;
 *   PRAGMA journal_mode=WAL;
 */
#define SQLITE_DEFAULT_LOCKING_MODE          1

/*
 * Conservative durability defaults for bring-up.
 *
 * Later, after power-fail testing, you may change WAL synchronous to NORMAL:
 *
 *   #define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
 */
#define SQLITE_DEFAULT_SYNCHRONOUS           2   /* FULL */
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS       2   /* FULL */

/*
 * Smaller WAL checkpoint threshold than desktop default.
 *
 * With 4096-byte pages:
 *   64 pages ~= 256 KiB WAL before autocheckpoint.
 */
#define SQLITE_DEFAULT_WAL_AUTOCHECKPOINT    4

/*
 * Do not claim powersafe overwrite for SD cards unless proven by testing.
 *
 * Your VFS xDeviceCharacteristics() should also be conservative and return 0
 * initially.
 */
#define SQLITE_POWERSAFE_OVERWRITE           0


/* -------------------------------------------------------------------------- */
/* Page/cache defaults                                                        */
/* -------------------------------------------------------------------------- */

#define SQLITE_DEFAULT_PAGE_SIZE             512

// Negative means KB, positive means page
#define SQLITE_DEFAULT_CACHE_SIZE           2


/* -------------------------------------------------------------------------- */
/* SQL feature defaults                                                       */
/* -------------------------------------------------------------------------- */

#define SQLITE_DEFAULT_FOREIGN_KEYS          0
#define SQLITE_SECURE_DELETE                 0
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS       1

/*
 * Keep PRAGMA support. You need PRAGMAs for:
 *   - journal_mode
 *   - locking_mode
 *   - synchronous
 *   - wal_checkpoint
 *   - integrity_check during bring-up
 */
#undef SQLITE_OMIT_PRAGMA
#undef SQLITE_OMIT_PAGER_PRAGMAS
#undef SQLITE_OMIT_SCHEMA_PRAGMAS
#undef SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS
#undef SQLITE_OMIT_FLAG_PRAGMAS

/*
 * Keep these during first bring-up/debug.
 */
#undef SQLITE_OMIT_INTEGRITY_CHECK
#undef SQLITE_OMIT_ANALYZE
#undef SQLITE_OMIT_EXPLAIN
#undef SQLITE_OMIT_REINDEX
#undef SQLITE_OMIT_VACUUM

/*
 * Keep common SQL features until the VFS is stable.
 * Trim later only after your test suite passes.
 */
#undef SQLITE_OMIT_ALTERTABLE
#undef SQLITE_OMIT_AUTOINCREMENT
#undef SQLITE_OMIT_AUTOVACUUM
#undef SQLITE_OMIT_BETWEEN_OPTIMIZATION
#undef SQLITE_OMIT_BLOB_LITERAL
#undef SQLITE_OMIT_CAST
#undef SQLITE_OMIT_CHECK
#undef SQLITE_OMIT_COMPOUND_SELECT
#undef SQLITE_OMIT_CONFLICT_CLAUSE
#undef SQLITE_OMIT_CTE
#undef SQLITE_OMIT_FOREIGN_KEY
#undef SQLITE_OMIT_LIKE_OPTIMIZATION
#undef SQLITE_OMIT_OR_OPTIMIZATION
#undef SQLITE_OMIT_TRIGGER
#undef SQLITE_OMIT_VIEW
#undef SQLITE_OMIT_VIRTUALTABLE

/*
 * Optional but useful for preventing application SQL from switching out of WAL.
 * Keep this enabled if you plan to use sqlite3_set_authorizer().
 */
#undef SQLITE_OMIT_AUTHORIZATION


/* -------------------------------------------------------------------------- */
/* Features normally safe to omit on ESP32                                    */
/* -------------------------------------------------------------------------- */

#define SQLITE_OMIT_LOAD_EXTENSION           1
#define SQLITE_OMIT_UTF16                    1
#define SQLITE_OMIT_SHARED_CACHE             1
#define SQLITE_OMIT_PROGRESS_CALLBACK        1
#define SQLITE_OMIT_COMPILEOPTION_DIAGS      1
#define SQLITE_OMIT_DEPRECATED               1
#define SQLITE_OMIT_TCL_VARIABLE             1
#define SQLITE_OMIT_TRACE                    1

/*
 * Avoid sqlite3_get_table(); use prepared statements instead.
 */
#define SQLITE_OMIT_GET_TABLE                1

/* Keep sqlite3_blob_open/read/write for block-oriented binary workloads. */
#undef SQLITE_OMIT_INCRBLOB

/*
 * Disable built-in test interfaces.
 */
#define SQLITE_OMIT_BUILTIN_TEST             1

/*
 * Optional planner/code-size trims. These should be safe for many embedded
 * workloads, but comment them out if you hit unexpected query planner behavior.
 */
#define SQLITE_OMIT_AUTOMATIC_INDEX          1
#define SQLITE_OMIT_BTREECOUNT               1
#define SQLITE_OMIT_XFER_OPT                 1


/* -------------------------------------------------------------------------- */
/* Parser / stack tuning                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Do not make this too small. 20 is easy to break with non-trivial SQL.
 * 50 is a reasonable embedded starting point.
 */
#define YYSTACKDEPTH                         50

/*
 * No expression depth limit. Avoids parser failures from arbitrary limit.
 * If stack pressure becomes a problem, set a real limit later.
 */
#define SQLITE_MAX_EXPR_DEPTH                0


/* -------------------------------------------------------------------------- */
/* Debugging                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Leave disabled for normal bring-up. Enable only when debugging SQLite
 * internals, not your VFS.
 */
/* #define SQLITE_DEBUG                      1 */
/* #define SQLITE_PERFORMANCE_TRACE          1 */

/*
 * If you define SQLITE_OMIT_AUTOINIT, you must call sqlite3_initialize()
 * manually before any other SQLite API. For your custom mutex setup, manual
 * init is good practice anyway, but keeping autoinit enabled is more forgiving.
 */
/* #define SQLITE_OMIT_AUTOINIT              1 */


/* -------------------------------------------------------------------------- */
/* Optional local malloc aliases                                               */
/* -------------------------------------------------------------------------- */

/*
 * Do not define os_malloc/os_realloc/os_free here unless your port layer uses
 * them directly. SQLite itself does not need these names.
 */
/*
#define os_malloc(x)       malloc(x)
#define os_realloc(x, y)   realloc((x), (y))
#define os_free(x)         free(x)
*/
