idf_component_register(
    SRCS "src/ulani_app.c"
         "src/testpattern.c"
    INCLUDE_DIRS "include"
    REQUIRES ulani_ble ulani_store
    PRIV_REQUIRES esp_timer esp_hw_support nvs_flash)
