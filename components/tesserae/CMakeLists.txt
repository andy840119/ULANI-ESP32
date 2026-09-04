idf_component_register(
    SRCS "src/tesserae.c"
    INCLUDE_DIRS "include"
    REQUIRES ulani_store
    PRIV_REQUIRES esp_http_client json nvs_flash esp_timer esp_hw_support)
