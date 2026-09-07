idf_component_register(
    SRCS "src/diag_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer nvs_flash esp_system esp_hw_support)
