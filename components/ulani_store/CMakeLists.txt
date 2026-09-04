idf_component_register(
    SRCS "src/ulani_store.c"
    INCLUDE_DIRS "include"
    REQUIRES ulani_ble
    PRIV_REQUIRES spiffs)
