#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#include "ulani_store.h"

static const char *TAG = "ulani_store";

#define MOUNT_POINT "/img"
#define PARTITION   "storage"

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
        .max_files              = 4,
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
