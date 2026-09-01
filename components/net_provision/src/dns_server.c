/*
 * Minimal DNS responder for the captive portal: every A query is answered with
 * the SoftAP address, so whatever hostname the phone probes resolves to us and
 * the "sign in to network" sheet opens on the UI.
 */

#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "net_provision.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT       53
#define DNS_MAX_LEN    512
#define DNS_QR_RESPONSE 0x80
#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1
#define DNS_TTL_SECONDS 60

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t ptr;
    uint16_t type;
    uint16_t klass;
    uint32_t ttl;
    uint16_t rd_length;
    uint32_t addr;
} dns_answer_t;

static TaskHandle_t s_task;
static int          s_sock = -1;

static uint32_t ap_address(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0;
}

/* Returns the length of the response, or 0 if the query is not answerable. */
static int build_response(uint8_t *buf, int len, int cap)
{
    if (len < (int)sizeof(dns_header_t)) {
        return 0;
    }

    dns_header_t *hdr = (dns_header_t *)buf;
    if (ntohs(hdr->qd_count) != 1) {
        return 0;
    }

    /* Walk the QNAME labels to find where the question ends. */
    int p = sizeof(dns_header_t);
    while (p < len && buf[p] != 0) {
        p += buf[p] + 1;
    }
    p += 1 + 4; /* terminating zero + QTYPE + QCLASS */
    if (p > len || p + (int)sizeof(dns_answer_t) > cap) {
        return 0;
    }

    hdr->flags    = htons(DNS_QR_RESPONSE << 8 | 0x0080); /* response + RA */
    hdr->an_count = htons(1);
    hdr->ns_count = 0;
    hdr->ar_count = 0;

    dns_answer_t ans = {
        .ptr       = htons(0xc000 | sizeof(dns_header_t)), /* pointer to QNAME */
        .type      = htons(DNS_TYPE_A),
        .klass     = htons(DNS_CLASS_IN),
        .ttl       = htonl(DNS_TTL_SECONDS),
        .rd_length = htons(4),
        .addr      = ap_address(),
    };
    memcpy(buf + p, &ans, sizeof(ans));
    return p + sizeof(ans);
}

static void dns_task(void *param)
{
    (void)param;
    uint8_t buf[DNS_MAX_LEN];

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot bind UDP/53 (errno %d)", errno);
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on UDP/53");

    for (;;) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            if (s_sock < 0) {
                break;
            }
            continue;
        }
        int rlen = build_response(buf, n, sizeof(buf));
        if (rlen > 0) {
            sendto(s_sock, buf, rlen, 0, (struct sockaddr *)&from, from_len);
        }
    }

    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t net_dns_hijack_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_dns_hijack_stop(void)
{
    int sock = s_sock;
    s_sock = -1;
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
}
