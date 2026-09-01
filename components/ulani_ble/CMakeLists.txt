idf_component_register(
    SRCS "src/ulani_proto.c"
         "src/ulani_ble.c"
         "src/ulani_transfer.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bt
    PRIV_REQUIRES esp_timer)
