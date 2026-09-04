/*
 * SoftAP + captive-portal DNS, plus an optional station connection to the
 * user's own network.
 *
 * The access point stays up at all times: it is how the UI is reached before
 * any credentials exist, and how it is recovered if those credentials stop
 * working. Joining a home network is additive, never a replacement.
 *
 * One radio means one channel, so associating as a station forces the access
 * point onto the router's channel and briefly drops whoever was connected to
 * it. That is unavoidable on this hardware; the UI warns about it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SSID_MAX 33 /* 32 characters plus terminator */
#define NET_PASS_MAX 64

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

/* ----------------------------------------------------------------- station */

typedef enum {
    NET_STA_DISABLED = 0, /* no credentials stored */
    NET_STA_CONNECTING,
    NET_STA_CONNECTED,
    NET_STA_FAILED,       /* credentials stored but the join keeps failing */
} net_sta_state_t;

typedef struct {
    net_sta_state_t state;
    char            ssid[NET_SSID_MAX];
    char            ip[16];
    int8_t          rssi;
    /* Why the last attempt failed, as an esp_wifi disconnect reason code. */
    uint8_t         last_reason;
} net_sta_status_t;

const char *net_sta_state_str(net_sta_state_t s);

/* Brings the station interface up and joins the stored network, if any. */
esp_err_t net_sta_start(void);

/* Stores the credentials and (re)joins. Pass an empty password for open APs. */
esp_err_t net_sta_connect(const char *ssid, const char *password);

/* Drops the association and erases the stored credentials. */
esp_err_t net_sta_forget(void);

void net_sta_get_status(net_sta_status_t *out);

/* ------------------------------------------------------------------- scan */

typedef struct {
    char    ssid[NET_SSID_MAX];
    int8_t  rssi;
    uint8_t channel;
    bool    open; /* no authentication required */
} net_scan_result_t;

#define NET_SCAN_MAX 20

/*
 * Starts an asynchronous scan. Scanning takes the radio away from the access
 * point for a moment, so the UI should not poll this on a timer.
 */
esp_err_t net_wifi_scan_start(void);
bool      net_wifi_scan_busy(void);
size_t    net_wifi_scan_results(net_scan_result_t *out, size_t max);

/* ------------------------------------------------------------------ power */

/*
 * Full radio power (on = true) for the length of a BLE image transfer, then
 * back to modem sleep (on = false). WiFi and BLE share one antenna, so a
 * transfer needs the radio awake or it starves; everything else runs fine on
 * modem sleep, which is what keeps an idle board from running warm (issue #23).
 * Safe to call from any task once the AP has started.
 */
void net_power_boost(bool on);

#ifdef __cplusplus
}
#endif
