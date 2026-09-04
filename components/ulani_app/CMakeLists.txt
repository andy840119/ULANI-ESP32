idf_component_register(
    SRCS "src/ulani_app.c"
         "src/testpattern.c"
         "src/page_badge.c"
    INCLUDE_DIRS "include"
    REQUIRES ulani_ble ulani_store
    PRIV_REQUIRES esp_timer esp_hw_support nvs_flash net_provision status_led)
