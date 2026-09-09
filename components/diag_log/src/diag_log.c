#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "diag_log.h"

static const char *TAG = "diag";

#define NVS_NAMESPACE "diag"

/* How the drain task paces itself. */
#define FLUSH_INTERVAL_MS 30000
#define FLUSH_THRESHOLD   32   /* records queued before an early flush */
#define FLUSH_BATCH       32   /* records per write() call: 768 bytes of stack */

static struct {
    SemaphoreHandle_t lock;

    diag_rec_t *ring;
    uint16_t    cap;
    uint16_t    count;
    uint16_t    head;    /* next slot to write */
    uint32_t    next_seq;
    uint32_t    dropped; /* recycled out of the ring, whether or not persisted */
    uint32_t    boot_id;

    /* Text trace: a byte ring of complete lines. */
    char    *trace;
    uint32_t trace_cap;
    uint32_t trace_len;
    uint32_t trace_head;
    uint32_t trace_total;
    int      trace_level;
    vprintf_like_t trace_next; /* the console writer we displaced */

    diag_write_fn write;
    diag_busy_fn  busy;
    void         *user;
    uint32_t      flushed_seq; /* everything below this has reached the sink */

    bool enabled;
    bool persist;
    bool started;
} s;

static void lock(void)   { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

/* ---------------------------------------------------------------- settings */

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, "on", &v) == ESP_OK) {
        s.enabled = v != 0;
    }
    if (nvs_get_u8(h, "flash", &v) == ESP_OK) {
        s.persist = v != 0;
    }
    if (nvs_get_u8(h, "trace", &v) == ESP_OK) {
        s.trace_level = v;
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "on", s.enabled ? 1 : 0);
    nvs_set_u8(h, "flash", s.persist ? 1 : 0);
    nvs_set_u8(h, "trace", (uint8_t)s.trace_level);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ clock */

/*
 * The wall clock is only real once something has set it (SNTP, or the Date
 * header as a fallback). Before that a record carries 0 rather than a time in
 * 1970, and uptime_ms is what orders it.
 */
static uint32_t now_epoch(void)
{
    time_t now = time(NULL);
    return (now > 1600000000) ? (uint32_t)now : 0;
}

/* ------------------------------------------------------------------ writing */

void diag_log(uint16_t code, uint8_t slot, int8_t result, int32_t a, int32_t b)
{
    if (!s.started || !s.enabled || !s.ring) {
        return;
    }

    diag_rec_t rec = {
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .epoch     = now_epoch(),
        .code      = code,
        .slot      = slot,
        .result    = result,
        .a         = a,
        .b         = b,
    };

    lock();
    rec.seq         = s.next_seq++;
    s.ring[s.head]  = rec;
    s.head          = (uint16_t)((s.head + 1) % s.cap);
    if (s.count < s.cap) {
        s.count++;
    } else {
        /*
         * The ring is full and this record just overwrote the oldest one.
         * That is only a loss if it had not reached the sink yet: with the
         * flash tier on, the ring turning over is the normal state of affairs
         * and counting it would have the log claim thousands of missing
         * records while holding every one of them.
         */
        uint32_t oldest_kept = rec.seq - s.cap + 1;
        if (s.flushed_seq < oldest_kept) {
            s.dropped += oldest_kept - s.flushed_seq;
            s.flushed_seq = oldest_kept;
        }
    }
    unlock();
}

void diag_log_note_dropped(uint32_t records)
{
    if (records == 0) {
        return;
    }
    lock();
    s.dropped += records;
    unlock();
    diag_log(DIAG_SYS_LOG_DROPPED, 0, 0, (int32_t)records, 0);
}

void diag_log_note_flash_full(uint32_t free_bytes)
{
    diag_log(DIAG_SYS_LOG_PAUSED, 0, 0, (int32_t)free_bytes, 0);
}

/* ------------------------------------------------------------------ reading */

void diag_log_get_stats(diag_log_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s.lock) {
        return;
    }
    lock();
    out->next_seq    = s.next_seq;
    out->flushed_seq = s.flushed_seq;
    out->count       = s.count;
    out->capacity    = s.cap;
    out->dropped     = s.dropped;
    out->oldest_seq  = (s.count > 0) ? s.next_seq - s.count : s.next_seq;
    out->boot_id     = s.boot_id;
    out->enabled     = s.enabled;
    out->persist     = s.persist;
    out->trace_level = s.trace_level;
    out->trace_len   = s.trace_len;
    out->trace_total = s.trace_total;
    unlock();
}

size_t diag_log_read(uint32_t since, diag_rec_t *out, size_t max)
{
    if (!out || max == 0 || !s.ring) {
        return 0;
    }

    size_t n = 0;
    lock();
    uint32_t oldest = (s.count > 0) ? s.next_seq - s.count : s.next_seq;
    uint32_t from   = (since > oldest) ? since : oldest;
    for (uint32_t seq = from; seq < s.next_seq && n < max; seq++) {
        uint16_t idx = (uint16_t)((s.head + s.cap - (uint16_t)(s.next_seq - seq)) % s.cap);
        out[n++] = s.ring[idx];
    }
    unlock();
    return n;
}

size_t diag_trace_read(uint32_t *from, char *out, size_t max)
{
    if (!from || !out || max == 0 || !s.trace) {
        return 0;
    }

    lock();
    uint32_t oldest = s.trace_total - s.trace_len;
    if (*from < oldest) {
        *from = oldest; /* those bytes have been recycled */
    }
    size_t n = 0;
    while (*from + n < s.trace_total && n < max) {
        uint32_t pos = (s.trace_head + s.trace_cap - (s.trace_total - (*from + n))) % s.trace_cap;
        out[n++] = s.trace[pos];
    }
    unlock();
    *from += n;
    return n;
}

/* ---------------------------------------------------------------- clearing */

void diag_log_clear(void)
{
    lock();
    s.count       = 0;
    s.head        = 0;
    s.dropped     = 0;
    s.flushed_seq = s.next_seq; /* seq keeps counting: gaps stay visible */
    s.trace_len   = 0;
    s.trace_head  = 0;
    unlock();
    diag_log(DIAG_SYS_LOG_CLEARED, 0, 0, 0, 0);
}

void diag_log_set_enabled(bool on)
{
    lock();
    s.enabled = on;
    unlock();
    settings_save();
    ESP_LOGI(TAG, "event log %s", on ? "on" : "off");
}

void diag_log_set_persist(bool on)
{
    lock();
    s.persist = on;
    unlock();
    settings_save();
}

void diag_log_set_trace_level(int level)
{
    lock();
    s.trace_level = level;
    unlock();
    settings_save();
}

/* ------------------------------------------------------------- text trace */

static void trace_put(const char *text, size_t len)
{
    if (!s.trace || len == 0) {
        return;
    }
    if (len > s.trace_cap) { /* keep the tail of an over-long line */
        text += len - s.trace_cap;
        len = s.trace_cap;
    }
    for (size_t i = 0; i < len; i++) {
        s.trace[s.trace_head] = text[i];
        s.trace_head = (s.trace_head + 1) % s.trace_cap;
    }
    s.trace_total += len;
    s.trace_len = (s.trace_len + len > s.trace_cap) ? s.trace_cap : s.trace_len + len;
}

/*
 * The hook sees a line already formatted for the console. With colours off
 * (CONFIG_LOG_COLORS unset, as this project builds) the level is the first
 * character -- "I (1234) tag: ...". Anything that does not look like a log
 * line is kept: it is more likely a panic or a bootloader message than noise.
 */
static int trace_level_of(const char *line)
{
    switch (line[0]) {
    case 'E': return ESP_LOG_ERROR;
    case 'W': return ESP_LOG_WARN;
    case 'I': return ESP_LOG_INFO;
    case 'D': return ESP_LOG_DEBUG;
    case 'V': return ESP_LOG_VERBOSE;
    default:  return ESP_LOG_ERROR;
    }
}

static int trace_vprintf(const char *fmt, va_list ap)
{
    /*
     * The console still gets everything: the capture is a tee, not a
     * redirect, so a board that does have a serial cable behaves as before.
     * Formatting happens once, here, and the copy is passed on.
     */
    char line[192];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0 && s.trace && s.started) {
        size_t len = ((size_t)n < sizeof(line) - 1) ? (size_t)n : sizeof(line) - 1;
        if (trace_level_of(line) <= s.trace_level) {
            /*
             * A dropped line beats a stalled task: the console writer below
             * runs on whatever task logged, including ones holding locks.
             */
            if (xSemaphoreTake(s.lock, 0) == pdTRUE) {
                trace_put(line, len);
                xSemaphoreGive(s.lock);
            }
        }
    }

    return s.trace_next ? s.trace_next(fmt, ap) : vprintf(fmt, ap);
}

/* --------------------------------------------------------------- draining */

static void flush_to_sink(void)
{
    if (!s.write || !s.persist) {
        return;
    }
    if (s.busy && s.busy(s.user)) {
        return; /* an image is going out; flash can wait */
    }

    for (;;) {
        diag_rec_t batch[FLUSH_BATCH];
        size_t     n = 0;

        lock();
        uint32_t oldest = (s.count > 0) ? s.next_seq - s.count : s.next_seq;
        if (s.flushed_seq < oldest) {
            s.flushed_seq = oldest;
        }
        for (uint32_t seq = s.flushed_seq; seq < s.next_seq && n < FLUSH_BATCH; seq++) {
            uint16_t idx = (uint16_t)((s.head + s.cap - (uint16_t)(s.next_seq - seq)) % s.cap);
            batch[n++] = s.ring[idx];
        }
        unlock();

        if (n == 0) {
            return;
        }
        if (s.write(batch, n * sizeof(batch[0]), s.user) != ESP_OK) {
            return; /* leave flushed_seq alone and try again next time */
        }
        lock();
        s.flushed_seq += n;
        unlock();
    }
}

static void diag_task(void *param)
{
    (void)param;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t pending;
        lock();
        pending = s.next_seq - s.flushed_seq;
        unlock();

        bool due = (xTaskGetTickCount() - last) >= pdMS_TO_TICKS(FLUSH_INTERVAL_MS);
        if (pending >= FLUSH_THRESHOLD || (due && pending > 0)) {
            flush_to_sink();
            last = xTaskGetTickCount();
        }

        /*
         * Heap headroom, once a minute, and only when it moves: this is what
         * says whether the rings are affordable on a board that is also
         * running WiFi, BLE and a web server.
         */
        static uint32_t tick;
        static uint32_t last_min;
        if (++tick % 60 == 0) {
            uint32_t min = (uint32_t)esp_get_minimum_free_heap_size();
            if (min != last_min) {
                last_min = min;
                diag_log(DIAG_SYS_HEAP, 0, 0, (int32_t)esp_get_free_heap_size(),
                         (int32_t)min);
            }
        }
    }
}

/* -------------------------------------------------------------- lifecycle */

esp_err_t diag_log_start(const diag_log_cfg_t *cfg)
{
    if (s.started) {
        return ESP_ERR_INVALID_STATE;
    }

    s.lock = xSemaphoreCreateMutex();
    if (!s.lock) {
        return ESP_ERR_NO_MEM;
    }

    s.enabled     = true;
    s.persist     = true;
    s.trace_level = ESP_LOG_WARN; /* INFO is chatty; the UI can turn it up */
    settings_load();

    s.cap = (cfg && cfg->records) ? cfg->records : DIAG_DEFAULT_RECORDS;
    s.ring = calloc(s.cap, sizeof(diag_rec_t));
    if (!s.ring) {
        ESP_LOGE(TAG, "no room for %u records", (unsigned)s.cap);
        return ESP_ERR_NO_MEM;
    }

    uint16_t trace_bytes = (cfg && cfg->trace_bytes) ? cfg->trace_bytes
                                                     : DIAG_DEFAULT_TRACE;
    if (trace_bytes) {
        s.trace = calloc(1, trace_bytes);
        if (s.trace) {
            s.trace_cap = trace_bytes;
        } else {
            ESP_LOGW(TAG, "no room for a %u byte trace; records only",
                     (unsigned)trace_bytes);
        }
    }

    if (cfg) {
        s.write = cfg->write;
        s.busy  = cfg->busy;
        s.user  = cfg->user;
    }

    s.boot_id = esp_random();
    s.started = true;

    if (s.trace) {
        s.trace_next = esp_log_set_vprintf(trace_vprintf);
    }

    if (xTaskCreate(diag_task, "diag", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no drain task; the log stays in RAM");
    }

    diag_log(DIAG_SYS_BOOT, 0, 0, (int32_t)esp_reset_reason(), (int32_t)s.boot_id);
    ESP_LOGI(TAG, "event log ready: %u records, %u byte trace, flash %s",
             (unsigned)s.cap, (unsigned)s.trace_cap, s.persist ? "on" : "off");
    return ESP_OK;
}
