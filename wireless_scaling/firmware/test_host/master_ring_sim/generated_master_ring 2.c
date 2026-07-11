/* GENERATED: do not edit; exact function bodies extracted from master.c. */
#include "ring_host.h"

static const char *TAG = "MASTER_RING_SIM";
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t unit_id;
    uint8_t valid_mask;
    int8_t rssi;
    uint8_t src_mac[6];
    uint32_t seq;
    uint32_t boot_id;
    uint32_t rx_ms;
    uint32_t ts;
    uint32_t vbat_mv;
    uint32_t gap;
    uint32_t rain_tips_total;
    uint32_t rain_tips_delta;
    int32_t weight_raw;
    int32_t hx711_raw;
    float temp_c;
    float ds0_hive_c;
    float ds1_scale_c;
    float aht20_t_c;
    float aht20_rh;
    float bmp280_t_c;
    float bmp280_pa;
    float rain_mm_total;
    float rain_mm_delta;
} master_record_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t unit_id;
    uint8_t valid_mask;
    int8_t rssi;
    uint8_t src_mac[6];
    uint32_t seq;
    uint32_t boot_id;
    uint32_t rx_ms;
    uint32_t ts;
    uint32_t vbat_mv;
    uint32_t gap;
    uint32_t rain_tips_total;
    uint32_t rain_tips_delta;
    int32_t weight_raw;
    int32_t hx711_raw;
    float temp_c;
    float ds0_hive_c;
    float ds1_scale_c;
    float aht20_t_c;
    float aht20_rh;
    float bmp280_t_c;
    float bmp280_pa;
    float rain_mm_total;
} master_record_v1_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_seq;
    uint32_t crc32;
    uint32_t format_version;
    master_record_t rec;
    uint8_t pad[BACKUP_SLOT_SIZE - 16 - sizeof(master_record_t)];
} backup_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_seq;
    uint32_t crc32;
    uint32_t format_version;
    uint8_t payload[BACKUP_SLOT_SIZE - 16];
} backup_raw_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t head;
    uint32_t tail;
    uint32_t write_seq;
    uint32_t master_seq;
    uint32_t rain_reported;
    uint32_t crc32;
} backup_index_t;

typedef struct __attribute__((packed)) {
    uint32_t head;
    uint32_t tail;
    uint32_t write_seq;
    uint32_t crc32;
} backup_index_legacy_t;

static SemaphoreHandle_t s_rain_mutex = (void *)1;
static SemaphoreHandle_t s_backup_mutex = (void *)1;
static nvs_handle_t s_rain_nvs = 2;
static nvs_handle_t s_backup_nvs = 3;
static uint32_t s_master_seq;
static uint32_t s_rain_tip_count;
static uint32_t s_rain_last_reported;
static const esp_partition_t *s_backup_part;
static uint32_t s_backup_slot_count;
static uint32_t s_backup_head;
static uint32_t s_backup_tail;
static uint32_t s_backup_write_seq;
static volatile uint32_t s_backup_enqueued;
static volatile uint32_t s_backup_uploaded;
static volatile uint32_t s_backup_bad;
static volatile uint32_t s_backup_full;

static uint32_t __attribute__((unused)) crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool rain_store_count(void)
{
    esp_err_t err = nvs_set_u32(s_rain_nvs, "tips", s_rain_tip_count);
    if (err == ESP_OK) err = nvs_commit(s_rain_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rain tip persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void rain_apply_durable_cursor(uint32_t reported)
{
    xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
    if (reported > s_rain_tip_count) {
        s_rain_tip_count = reported;
        (void)rain_store_count();
    }
    s_rain_last_reported = reported;
    xSemaphoreGive(s_rain_mutex);
}

static uint32_t __attribute__((unused)) rain_get_durable_cursor(void)
{
    xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
    uint32_t reported = s_rain_last_reported;
    xSemaphoreGive(s_rain_mutex);
    return reported;
}

static bool backup_store_index_values(uint32_t head, uint32_t tail, uint32_t write_seq,
                                      uint32_t master_seq, uint32_t rain_reported)
{
    backup_index_t index = {
        .magic = BACKUP_INDEX_MAGIC,
        .version = BACKUP_INDEX_VERSION,
        .head = head,
        .tail = tail,
        .write_seq = write_seq,
        .master_seq = master_seq,
        .rain_reported = rain_reported,
        .crc32 = 0,
    };
    index.crc32 = crc32_update(0, (const uint8_t *)&index,
                               sizeof(index) - sizeof(index.crc32));
    esp_err_t err = nvs_set_blob(s_backup_nvs, "state2", &index, sizeof(index));
    if (err == ESP_OK) err = nvs_commit(s_backup_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backup index persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static uint32_t backup_count_unlocked(void)
{
    if (s_backup_head >= s_backup_tail) {
        return s_backup_head - s_backup_tail;
    }
    return s_backup_slot_count - s_backup_tail + s_backup_head;
}

static uint32_t master_next_seq(void)
{
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    uint32_t seq = s_master_seq + 1;
    xSemaphoreGive(s_backup_mutex);
    return seq;
}

static void backup_init(void)
{
    s_backup_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "backup");
    if (s_backup_part == NULL) {
        ESP_LOGW(TAG, "backup partition not found; flash backlog disabled");
        return;
    }
    s_backup_slot_count = s_backup_part->size / BACKUP_SLOT_SIZE;

    backup_index_t index = {0};
    size_t index_len = sizeof(index);
    esp_err_t err = nvs_get_blob(s_backup_nvs, "state2", &index, &index_len);
    bool valid_index = err == ESP_OK && index_len == sizeof(index) &&
                       index.magic == BACKUP_INDEX_MAGIC &&
                       index.version == BACKUP_INDEX_VERSION &&
                       index.crc32 == crc32_update(0, (const uint8_t *)&index,
                                                  sizeof(index) - sizeof(index.crc32));
    bool migrate_index = false;
    if (valid_index) {
        s_backup_head = index.head;
        s_backup_tail = index.tail;
        s_backup_write_seq = index.write_seq;
        s_master_seq = index.master_seq;
        s_rain_last_reported = index.rain_reported;
    } else if (err == ESP_OK && index_len == sizeof(backup_index_legacy_t)) {
        const backup_index_legacy_t *legacy = (const backup_index_legacy_t *)&index;
        valid_index = legacy->crc32 ==
                      crc32_update(0, (const uint8_t *)legacy,
                                   sizeof(*legacy) - sizeof(legacy->crc32));
        if (valid_index) {
            s_backup_head = legacy->head;
            s_backup_tail = legacy->tail;
            s_backup_write_seq = legacy->write_seq;
            migrate_index = true;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_err_t head_err = nvs_get_u32(s_backup_nvs, "head", &s_backup_head);
        esp_err_t tail_err = nvs_get_u32(s_backup_nvs, "tail", &s_backup_tail);
        esp_err_t seq_err = nvs_get_u32(s_backup_nvs, "wseq", &s_backup_write_seq);
        if (head_err == ESP_ERR_NVS_NOT_FOUND) s_backup_head = 0;
        else if (head_err != ESP_OK) valid_index = false;
        if (tail_err == ESP_ERR_NVS_NOT_FOUND) s_backup_tail = 0;
        else if (tail_err != ESP_OK) valid_index = false;
        if (seq_err == ESP_ERR_NVS_NOT_FOUND) s_backup_write_seq = 0;
        else if (seq_err != ESP_OK) valid_index = false;
        valid_index = head_err == ESP_OK || head_err == ESP_ERR_NVS_NOT_FOUND;
        valid_index = valid_index && (tail_err == ESP_OK || tail_err == ESP_ERR_NVS_NOT_FOUND);
        valid_index = valid_index && (seq_err == ESP_OK || seq_err == ESP_ERR_NVS_NOT_FOUND);
        migrate_index = valid_index;
    }
    if (!valid_index || s_backup_head >= s_backup_slot_count ||
        s_backup_tail >= s_backup_slot_count) {
        ESP_LOGE(TAG, "backup index invalid; durable backlog disabled");
        s_backup_part = NULL;
        s_backup_slot_count = 0;
        s_backup_head = 0;
        s_backup_tail = 0;
        return;
    }
    if (migrate_index &&
        !backup_store_index_values(s_backup_head, s_backup_tail, s_backup_write_seq,
                                   s_master_seq, s_rain_last_reported)) {
        ESP_LOGE(TAG, "backup index migration failed; durable backlog disabled");
        s_backup_part = NULL;
        s_backup_slot_count = 0;
        return;
    }
    rain_apply_durable_cursor(s_rain_last_reported);
    ESP_LOGI(TAG, "backup partition size=%u slots=%u pending=%u",
             (unsigned)s_backup_part->size, (unsigned)s_backup_slot_count,
             (unsigned)backup_count_unlocked());
}

static bool backup_slot_decode(const backup_raw_slot_t *slot, master_record_t *rec,
                               bool log_migration)
{
    if (slot->magic == BACKUP_MAGIC_V2 &&
        slot->format_version == BACKUP_FORMAT_VERSION &&
        slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_t))) {
        memcpy(rec, slot->payload, sizeof(*rec));
        return true;
    }
    if (slot->magic != BACKUP_MAGIC_V1) return false;
    if (slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_t))) {
        memcpy(rec, slot->payload, sizeof(*rec));
        return true;
    }
    if (slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_v1_t))) {
        memset(rec, 0, sizeof(*rec));
        memcpy(rec, slot->payload, sizeof(master_record_v1_t));
        rec->rain_mm_delta = (float)rec->rain_tips_delta * RAIN_MM_PER_TIP;
        if (log_migration) {
            ESP_LOGI(TAG, "backup V1 record migrated write_seq=%u",
                     (unsigned)slot->write_seq);
        }
        return true;
    }
    return false;
}

static bool backup_slot_erased(const backup_raw_slot_t *slot)
{
    const uint8_t *bytes = (const uint8_t *)slot;
    for (size_t i = 0; i < sizeof(*slot); i++) {
        if (bytes[i] != 0xFF) return false;
    }
    return true;
}

static bool backup_prepare_head_unlocked(const master_record_t *incoming,
                                         bool *incoming_already_durable)
{
    *incoming_already_durable = false;
    const uint32_t slots_per_sector = BACKUP_SECTOR_SIZE / BACKUP_SLOT_SIZE;
    for (;;) {
        uint32_t next = (s_backup_head + 1) % s_backup_slot_count;
        if (next == s_backup_tail) {
            s_backup_full++;
            ESP_LOGE(TAG, "backup ring full; refusing type=%u unit=%u seq=%u",
                     incoming->type, incoming->unit_id, (unsigned)incoming->seq);
            return false;
        }

        backup_raw_slot_t existing;
        esp_err_t err = esp_partition_read(s_backup_part,
                                           s_backup_head * BACKUP_SLOT_SIZE,
                                           &existing, sizeof(existing));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "backup head read failed: %s", esp_err_to_name(err));
            return false;
        }
        if (backup_slot_erased(&existing)) {
            if ((s_backup_head % slots_per_sector) == 0) {
                uint32_t sector = s_backup_head / slots_per_sector;
                uint32_t tail_sector = s_backup_tail / slots_per_sector;
                if (s_backup_tail != s_backup_head && tail_sector == sector) {
                    s_backup_full++;
                    ESP_LOGE(TAG, "backup erase boundary reached unread sector; refusing type=%u unit=%u seq=%u",
                             incoming->type, incoming->unit_id, (unsigned)incoming->seq);
                    return false;
                }
                err = esp_partition_erase_range(s_backup_part,
                                                sector * BACKUP_SECTOR_SIZE,
                                                BACKUP_SECTOR_SIZE);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "backup erase failed: %s", esp_err_to_name(err));
                    return false;
                }
            }
            return true;
        }

        master_record_t recovered;
        bool valid = backup_slot_decode(&existing, &recovered, false);
        bool recovered_expected = valid && existing.write_seq == s_backup_write_seq + 1;
        if (!recovered_expected) {
            uint32_t sector = s_backup_head / slots_per_sector;
            if (s_backup_tail != s_backup_head) {
                /* Szektorhatáron az előző ring-kör konzumált slotjai maradnak a headnél.
                 * A szektor minden slotja a headtől előre esik, a pending [tail, head)
                 * tartományon kívül, ezért ha a tail nincs ebben a szektorban, a törlés
                 * nem érinthet olvasatlan rekordot. Az orphan-adopt ág fentebb már
                 * lefutott, így indexeletlen, de teljes slot ide nem juthat. */
                uint32_t tail_sector = s_backup_tail / slots_per_sector;
                bool head_on_boundary = (s_backup_head % slots_per_sector) == 0;
                if (!head_on_boundary || tail_sector == sector) {
                    s_backup_full++;
                    ESP_LOGE(TAG, "occupied unexpected backup head=%u while queue pending; refusing write",
                             (unsigned)s_backup_head);
                    return false;
                }
                err = esp_partition_erase_range(s_backup_part,
                                                sector * BACKUP_SECTOR_SIZE,
                                                BACKUP_SECTOR_SIZE);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "backup stale-sector erase failed: %s",
                             esp_err_to_name(err));
                    return false;
                }
                ESP_LOGI(TAG, "backup erased stale sector=%u at head=%u (previous ring lap)",
                         (unsigned)sector, (unsigned)s_backup_head);
                return true;
            }
            err = esp_partition_erase_range(s_backup_part,
                                            sector * BACKUP_SECTOR_SIZE,
                                            BACKUP_SECTOR_SIZE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "backup damaged-sector erase failed: %s",
                         esp_err_to_name(err));
                return false;
            }
            ESP_LOGW(TAG, "erased unexpected/partial backup head=%u after queue drained",
                     (unsigned)s_backup_head);
            return true;
        }

        uint32_t recovered_write_seq = existing.write_seq;
        uint32_t recovered_master_seq = s_master_seq;
        uint32_t recovered_rain_reported = rain_get_durable_cursor();
        ESP_LOGW(TAG, "recovered unindexed backup slot=%u write_seq=%u",
                 (unsigned)s_backup_head, (unsigned)existing.write_seq);
        if (recovered.type == REC_TYPE_MASTER) {
            if (recovered.seq > recovered_master_seq) {
                recovered_master_seq = recovered.seq;
            }
            if (recovered.rain_tips_total > recovered_rain_reported) {
                recovered_rain_reported = recovered.rain_tips_total;
            }
        }

        uint32_t recovered_head = next;
        bool stored = backup_store_index_values(recovered_head, s_backup_tail,
                                                recovered_write_seq,
                                                recovered_master_seq,
                                                recovered_rain_reported);
        if (!stored) return false;
        s_backup_head = recovered_head;
        s_backup_write_seq = recovered_write_seq;
        s_master_seq = recovered_master_seq;
        rain_apply_durable_cursor(recovered_rain_reported);
        s_backup_enqueued++;
        if (incoming->type == REC_TYPE_NODE &&
            recovered.type == incoming->type &&
            recovered.unit_id == incoming->unit_id && recovered.seq == incoming->seq &&
            recovered.boot_id == incoming->boot_id) {
            *incoming_already_durable = true;
            return true;
        }
    }
}

static bool backup_enqueue(master_record_t *rec)
{
    if (s_backup_part == NULL || s_backup_slot_count == 0) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    bool already_durable = false;
    if (!backup_prepare_head_unlocked(rec, &already_durable)) {
        xSemaphoreGive(s_backup_mutex);
        return false;
    }
    if (already_durable) {
        xSemaphoreGive(s_backup_mutex);
        return true;
    }

    if (rec->type == REC_TYPE_MASTER) {
        uint32_t rain_reported = rain_get_durable_cursor();
        if (rec->rain_tips_total < rain_reported) {
            rec->rain_tips_total = rain_reported;
            rec->rain_mm_total = (float)rain_reported * RAIN_MM_PER_TIP;
        }
        rec->seq = s_master_seq + 1;
        rec->rain_tips_delta = rec->rain_tips_total - rain_reported;
        rec->rain_mm_delta = (float)rec->rain_tips_delta * RAIN_MM_PER_TIP;
    }

    uint32_t next = (s_backup_head + 1) % s_backup_slot_count;
    backup_slot_t slot;
    memset(&slot, 0xFF, sizeof(slot));
    uint32_t next_write_seq = s_backup_write_seq + 1;
    slot.magic = BACKUP_MAGIC_V2;
    slot.write_seq = next_write_seq;
    slot.format_version = BACKUP_FORMAT_VERSION;
    memcpy(&slot.rec, rec, sizeof(slot.rec));
    slot.crc32 = crc32_update(0, (const uint8_t *)&slot.rec, sizeof(slot.rec));

    esp_err_t err = esp_partition_write(s_backup_part, s_backup_head * BACKUP_SLOT_SIZE,
                                        &slot, sizeof(slot));
    if (err == ESP_OK) {
        backup_slot_t verify;
        err = esp_partition_read(s_backup_part, s_backup_head * BACKUP_SLOT_SIZE,
                                 &verify, sizeof(verify));
        if (err == ESP_OK && memcmp(&verify, &slot, sizeof(slot)) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        uint32_t next_master_seq = s_master_seq;
        uint32_t next_rain_reported = rain_get_durable_cursor();
        if (rec->type == REC_TYPE_MASTER) {
            next_master_seq = rec->seq;
            next_rain_reported = rec->rain_tips_total;
        }
        if (backup_store_index_values(next, s_backup_tail, next_write_seq,
                                      next_master_seq, next_rain_reported)) {
            s_backup_head = next;
            s_backup_write_seq = next_write_seq;
            s_master_seq = next_master_seq;
            rain_apply_durable_cursor(next_rain_reported);
            s_backup_enqueued++;
        } else {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "backup write/readback failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_backup_mutex);
    return err == ESP_OK;
}

static bool backup_peek(master_record_t *rec)
{
    if (s_backup_part == NULL || s_backup_head == s_backup_tail) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);

    backup_raw_slot_t slot;
    esp_err_t err = esp_partition_read(s_backup_part, s_backup_tail * BACKUP_SLOT_SIZE,
                                       &slot, sizeof(slot));
    if (err != ESP_OK) {
        /* Átmeneti olvasási hiba: a tail marad, a cloud task később újrapróbálja. */
        ESP_LOGW(TAG, "backup tail read failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_backup_mutex);
        return false;
    }
    bool valid = backup_slot_decode(&slot, rec, true);
    if (!valid) {
        s_backup_bad++;
        uint32_t next_tail = (s_backup_tail + 1) % s_backup_slot_count;
        uint32_t rain_reported = rain_get_durable_cursor();
        if (backup_store_index_values(s_backup_head, next_tail, s_backup_write_seq,
                                      s_master_seq, rain_reported)) {
            s_backup_tail = next_tail;
        }
        xSemaphoreGive(s_backup_mutex);
        return false;
    }

    xSemaphoreGive(s_backup_mutex);
    return true;
}

static bool backup_pop(void)
{
    if (s_backup_part == NULL || s_backup_head == s_backup_tail) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    uint32_t next_tail = (s_backup_tail + 1) % s_backup_slot_count;
    uint32_t rain_reported = rain_get_durable_cursor();
    bool stored = backup_store_index_values(s_backup_head, next_tail, s_backup_write_seq,
                                            s_master_seq, rain_reported);
    if (stored) {
        s_backup_tail = next_tail;
        s_backup_uploaded++;
    }
    xSemaphoreGive(s_backup_mutex);
    return stored;
}

void prod_reset_and_init(void)
{
    s_master_seq = 0; s_rain_tip_count = 0; s_rain_last_reported = 0;
    s_backup_part = NULL; s_backup_slot_count = 0; s_backup_head = 0;
    s_backup_tail = 0; s_backup_write_seq = 0;
    s_backup_enqueued = s_backup_uploaded = s_backup_bad = s_backup_full = 0;
    backup_init();
}
bool prod_enqueue(host_master_record_t *r) { return backup_enqueue((master_record_t *)r); }
bool prod_peek(host_master_record_t *r) { return backup_peek((master_record_t *)r); }
bool prod_pop(void) { return backup_pop(); }
uint32_t prod_count(void) { return backup_count_unlocked(); }
uint32_t prod_head(void) { return s_backup_head; }
uint32_t prod_tail(void) { return s_backup_tail; }
uint32_t prod_write_seq(void) { return s_backup_write_seq; }
uint32_t prod_slot_count(void) { return s_backup_slot_count; }
uint32_t prod_master_seq(void) { return master_next_seq() - 1U; }
bool prod_enabled(void) { return s_backup_part != NULL; }
void prod_set_rain(uint32_t tips, uint32_t reported)
{ s_rain_tip_count = tips; s_rain_last_reported = reported; }
