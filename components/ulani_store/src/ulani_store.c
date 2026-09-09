#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"

#include "diag_log.h"
#include "ulani_store.h"

static const char *TAG = "ulani_store";

#define MOUNT_POINT "/img"
#define PARTITION   "storage"

/* Where the event log ceiling is remembered. */
#define LOG_NVS_NAMESPACE "ulani_log"

/* Written next to the payload so the CRC survives a reboot. */
#define CRC_SUFFIX ".crc"

static bool s_mounted;

static void slot_path(uint8_t slot, char *out, size_t len)
{
    snprintf(out, len, MOUNT_POINT "/slot%u.bin", (unsigned)slot);
}

static void slot_tmp_path(uint8_t slot, char *out, size_t len)
{
    snprintf(out, len, MOUNT_POINT "/slot%u.tmp", (unsigned)slot);
}

static void slot_crc_path(uint8_t slot, char *out, size_t len)
{
    snprintf(out, len, MOUNT_POINT "/slot%u" CRC_SUFFIX, (unsigned)slot);
}

static bool slot_valid(uint8_t slot)
{
    return slot >= ULANI_SLOT_MIN && slot <= ULANI_SLOT_MAX;
}

esp_err_t ulani_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = PARTITION,
        /* Four pages, plus the event log appending and being exported. */
        .max_files              = 6,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    size_t total = 0, used = 0;
    if (esp_spiffs_info(PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted at " MOUNT_POINT ": %u of %u bytes used",
                 (unsigned)used, (unsigned)total);
    }

    /* An upload interrupted by a reset leaves these behind. */
    for (uint8_t slot = ULANI_SLOT_MIN; slot <= ULANI_SLOT_MAX; slot++) {
        char tmp[40];
        slot_tmp_path(slot, tmp, sizeof(tmp));
        if (unlink(tmp) == 0) {
            ESP_LOGW(TAG, "discarded a partial upload for slot %u", slot);
        }
    }
    return ESP_OK;
}

static uint16_t crc_load(uint8_t slot)
{
    char path[40];
    slot_crc_path(slot, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    uint16_t crc = 0;
    if (fread(&crc, sizeof(crc), 1, fp) != 1) {
        crc = 0;
    }
    fclose(fp);
    return crc;
}

static void crc_store(uint8_t slot, uint16_t crc)
{
    char path[40];
    slot_crc_path(slot, path, sizeof(path));

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return;
    }
    fwrite(&crc, sizeof(crc), 1, fp);
    fclose(fp);
}

void ulani_store_info(uint8_t slot, ulani_slot_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mounted || !slot_valid(slot)) {
        return;
    }

    char path[40];
    slot_path(slot, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }
    out->present = (st.st_size == (off_t)ULANI_PAYLOAD_BYTES);
    out->size    = (size_t)st.st_size;
    out->crc     = crc_load(slot);
}

/* -------------------------------------------------------------- uploading */

esp_err_t ulani_store_write_begin(uint8_t slot, ulani_store_writer_t *w)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(w, 0, sizeof(*w));

    char tmp[40];
    slot_tmp_path(slot, tmp, sizeof(tmp));

    w->fp = fopen(tmp, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "cannot open %s", tmp);
        return ESP_FAIL;
    }
    w->slot = slot;
    ulani_crc16_init(&w->crc);
    return ESP_OK;
}

esp_err_t ulani_store_write(ulani_store_writer_t *w, const void *data, size_t len)
{
    if (!w->fp) {
        return ESP_ERR_INVALID_STATE;
    }
    if (w->written + len > ULANI_PAYLOAD_BYTES) {
        ESP_LOGE(TAG, "upload longer than a payload (%u bytes)",
                 (unsigned)(w->written + len));
        return ESP_ERR_INVALID_SIZE;
    }
    if (fwrite(data, 1, len, w->fp) != len) {
        ESP_LOGE(TAG, "write failed at %u bytes", (unsigned)w->written);
        return ESP_FAIL;
    }
    ulani_crc16_update(&w->crc, data, len);
    w->written += len;
    return ESP_OK;
}

esp_err_t ulani_store_write_commit(ulani_store_writer_t *w)
{
    if (!w->fp) {
        return ESP_ERR_INVALID_STATE;
    }
    fclose(w->fp);
    w->fp = NULL;

    char tmp[40], path[40];
    slot_tmp_path(w->slot, tmp, sizeof(tmp));
    slot_path(w->slot, path, sizeof(path));

    if (w->written != ULANI_PAYLOAD_BYTES) {
        ESP_LOGE(TAG, "slot %u: got %u bytes, expected %u",
                 w->slot, (unsigned)w->written, (unsigned)ULANI_PAYLOAD_BYTES);
        unlink(tmp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* SPIFFS rename does not replace an existing name. */
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "cannot publish slot %u", w->slot);
        unlink(tmp);
        return ESP_FAIL;
    }

    uint16_t crc = ulani_crc16_final(&w->crc);
    crc_store(w->slot, crc);

    ESP_LOGI(TAG, "slot %u stored (%u bytes, crc=%04x)",
             w->slot, (unsigned)w->written, crc);
    return ESP_OK;
}

void ulani_store_write_abort(ulani_store_writer_t *w)
{
    if (w->fp) {
        fclose(w->fp);
        w->fp = NULL;
    }
    char tmp[40];
    slot_tmp_path(w->slot, tmp, sizeof(tmp));
    unlink(tmp);
    ESP_LOGW(TAG, "upload to slot %u abandoned", w->slot);
}

esp_err_t ulani_store_delete(uint8_t slot)
{
    if (!s_mounted || !slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[40];
    slot_path(slot, path, sizeof(path));
    unlink(path);

    slot_crc_path(slot, path, sizeof(path));
    unlink(path);

    ESP_LOGI(TAG, "slot %u cleared", slot);
    return ESP_OK;
}

/* -------------------------------------------------------------- streaming */

static esp_err_t read_at(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    FILE *fp = ((ulani_store_reader_t *)ctx)->fp;

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(out, 1, len, fp) != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ulani_store_payload_src(uint8_t slot, ulani_store_reader_t *r,
                                  ulani_payload_src_t *src)
{
    memset(r, 0, sizeof(*r));

    ulani_slot_info_t info;
    ulani_store_info(slot, &info);
    if (!info.present) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[40];
    slot_path(slot, path, sizeof(path));

    r->fp = fopen(path, "rb");
    if (!r->fp) {
        return ESP_FAIL;
    }

    src->read = read_at;
    src->ctx  = r;
    return ESP_OK;
}

void ulani_store_reader_close(ulani_store_reader_t *r)
{
    if (r->fp) {
        fclose(r->fp);
        r->fp = NULL;
    }
}

/* ------------------------------------------------------------- event log */

/*
 * Segment layout: a 16-byte header then a run of diag_rec_t. The header's
 * seg_seq is what orders the segments -- filenames get recycled, so the name
 * says nothing about age -- and it is why nothing has to be rewritten to
 * rotate: the oldest file is truncated and given a new seq.
 */
#define LOG_MAGIC 0x474f4c55u /* "ULOG" */

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint32_t seg_seq; /* higher is newer */
    uint32_t reserved2;
} log_hdr_t;

_Static_assert(sizeof(log_hdr_t) == 16, "log segment header is a stored format");

static struct {
    bool     scanned;
    uint8_t  segments;  /* how many to keep */
    int8_t   cur;       /* segment being appended to, -1 = none yet */
    uint32_t cur_seq;
    size_t   cur_bytes; /* records in the current segment, header excluded */
    bool     paused;    /* stopped for want of space; only says so once */
} lg = { .cur = -1, .segments = 4 };

static void log_path(uint8_t idx, char *out, size_t len)
{
    snprintf(out, len, MOUNT_POINT "/log%u.bin", (unsigned)idx);
}

static void log_segments_load(void)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, "segs", &n) == ESP_OK &&
        n >= ULANI_LOG_SEGMENTS_MIN && n <= ULANI_LOG_SEGMENTS_MAX) {
        lg.segments = n;
    }
    nvs_close(h);
}

/*
 * Reads one segment's header. False when the file is absent or is not a
 * segment this build understands, in which case the index is free to reuse.
 */
static bool log_seg_read(uint8_t idx, log_hdr_t *hdr, size_t *bytes)
{
    char path[40];
    log_path(idx, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size < sizeof(*hdr)) {
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    bool ok = fread(hdr, sizeof(*hdr), 1, fp) == 1;
    fclose(fp);
    if (!ok || hdr->magic != LOG_MAGIC || hdr->schema != DIAG_SCHEMA_VERSION) {
        return false;
    }
    if (bytes) {
        *bytes = (size_t)st.st_size - sizeof(*hdr);
    }
    return true;
}

/* Which segments exist, oldest first. Returns how many were found. */
static uint8_t log_scan(uint8_t *order, size_t *bytes_out)
{
    uint8_t  n = 0;
    uint8_t  idx[ULANI_LOG_SEGMENTS_MAX];
    uint32_t seq[ULANI_LOG_SEGMENTS_MAX];
    size_t   len[ULANI_LOG_SEGMENTS_MAX];

    for (uint8_t i = 0; i < ULANI_LOG_SEGMENTS_MAX; i++) {
        log_hdr_t hdr;
        size_t    bytes = 0;
        if (!log_seg_read(i, &hdr, &bytes)) {
            continue;
        }
        idx[n] = i;
        seq[n] = hdr.seg_seq;
        len[n] = bytes;
        n++;
    }

    /* Insertion sort by seq: there are at most eight. */
    for (uint8_t i = 1; i < n; i++) {
        uint8_t  ti = idx[i];
        uint32_t ts = seq[i];
        size_t   tl = len[i];
        int8_t   j  = (int8_t)i - 1;
        while (j >= 0 && seq[j] > ts) {
            idx[j + 1] = idx[j];
            seq[j + 1] = seq[j];
            len[j + 1] = len[j];
            j--;
        }
        idx[j + 1] = ti;
        seq[j + 1] = ts;
        len[j + 1] = tl;
    }

    for (uint8_t i = 0; i < n; i++) {
        order[i] = idx[i];
        if (bytes_out) {
            bytes_out[i] = len[i];
        }
    }
    return n;
}

size_t ulani_store_free_bytes(void)
{
    size_t total = 0, used = 0;
    if (!s_mounted || esp_spiffs_info(PARTITION, &total, &used) != ESP_OK) {
        return 0;
    }
    return (total > used) ? total - used : 0;
}

/*
 * Starts a fresh newest segment. Prefers an index not in use yet, then
 * recycles the oldest -- which is also what frees the space the new one needs,
 * so a partition with no room left resolves itself here rather than by
 * crowding out a page.
 */
static esp_err_t log_rotate(void)
{
    uint8_t order[ULANI_LOG_SEGMENTS_MAX];
    size_t  bytes[ULANI_LOG_SEGMENTS_MAX];
    uint8_t n = log_scan(order, bytes);

    uint32_t newest = 0;
    for (uint8_t i = 0; i < n; i++) {
        log_hdr_t hdr;
        if (log_seg_read(order[i], &hdr, NULL) && hdr.seg_seq > newest) {
            newest = hdr.seg_seq;
        }
    }

    int8_t victim = -1;
    if (n < lg.segments) {
        for (uint8_t i = 0; i < lg.segments && victim < 0; i++) {
            bool taken = false;
            for (uint8_t j = 0; j < n; j++) {
                taken = taken || (order[j] == i);
            }
            if (!taken) {
                victim = (int8_t)i;
            }
        }
        /* Growing needs free space; recycling does not, so fall through. */
        if (victim >= 0 && ulani_store_free_bytes() < ULANI_LOG_RESERVE_BYTES) {
            victim = -1;
        }
    }
    if (victim < 0 && n > 0) {
        victim = (int8_t)order[0]; /* the oldest */
        diag_log_note_dropped((uint32_t)(bytes[0] / sizeof(diag_rec_t)));
    }
    if (victim < 0) {
        return ESP_ERR_NO_MEM;
    }

    char path[40];
    log_path((uint8_t)victim, path, sizeof(path));
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return ESP_FAIL;
    }
    log_hdr_t hdr = {
        .magic   = LOG_MAGIC,
        .schema  = DIAG_SCHEMA_VERSION,
        .seg_seq = newest + 1,
    };
    bool ok = fwrite(&hdr, sizeof(hdr), 1, fp) == 1;
    fclose(fp);
    if (!ok) {
        return ESP_FAIL;
    }

    lg.cur       = victim;
    lg.cur_seq   = hdr.seg_seq;
    lg.cur_bytes = 0;
    return ESP_OK;
}

/* Picks up where the last boot left off. */
static void log_scan_once(void)
{
    if (lg.scanned) {
        return;
    }
    lg.scanned = true;
    log_segments_load();

    uint8_t order[ULANI_LOG_SEGMENTS_MAX];
    size_t  bytes[ULANI_LOG_SEGMENTS_MAX];
    uint8_t n = log_scan(order, bytes);
    if (n == 0) {
        return;
    }

    log_hdr_t hdr;
    if (log_seg_read(order[n - 1], &hdr, NULL)) {
        lg.cur       = (int8_t)order[n - 1];
        lg.cur_seq   = hdr.seg_seq;
        lg.cur_bytes = bytes[n - 1];
    }

    /* The ceiling may have been lowered since these were written. */
    for (uint8_t i = 0; n - i > lg.segments; i++) {
        char path[40];
        log_path(order[i], path, sizeof(path));
        unlink(path);
    }
}

esp_err_t ulani_store_log_append(const void *data, size_t len)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_OK;
    }
    log_scan_once();

    if (lg.cur < 0 || lg.cur_bytes + len > ULANI_LOG_SEGMENT_BYTES) {
        esp_err_t err = log_rotate();
        if (err != ESP_OK) {
            if (!lg.paused) {
                lg.paused = true;
                diag_log_note_flash_full((uint32_t)ulani_store_free_bytes());
                ESP_LOGW(TAG, "event log paused: %u bytes free in " PARTITION,
                         (unsigned)ulani_store_free_bytes());
            }
            return err;
        }
    }

    char path[40];
    log_path((uint8_t)lg.cur, path, sizeof(path));
    FILE *fp = fopen(path, "ab");
    if (!fp) {
        return ESP_FAIL;
    }
    size_t wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    if (wrote != len) {
        /*
         * A short write leaves half a record at the end of the segment, and
         * everything after it would be read at the wrong offset -- one failed
         * write would cost the whole segment. Cut back to a record boundary,
         * and if even that fails, believe the file rather than the counter so
         * the next append at least lands after the damage.
         */
        log_hdr_t hdr;
        size_t    bytes = 0;
        if (log_seg_read((uint8_t)lg.cur, &hdr, &bytes)) {
            size_t whole = bytes - (bytes % sizeof(diag_rec_t));
            if (whole != bytes &&
                truncate(path, (off_t)(sizeof(log_hdr_t) + whole)) == 0) {
                bytes = whole;
            }
            lg.cur_bytes = bytes;
        }
        return ESP_ERR_NO_MEM;
    }
    lg.cur_bytes += len;
    lg.paused = false;
    return ESP_OK;
}

size_t ulani_store_log_size(void)
{
    if (!s_mounted) {
        return 0;
    }
    log_scan_once();

    uint8_t order[ULANI_LOG_SEGMENTS_MAX];
    size_t  bytes[ULANI_LOG_SEGMENTS_MAX];
    uint8_t n = log_scan(order, bytes);

    size_t total = 0;
    for (uint8_t i = 0; i < n; i++) {
        total += bytes[i];
    }
    return total;
}

size_t ulani_store_log_read(size_t offset, void *out, size_t len)
{
    if (!s_mounted || !out || len == 0) {
        return 0;
    }
    log_scan_once();

    uint8_t order[ULANI_LOG_SEGMENTS_MAX];
    size_t  bytes[ULANI_LOG_SEGMENTS_MAX];
    uint8_t n = log_scan(order, bytes);

    size_t   done = 0;
    uint8_t *dst  = out;

    for (uint8_t i = 0; i < n && done < len; i++) {
        if (offset >= bytes[i]) {
            offset -= bytes[i]; /* this segment is wholly before the window */
            continue;
        }
        char path[40];
        log_path(order[i], path, sizeof(path));
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            break;
        }
        if (fseek(fp, (long)(sizeof(log_hdr_t) + offset), SEEK_SET) == 0) {
            size_t want = bytes[i] - offset;
            if (want > len - done) {
                want = len - done;
            }
            done += fread(dst + done, 1, want, fp);
        }
        fclose(fp);
        offset = 0;
    }
    return done;
}

esp_err_t ulani_store_log_clear(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t i = 0; i < ULANI_LOG_SEGMENTS_MAX; i++) {
        char path[40];
        log_path(i, path, sizeof(path));
        unlink(path);
    }
    lg.cur       = -1;
    lg.cur_bytes = 0;
    lg.paused    = false;
    return ESP_OK;
}

void ulani_store_log_set_segments(uint8_t segments)
{
    if (segments < ULANI_LOG_SEGMENTS_MIN) {
        segments = ULANI_LOG_SEGMENTS_MIN;
    }
    if (segments > ULANI_LOG_SEGMENTS_MAX) {
        segments = ULANI_LOG_SEGMENTS_MAX;
    }
    log_scan_once();
    lg.segments = segments;

    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "segs", segments);
        nvs_commit(h);
        nvs_close(h);
    }

    /* Lowering the ceiling takes effect now, from the oldest end. */
    uint8_t order[ULANI_LOG_SEGMENTS_MAX];
    size_t  bytes[ULANI_LOG_SEGMENTS_MAX];
    uint8_t n = log_scan(order, bytes);
    for (uint8_t i = 0; n - i > segments; i++) {
        char path[40];
        log_path(order[i], path, sizeof(path));
        unlink(path);
        diag_log_note_dropped((uint32_t)(bytes[i] / sizeof(diag_rec_t)));
        if (order[i] == (uint8_t)lg.cur) {
            lg.cur = -1;
        }
    }
}

uint8_t ulani_store_log_segments(void)
{
    log_scan_once();
    return lg.segments;
}
