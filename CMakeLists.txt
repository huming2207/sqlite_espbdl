idf_component_register(
    SRCS "sqlite3.c" "sqlite_espbdl.c" "sqlite_esp_fs.c"
    INCLUDE_DIRS "."
    REQUIRES esp_blockdev esp_timer esp_system freertos
)
