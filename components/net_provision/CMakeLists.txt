idf_component_register(
    SRCS "src/net_provision.c"
         "src/dns_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi
    PRIV_REQUIRES esp_netif esp_event lwip)
