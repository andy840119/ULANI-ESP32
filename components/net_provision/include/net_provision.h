/*
 * SoftAP + captive-portal DNS.
 *
 * The board brings up its own access point; any DNS query answered from it
 * points back at the ESP32, which is what makes phones pop the "sign in to
 * network" sheet straight onto the UI.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ssid;
    const char *password; /* NULL or "" for an open network */
    uint8_t     channel;
    uint8_t     max_connections;
} net_ap_cfg_t;

esp_err_t net_provision_start_ap(const net_ap_cfg_t *cfg);

/* Answers every A query with the AP address (192.168.4.1 by default). */
esp_err_t net_dns_hijack_start(void);
void      net_dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
