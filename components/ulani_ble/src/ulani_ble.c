/*
 * ULANI BLE central: GAP scanning, connection setup and the op/data channels.
 *
 * Transcribed from Grassboy's ULANI.node.js (src/BLEComm.js). The Node version
 * leans on noble and the host OS for pairing and MTU negotiation; here we have
 * to do both ourselves, which is where most of the extra code below comes from.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ulani_ble_priv.h"

static const char *TAG = "ulani_ble";

/* 128-bit UUIDs. NimBLE wants them little-endian, i.e. string order reversed. */
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x59, 0x5a, 0x08, 0xe4, 0x86, 0x2a, 0x9e, 0x8f,
    0xe9, 0x11, 0xbc, 0x7c, 0x00, 0xa2, 0x34, 0x12);
static const ble_uuid128_t UUID_OP = BLE_UUID128_INIT(
    0x59, 0x5a, 0x08, 0xe4, 0x86, 0x2a, 0x9e, 0x8f,
    0xe9, 0x11, 0xbc, 0x7c, 0x01, 0xa2, 0x34, 0x12);
static const ble_uuid128_t UUID_DAT = BLE_UUID128_INIT(
    0x59, 0x5a, 0x08, 0xe4, 0x86, 0x2a, 0x9e, 0x8f,
    0xe9, 0x11, 0xbc, 0x7c, 0x02, 0xa2, 0x34, 0x12);

#define SEEN_MAX 8

static struct {
    ulani_event_cb_t cb;
    void            *cb_user;

    ulani_state_t state;
    bool          host_synced;
    uint8_t       own_addr_type;

    uint16_t conn_handle;
    uint16_t svc_start, svc_end;
    uint16_t op_val, dat_val;
    uint16_t op_cccd, dat_cccd;
    uint16_t mtu;

    SemaphoreHandle_t api_lock;

    SemaphoreHandle_t proc_sem;   /* generic GATT procedure completion */
    volatile int      proc_status;

    SemaphoreHandle_t write_sem;
    volatile int      write_status;

    SemaphoreHandle_t conn_sem;
    volatile int      conn_status;

    SemaphoreHandle_t op_sem;
    volatile uint8_t  op_expect;
    volatile uint16_t op_rsp;

    SemaphoreHandle_t dat_sem;
    volatile bool     dat_armed;
    volatile bool     dat_ready;
    volatile uint16_t dat_rsp;

    volatile bool abort_req;

    ulani_device_t seen[SEEN_MAX];
    int            seen_n;
} s;

const char *ulani_state_str(ulani_state_t st)
{
    switch (st) {
    case ULANI_STATE_OFF:          return "off";
    case ULANI_STATE_IDLE:         return "idle";
    case ULANI_STATE_SCANNING:     return "scanning";
    case ULANI_STATE_CONNECTING:   return "connecting";
    case ULANI_STATE_DISCOVERING:  return "discovering";
    case ULANI_STATE_READY:        return "ready";
    case ULANI_STATE_TRANSFERRING: return "transferring";
    }
    return "?";
}

void ulani_emit(const ulani_event_t *ev)
{
    if (s.cb) {
        s.cb(ev, s.cb_user);
    }
}

void ulani_set_state(ulani_state_t st)
{
    if (s.state == st) {
        return;
    }
    s.state = st;
    ESP_LOGI(TAG, "state -> %s", ulani_state_str(st));
    ulani_event_t ev = { .type = ULANI_EV_STATE_CHANGED, .state_changed = { .state = st } };
    ulani_emit(&ev);
}

ulani_state_t ulani_ble_state(void) { return s.state; }
bool     ulani_ble_is_connected(void) { return s.conn_handle != BLE_HS_CONN_HANDLE_NONE; }
uint16_t ulani_op_handle(void)        { return s.op_val; }
uint16_t ulani_dat_handle(void)       { return s.dat_val; }

bool ulani_transfer_abort_requested(void) { return s.abort_req; }
void ulani_transfer_abort_clear(void)     { s.abort_req = false; }
void ulani_ble_abort_transfer(void)       { s.abort_req = true; }

/* ------------------------------------------------------------------ utils */

static void addr_to_str(const ble_addr_t *a, char out[18])
{
    const uint8_t *v = a->val; /* NimBLE stores addresses little-endian */
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             v[5], v[4], v[3], v[2], v[1], v[0]);
}

static bool str_to_addr(const char *str, ble_addr_t *out)
{
    unsigned b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t)b[5 - i];
    }
    out->type = BLE_ADDR_PUBLIC;
    return true;
}

static void sem_drain(SemaphoreHandle_t sem)
{
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

/* ------------------------------------------------------- GATT procedures */

static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s.write_status = err->status;
    xSemaphoreGive(s.write_sem);
    return 0;
}

esp_err_t ulani_gatt_write(uint16_t val_handle, const void *data, uint16_t len,
                           uint32_t timeout_ms)
{
    if (s.conn_handle == BLE_HS_CONN_HANDLE_NONE || val_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    sem_drain(s.write_sem);
    s.write_status = 0;

    int rc = ble_gattc_write_flat(s.conn_handle, val_handle, data, len,
                                  on_write_done, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "write_flat handle=%u rc=%d", val_handle, rc);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s.write_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "write timeout handle=%u", val_handle);
        return ESP_ERR_TIMEOUT;
    }
    if (s.write_status != 0) {
        ESP_LOGE(TAG, "write status=%d handle=%u", s.write_status, val_handle);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ulani_op_exec(const uint8_t *frame, uint16_t len, bool wait_rsp, uint16_t *rsp)
{
    if (len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    s.op_expect = frame[0];
    sem_drain(s.op_sem);

    esp_err_t err = ulani_gatt_write(s.op_val, frame, len, 5000);
    if (err != ESP_OK) {
        return err;
    }
    if (!wait_rsp) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s.op_sem, pdMS_TO_TICKS(ULANI_OP_TIMEOUT_MS)) != pdTRUE) {
        /* The JS resolves this as "<op>9999" and carries on; we surface it. */
        ESP_LOGW(TAG, "op 0x%02x timed out", frame[0]);
        return ESP_ERR_TIMEOUT;
    }
    if (rsp) {
        *rsp = s.op_rsp;
    }
    return ESP_OK;
}

void ulani_data_result_arm(void)
{
    sem_drain(s.dat_sem);
    s.dat_ready = false;
    s.dat_rsp   = 0;
    s.dat_armed = true;
}

bool ulani_data_result_ready(void) { return s.dat_ready; }

esp_err_t ulani_data_result_wait(uint32_t timeout_ms, uint16_t *rsp)
{
    if (!s.dat_ready &&
        xSemaphoreTake(s.dat_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s.dat_armed = false;
        return ESP_ERR_TIMEOUT;
    }
    if (rsp) {
        *rsp = s.dat_rsp;
    }
    s.dat_armed = false;
    return s.dat_ready ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------- discovery steps */

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && svc) {
        s.svc_start = svc->start_handle;
        s.svc_end   = svc->end_handle;
        return 0;
    }
    s.proc_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
    xSemaphoreGive(s.proc_sem);
    return 0;
}

static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_OP.u) == 0) {
            s.op_val = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_DAT.u) == 0) {
            s.dat_val = chr->val_handle;
        }
        return 0;
    }
    s.proc_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
    xSemaphoreGive(s.proc_sem);
    return 0;
}

static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            if (chr_val_handle == s.op_val) {
                s.op_cccd = dsc->handle;
            } else if (chr_val_handle == s.dat_val) {
                s.dat_cccd = dsc->handle;
            }
        }
        return 0;
    }
    s.proc_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
    xSemaphoreGive(s.proc_sem);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    (void)conn; (void)arg;
    s.mtu         = mtu;
    s.proc_status = err->status;
    xSemaphoreGive(s.proc_sem);
    return 0;
}

static esp_err_t wait_proc(const char *what, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s.proc_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "%s timed out", what);
        return ESP_ERR_TIMEOUT;
    }
    if (s.proc_status != 0) {
        ESP_LOGE(TAG, "%s failed status=%d", what, s.proc_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t subscribe(uint16_t cccd_handle, const char *what)
{
    if (cccd_handle == 0) {
        ESP_LOGE(TAG, "%s characteristic has no CCCD", what);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t value[2] = { 0x01, 0x00 }; /* notifications */
    esp_err_t err = ulani_gatt_write(cccd_handle, value, sizeof(value), 5000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "subscribe %s failed", what);
    }
    return err;
}

/* ------------------------------------------------------------ GAP events */

static void seen_reset(void) { s.seen_n = 0; }

static bool seen_add(const ulani_device_t *dev)
{
    for (int i = 0; i < s.seen_n; i++) {
        if (strcmp(s.seen[i].addr, dev->addr) == 0) {
            return false;
        }
    }
    if (s.seen_n < SEEN_MAX) {
        s.seen[s.seen_n++] = *dev;
    }
    return true;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) {
            return 0;
        }
        if (fields.name == NULL || fields.name_len == 0) {
            return 0;
        }

        ulani_device_t dev = { 0 };
        size_t n = fields.name_len < sizeof(dev.name) - 1 ? fields.name_len
                                                          : sizeof(dev.name) - 1;
        memcpy(dev.name, fields.name, n);
        dev.name[n] = 0;

        if (strncmp(dev.name, ULANI_NAME_PREFIX, strlen(ULANI_NAME_PREFIX)) != 0) {
            return 0;
        }

        addr_to_str(&event->disc.addr, dev.addr);
        dev.addr_type = event->disc.addr.type;
        dev.rssi      = event->disc.rssi;

        if (seen_add(&dev)) {
            ESP_LOGI(TAG, "found %s [%s] rssi=%d", dev.name, dev.addr, dev.rssi);
            ulani_event_t ev = { .type = ULANI_EV_DEVICE_FOUND,
                                 .device_found = { .dev = dev } };
            ulani_emit(&ev);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        if (s.state == ULANI_STATE_SCANNING) {
            ulani_set_state(ULANI_STATE_IDLE);
        }
        ulani_event_t ev = { .type = ULANI_EV_SCAN_DONE };
        ulani_emit(&ev);
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        s.conn_status = event->connect.status;
        if (event->connect.status == 0) {
            s.conn_handle = event->connect.conn_handle;
        }
        xSemaphoreGive(s.conn_sem);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGW(TAG, "disconnected reason=%d", event->disconnect.reason);
        s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s.op_val = s.dat_val = s.op_cccd = s.dat_cccd = 0;
        s.abort_req = true; /* unblock a transfer that is mid-flight */
        xSemaphoreGive(s.op_sem);
        xSemaphoreGive(s.dat_sem);
        xSemaphoreGive(s.write_sem);
        ulani_set_state(ULANI_STATE_IDLE);
        ulani_event_t ev = { .type = ULANI_EV_DISCONNECTED,
                             .disconnected = { .reason = event->disconnect.reason } };
        ulani_emit(&ev);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_MTU:
        s.mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU = %u", s.mtu);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t  buf[4] = { 0 };
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &copied);
        if (copied == 0) {
            return 0;
        }
        uint16_t rsp = (uint16_t)((buf[0] << 8) | (copied > 1 ? buf[1] : 0));
        ESP_LOGD(TAG, "notify handle=%u rsp=%04x", event->notify_rx.attr_handle, rsp);

        if (buf[0] == ULANI_OP_IMAGE_RESULT && s.dat_armed) {
            s.dat_rsp   = rsp;
            s.dat_ready = true;
            xSemaphoreGive(s.dat_sem);
        } else if (buf[0] == s.op_expect) {
            s.op_rsp = rsp;
            xSemaphoreGive(s.op_sem);
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------------- host task */

static void on_host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    s.host_synced = true;
    ulani_set_state(ULANI_STATE_IDLE);
    ESP_LOGI(TAG, "host synced, own addr type %d", s.own_addr_type);
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    s.host_synced = false;
    s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ulani_set_state(ULANI_STATE_OFF);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------ public API */

esp_err_t ulani_ble_init(const ulani_ble_cfg_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (cfg) {
        s.cb      = cfg->event_cb;
        s.cb_user = cfg->event_user;
    }

    s.api_lock  = xSemaphoreCreateMutex();
    s.proc_sem  = xSemaphoreCreateBinary();
    s.write_sem = xSemaphoreCreateBinary();
    s.conn_sem  = xSemaphoreCreateBinary();
    s.op_sem    = xSemaphoreCreateBinary();
    s.dat_sem   = xSemaphoreCreateBinary();
    if (!s.api_lock || !s.proc_sem || !s.write_sem || !s.conn_sem ||
        !s.op_sem || !s.dat_sem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb         = on_host_sync;
    ble_hs_cfg.reset_cb        = on_host_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 0;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /*
     * No ble_svc_gap_init() here: we build as a pure central, so the peripheral
     * GAP service is compiled out and there is nothing to register.
     */
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ulani_ble_scan_start(uint32_t duration_ms)
{
    if (!s.host_synced || s.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    seen_reset();

    struct ble_gap_disc_params params = {
        .itvl              = 0,
        .window            = 0,
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = 0, /* active scan: the name may be in a scan response */
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(s.own_addr_type, duration_ms, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return ESP_FAIL;
    }
    ulani_set_state(ULANI_STATE_SCANNING);
    return ESP_OK;
}

esp_err_t ulani_ble_scan_stop(void)
{
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    if (s.state == ULANI_STATE_SCANNING) {
        ulani_set_state(ULANI_STATE_IDLE);
    }
    return ESP_OK;
}

esp_err_t ulani_ble_connect(const char *addr_str, uint32_t timeout_ms)
{
    if (!s.host_synced || s.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_addr_t addr;
    if (!str_to_addr(addr_str, &addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Prefer the address type we actually saw while scanning. */
    for (int i = 0; i < s.seen_n; i++) {
        if (strcmp(s.seen[i].addr, addr_str) == 0) {
            addr.type = s.seen[i].addr_type;
            break;
        }
    }

    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }

    xSemaphoreTake(s.api_lock, portMAX_DELAY);
    ulani_set_state(ULANI_STATE_CONNECTING);

    sem_drain(s.conn_sem);
    s.conn_status = -1;

    esp_err_t err = ESP_OK;
    int rc = ble_gap_connect(s.own_addr_type, &addr, timeout_ms, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        err = ESP_FAIL;
        goto out;
    }
    if (xSemaphoreTake(s.conn_sem, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE) {
        ble_gap_conn_cancel();
        err = ESP_ERR_TIMEOUT;
        goto out;
    }
    if (s.conn_status != 0) {
        ESP_LOGE(TAG, "connect failed status=%d", s.conn_status);
        err = ESP_FAIL;
        goto out;
    }

    ulani_set_state(ULANI_STATE_DISCOVERING);
    s.abort_req = false;

#if CONFIG_ULANI_BLE_INITIATE_SECURITY
    /*
     * The Node.js flow requires an OS-level pairing first, so the panel very
     * likely wants an encrypted link. Ask for it, but do not give up if the
     * device refuses: some units may be happy unencrypted.
     */
    rc = ble_gap_security_initiate(s.conn_handle);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "security_initiate rc=%d (continuing unencrypted)", rc);
    }
#endif

    /* MTU first: a 230-byte write needs at least 233. */
    sem_drain(s.proc_sem);
    rc = ble_gattc_exchange_mtu(s.conn_handle, on_mtu, NULL);
    if (rc == 0) {
        wait_proc("mtu exchange", 5000);
    }
    if (s.mtu < ULANI_CHUNK_BYTES + 3) {
        ESP_LOGW(TAG, "MTU %u < %d: image writes may be rejected",
                 s.mtu, ULANI_CHUNK_BYTES + 3);
    }

    sem_drain(s.proc_sem);
    s.svc_start = s.svc_end = 0;
    rc = ble_gattc_disc_svc_by_uuid(s.conn_handle, &UUID_SVC.u, on_disc_svc, NULL);
    err = (rc != 0) ? ESP_FAIL : wait_proc("service discovery", 10000);
    if (err != ESP_OK) {
        goto fail;
    }
    if (s.svc_start == 0) {
        ESP_LOGE(TAG, "ULANI service not present on this device");
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    sem_drain(s.proc_sem);
    rc = ble_gattc_disc_all_chrs(s.conn_handle, s.svc_start, s.svc_end, on_disc_chr, NULL);
    err = (rc != 0) ? ESP_FAIL : wait_proc("characteristic discovery", 10000);
    if (err != ESP_OK) {
        goto fail;
    }
    if (s.op_val == 0 || s.dat_val == 0) {
        ESP_LOGE(TAG, "op/data characteristic missing (op=%u dat=%u)", s.op_val, s.dat_val);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    sem_drain(s.proc_sem);
    rc = ble_gattc_disc_all_dscs(s.conn_handle, s.svc_start, s.svc_end, on_disc_dsc, NULL);
    err = (rc != 0) ? ESP_FAIL : wait_proc("descriptor discovery", 10000);
    if (err != ESP_OK) {
        goto fail;
    }

    err = subscribe(s.op_cccd, "op");
    if (err != ESP_OK) {
        goto fail;
    }
    err = subscribe(s.dat_cccd, "data");
    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "ready: op=%u data=%u mtu=%u", s.op_val, s.dat_val, s.mtu);
    ulani_set_state(ULANI_STATE_READY);
    {
        ulani_event_t ev = { .type = ULANI_EV_CONNECTED };
        ulani_emit(&ev);
    }
    xSemaphoreGive(s.api_lock);
    return ESP_OK;

fail:
    if (s.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
out:
    if (s.state == ULANI_STATE_CONNECTING || s.state == ULANI_STATE_DISCOVERING) {
        ulani_set_state(ULANI_STATE_IDLE);
    }
    xSemaphoreGive(s.api_lock);
    return err;
}

esp_err_t ulani_ble_disconnect(void)
{
    if (s.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_OK;
    }
    ble_gap_terminate(s.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return ESP_OK;
}

/* -------------------------------------------------------- opcode wrappers */

esp_err_t ulani_ble_check_customer_id(uint16_t *rsp)
{
    const uint8_t frame[] = { 0x04, 0x4e, 0x42 };
    return ulani_op_exec(frame, sizeof(frame), true, rsp);
}

esp_err_t ulani_ble_get_battery(uint16_t *rsp)
{
    const uint8_t frame[] = { 0x06, 0x00 };
    return ulani_op_exec(frame, sizeof(frame), true, rsp);
}

esp_err_t ulani_ble_ack(void)
{
    const uint8_t frame[] = { 0x06, 0x00 };
    return ulani_op_exec(frame, sizeof(frame), false, NULL);
}

esp_err_t ulani_ble_ask_disconnect(void)
{
    const uint8_t frame[] = { 0x09, 0x03 };
    return ulani_op_exec(frame, sizeof(frame), true, NULL);
}

esp_err_t ulani_ble_get_active_slot(uint8_t *slot)
{
    const uint8_t frame[] = { 0x0c, 0x00 };
    uint16_t rsp = 0;
    esp_err_t err = ulani_op_exec(frame, sizeof(frame), true, &rsp);
    if (err != ESP_OK) {
        return err;
    }
    if ((rsp >> 8) != ULANI_OP_GET_SLOT) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (slot) {
        *slot = (uint8_t)(rsp & 0x0f);
    }
    return ESP_OK;
}

esp_err_t ulani_ble_set_active_slot(uint8_t slot)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t frame[] = { 0x0b, slot };
    uint16_t rsp = 0;
    esp_err_t err = ulani_op_exec(frame, sizeof(frame), true, &rsp);
    if (err != ESP_OK) {
        return err;
    }
    if (rsp != 0x0b00) {
        ESP_LOGW(TAG, "set slot rejected rsp=%04x", rsp);
        return ESP_FAIL;
    }
    ulani_event_t ev = { .type = ULANI_EV_SLOT_CHANGED, .slot_changed = { .slot = slot } };
    ulani_emit(&ev);
    return ESP_OK;
}
