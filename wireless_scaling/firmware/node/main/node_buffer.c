/*
 * node_buffer.c — flash-backed pending queue NVS partíción.
 *
 * Tartós invariáns:
 *   state2 = {magic, version, size, head, next, count, crc}, és next - head == count.
 *   Az aktív rekordkulcsok logikai tartománya [head, next), legfeljebb 512 elem.
 *
 * Power-loss sorrend:
 *   enqueue: record set+commit -> state2 set+commit -> RAM state léptetés. A két commit közti
 *            reset legfeljebb egy, pontosan a régi next kulcson levő orphan rekordot hagy;
 *            init ezt CRC/seq ellenőrzés után adoptálja, ezért ugyanaz a seq nem íródik felül.
 *   clear:   csak durable master ACK után record erase+commit -> state2 set+commit -> RAM state.
 *            A két commit közti reset régi state-et és hiányzó frontot hagy; a front repair ezt
 *            újra felismeri és lépteti. Erase-first miatt unreachable rekordblob nem szivárog.
 *
 * A RAM head/next/count kizárólag sikeres state2 commit után változhat.
 */
#include "node_buffer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "node_config.h"

#define NODE_BUFFER_PARTITION       "nodebuf"
#define NODE_BUFFER_NAMESPACE       "queue"
#define NODE_BUFFER_STATE_KEY       "state2"
#define NODE_BUFFER_STATE_MAGIC     0x3246424EU /* "NBF2" little-endian. */
#define NODE_BUFFER_STATE_VERSION   2U
#define NODE_RECORD_KEY_TEXT_LEN    9U          /* r + 8 hex digit. */

_Static_assert(NVS_KEY_NAME_MAX_SIZE >= NODE_RECORD_KEY_TEXT_LEN + 1U,
               "NVS record key buffer too small");
_Static_assert(sizeof(NODE_BUFFER_STATE_KEY) <= NVS_KEY_NAME_MAX_SIZE,
               "state2 NVS key too long");
_Static_assert(NODE_PENDING_CAPACITY > 0 && NODE_PENDING_CAPACITY <= INT32_MAX,
               "queue capacity must fit wrap-safe sequence comparisons");

typedef struct {
    uint32_t head;
    uint32_t next;
    uint32_t count;
} queue_state_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t head;
    uint32_t next;
    uint32_t count;
    uint16_t crc;
} queue_state_blob_t;

_Static_assert(sizeof(queue_state_blob_t) == 22, "state2 blob layout changed");

typedef enum {
    RECORD_VALID,
    RECORD_MISSING,
    RECORD_CORRUPT,
    RECORD_IO_ERROR,
} record_status_t;

typedef enum {
    STATE2_VALID,
    STATE2_NOT_FOUND,
    STATE2_CORRUPT,
    STATE2_IO_ERROR,
} state2_status_t;

typedef struct {
    bool found;
    uint32_t value;
} legacy_u32_t;

static const char *TAG = "node_buffer";

static nvs_handle_t s_nvs;
static bool s_ready = false;
static uint32_t s_head = 0;
static uint32_t s_next = 0;
static uint32_t s_count = 0;

static void record_key(uint32_t seq, char key[NVS_KEY_NAME_MAX_SIZE])
{
    int written = snprintf(key, NVS_KEY_NAME_MAX_SIZE, "r%08" PRIX32, seq);
    (void)written; /* A format hosszat a fenti NVS limit static assert garantalja. */
}

static bool parse_record_key(const char *key, uint32_t *seq_out)
{
    if (key == NULL || strlen(key) != NODE_RECORD_KEY_TEXT_LEN || key[0] != 'r') {
        return false;
    }

    uint32_t seq = 0;
    for (size_t i = 1; i <= 8; i++) {
        char c = key[i];
        uint8_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint8_t)(c - 'a' + 10);
        } else {
            return false;
        }
        seq = (seq << 4) | nibble;
    }
    *seq_out = seq;
    return true;
}

static bool seq_leq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static queue_state_t current_state(void)
{
    return (queue_state_t) {
        .head = s_head,
        .next = s_next,
        .count = s_count,
    };
}

static void apply_state(const queue_state_t *state)
{
    s_head = state->head;
    s_next = state->next;
    s_count = state->count;
}

static bool state_layout_valid(const queue_state_t *state)
{
    return state->count <= NODE_PENDING_CAPACITY &&
           (uint32_t)(state->next - state->head) == state->count;
}

static uint16_t state_blob_crc(const queue_state_blob_t *blob)
{
    return sz2_crc16((const uint8_t *)blob, offsetof(queue_state_blob_t, crc));
}

static esp_err_t persist_state(const queue_state_t *state, const char *reason)
{
    if (!state_layout_valid(state)) {
        ESP_LOGE(TAG, "STATE2 reject reason=%s head=%" PRIu32 " next=%" PRIu32
                      " count=%" PRIu32,
                 reason, state->head, state->next, state->count);
        return ESP_ERR_INVALID_STATE;
    }

    queue_state_blob_t blob = {
        .magic = NODE_BUFFER_STATE_MAGIC,
        .version = NODE_BUFFER_STATE_VERSION,
        .size = sizeof(queue_state_blob_t),
        .head = state->head,
        .next = state->next,
        .count = state->count,
        .crc = 0,
    };
    blob.crc = state_blob_crc(&blob);

    esp_err_t err = nvs_set_blob(s_nvs, NODE_BUFFER_STATE_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STATE2 commit fail reason=%s head=%" PRIu32 " next=%" PRIu32
                      " count=%" PRIu32 " err=%s",
                 reason, state->head, state->next, state->count, esp_err_to_name(err));
    }
    return err;
}

static state2_status_t load_state2(queue_state_t *state_out, esp_err_t *detail_out)
{
    queue_state_blob_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(s_nvs, NODE_BUFFER_STATE_KEY, &blob, &len);
    if (detail_out != NULL) {
        *detail_out = err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return STATE2_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        ESP_LOGE(TAG, "STATE2 corrupt read err=%s stored_len=%u expected=%u; queue preserved",
                 esp_err_to_name(err), (unsigned)len, (unsigned)sizeof(blob));
        return STATE2_CORRUPT;
    }
    if (err != ESP_OK) {
        return STATE2_IO_ERROR;
    }

    queue_state_t state = {
        .head = blob.head,
        .next = blob.next,
        .count = blob.count,
    };
    bool valid = len == sizeof(blob) &&
                 blob.magic == NODE_BUFFER_STATE_MAGIC &&
                 blob.version == NODE_BUFFER_STATE_VERSION &&
                 blob.size == sizeof(blob) &&
                 blob.crc == state_blob_crc(&blob) &&
                 state_layout_valid(&state);
    if (!valid) {
        ESP_LOGE(TAG, "STATE2 invalid magic=0x%08" PRIX32 " version=%u size=%u "
                      "head=%" PRIu32 " next=%" PRIu32 " count=%" PRIu32
                      " crc=0x%04X expected=0x%04X; queue preserved",
                 blob.magic, blob.version, blob.size, state.head, state.next, state.count,
                 blob.crc, state_blob_crc(&blob));
        if (detail_out != NULL) {
            *detail_out = ESP_ERR_INVALID_CRC;
        }
        return STATE2_CORRUPT;
    }

    *state_out = state;
    return STATE2_VALID;
}

static bool payload_valid_for_seq(const sz2_payload_t *payload, uint32_t seq,
                                  const char **reason_out)
{
    const char *reason = NULL;
    if (payload->protocol_version != SZ2_PROTO_VERSION) {
        reason = "protocol";
    } else if (payload->packet_type != SZ2_PKT_DATA) {
        reason = "packet_type";
    } else if (payload->seq != seq) {
        reason = "seq";
    } else if (payload->crc != sz2_payload_crc(payload)) {
        reason = "crc";
    }
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    return reason == NULL;
}

static record_status_t read_record(uint32_t seq, sz2_payload_t *payload_out,
                                   esp_err_t *detail_out, const char **reason_out)
{
    char key[NVS_KEY_NAME_MAX_SIZE];
    record_key(seq, key);
    size_t len = sizeof(*payload_out);
    esp_err_t err = nvs_get_blob(s_nvs, key, payload_out, &len);
    if (detail_out != NULL) {
        *detail_out = err;
    }
    if (reason_out != NULL) {
        *reason_out = NULL;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return RECORD_MISSING;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        if (reason_out != NULL) {
            *reason_out = "stored_size/type";
        }
        return RECORD_CORRUPT;
    }
    if (err != ESP_OK) {
        return RECORD_IO_ERROR;
    }
    if (len != sizeof(*payload_out)) {
        if (reason_out != NULL) {
            *reason_out = "stored_size";
        }
        return RECORD_CORRUPT;
    }
    return payload_valid_for_seq(payload_out, seq, reason_out) ? RECORD_VALID
                                                               : RECORD_CORRUPT;
}

/*
 * Erase-first helper. A hívó csak durable ACK-olt vagy bizonyítottan hibás rekordokra használja.
 * Ha az erase commit után reset jön, a régi state a hiányzó frontra mutat, amit repair újraléptet.
 */
static esp_err_t erase_record_range(uint32_t first_seq, uint32_t count, const char *reason)
{
    bool dirty = false;
    esp_err_t first_error = ESP_OK;
    for (uint32_t i = 0; i < count; i++) {
        char key[NVS_KEY_NAME_MAX_SIZE];
        record_key(first_seq + i, key);
        esp_err_t err = nvs_erase_key(s_nvs, key);
        if (err == ESP_OK) {
            dirty = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            first_error = err;
            ESP_LOGE(TAG, "ERASE fail reason=%s seq=%" PRIu32 " err=%s",
                     reason, first_seq + i, esp_err_to_name(err));
            break;
        }
    }

    if (dirty) {
        esp_err_t commit_err = nvs_commit(s_nvs);
        if (commit_err != ESP_OK) {
            ESP_LOGE(TAG, "ERASE commit fail reason=%s first=%" PRIu32
                          " count=%" PRIu32 " err=%s",
                     reason, first_seq, count, esp_err_to_name(commit_err));
            return commit_err;
        }
    }
    return first_error;
}

/*
 * Missing/corrupt front repair: előbb minden bizonyítottan használhatatlan frontkulcs erase+commit,
 * utána egyetlen state2 commit. Power loss esetén a régi state újra ugyanazt a repairt futtatja.
 */
static esp_err_t repair_front(queue_state_t *state, sz2_payload_t *front_out,
                              bool *has_front_out)
{
    queue_state_t candidate = *state;
    uint32_t old_head = candidate.head;
    uint32_t missing = 0;
    uint32_t corrupt = 0;
    sz2_payload_t payload;

    while (candidate.count > 0) {
        esp_err_t detail = ESP_OK;
        const char *why = NULL;
        record_status_t status = read_record(candidate.head, &payload, &detail, &why);
        if (status == RECORD_VALID) {
            break;
        }
        if (status == RECORD_IO_ERROR) {
            ESP_LOGE(TAG, "FRONT_REPAIR read fail seq=%" PRIu32 " err=%s",
                     candidate.head, esp_err_to_name(detail));
            return detail;
        }

        if (status == RECORD_MISSING) {
            missing++;
            ESP_LOGW(TAG, "FRONT_REPAIR missing seq=%" PRIu32, candidate.head);
        } else {
            corrupt++;
            ESP_LOGE(TAG, "FRONT_REPAIR corrupt seq=%" PRIu32 " reason=%s",
                     candidate.head, why != NULL ? why : "unknown");
        }
        candidate.head++;
        candidate.count--;
    }
    if (candidate.count == 0) {
        candidate.head = candidate.next;
    }

    uint32_t skipped = missing + corrupt;
    if (skipped > 0) {
        esp_err_t err = erase_record_range(old_head, skipped, "front_repair");
        if (err != ESP_OK) {
            return err;
        }
        err = persist_state(&candidate, "front_repair");
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "FRONT_REPAIR committed old_head=%" PRIu32 " new_head=%" PRIu32
                      " skipped=%" PRIu32 " missing=%" PRIu32 " corrupt=%" PRIu32,
                 old_head, candidate.head, skipped, missing, corrupt);
        *state = candidate;
    }

    *has_front_out = candidate.count > 0;
    if (*has_front_out && front_out != NULL) {
        *front_out = payload;
    }
    return ESP_OK;
}

static esp_err_t read_legacy_u32(const char *key, legacy_u32_t *value)
{
    esp_err_t err = nvs_get_u32(s_nvs, key, &value->value);
    if (err == ESP_OK) {
        value->found = true;
        return ESP_OK;
    }
    value->found = false;
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
            ESP_LOGW(TAG, "LEGACY key=%s type mismatch; record scan fallback lehet", key);
        }
        return ESP_OK;
    }
    return err;
}

static esp_err_t reconstruct_legacy_from_records(uint32_t next_hint, bool have_next_hint,
                                                  queue_state_t *state_out)
{
    nvs_iterator_t it = NULL;
    esp_err_t iter_err = nvs_entry_find_in_handle(s_nvs, NVS_TYPE_BLOB, &it);
    bool have_record = false;
    uint32_t min_seq = 0;
    uint32_t max_seq = 0;
    uint32_t valid_records = 0;

    while (iter_err == ESP_OK) {
        nvs_entry_info_t info;
        esp_err_t info_err = nvs_entry_info(it, &info);
        if (info_err != ESP_OK) {
            nvs_release_iterator(it);
            return info_err;
        }

        uint32_t seq;
        if (parse_record_key(info.key, &seq)) {
            sz2_payload_t payload;
            esp_err_t detail = ESP_OK;
            const char *why = NULL;
            record_status_t status = read_record(seq, &payload, &detail, &why);
            if (status == RECORD_IO_ERROR) {
                nvs_release_iterator(it);
                return detail;
            }
            if (status == RECORD_VALID) {
                if (!have_record) {
                    min_seq = seq;
                    max_seq = seq;
                    have_record = true;
                } else {
                    if (seq < min_seq) min_seq = seq;
                    if (seq > max_seq) max_seq = seq;
                }
                valid_records++;
            } else if (status == RECORD_CORRUPT) {
                ESP_LOGE(TAG, "LEGACY_SCAN corrupt key=%s reason=%s; preserved outside state",
                         info.key, why != NULL ? why : "unknown");
            }
        }
        iter_err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (iter_err != ESP_ERR_NVS_NOT_FOUND) {
        return iter_err;
    }

    if (!have_record) {
        uint32_t next = have_next_hint ? next_hint : 0;
        *state_out = (queue_state_t) { .head = next, .next = next, .count = 0 };
        ESP_LOGW(TAG, "LEGACY_SCAN no valid records; empty state next=%" PRIu32, next);
        return ESP_OK;
    }

    uint64_t span64 = (uint64_t)max_seq - min_seq + 1U;
    if (span64 > NODE_PENDING_CAPACITY) {
        ESP_LOGE(TAG, "LEGACY_SCAN ambiguous span=%" PRIu64 " exceeds cap=%u; "
                      "records preserved, manual recovery required",
                 span64, NODE_PENDING_CAPACITY);
        return ESP_ERR_INVALID_STATE;
    }
    *state_out = (queue_state_t) {
        .head = min_seq,
        .next = max_seq + 1U,
        .count = (uint32_t)span64,
    };
    ESP_LOGW(TAG, "LEGACY_SCAN recovered head=%" PRIu32 " next=%" PRIu32
                  " span=%" PRIu32 " valid_records=%" PRIu32,
             state_out->head, state_out->next, state_out->count, valid_records);
    return ESP_OK;
}

static void cleanup_legacy_keys(void)
{
    static const char *keys[] = { "head", "next", "count" };
    bool dirty = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_err_t err = nvs_erase_key(s_nvs, keys[i]);
        if (err == ESP_OK) {
            dirty = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "LEGACY cleanup key=%s err=%s", keys[i], esp_err_to_name(err));
        }
    }
    if (dirty) {
        esp_err_t err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LEGACY cleanup commit err=%s; state2 mar authoritative",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t migrate_legacy_state(queue_state_t *state_out)
{
    legacy_u32_t head = { 0 };
    legacy_u32_t next = { 0 };
    legacy_u32_t count = { 0 };
    esp_err_t err = read_legacy_u32("head", &head);
    if (err == ESP_OK) err = read_legacy_u32("next", &next);
    if (err == ESP_OK) err = read_legacy_u32("count", &count);
    if (err != ESP_OK) {
        return err;
    }

    queue_state_t candidate = { 0 };
    bool candidate_ready = false;
    if (head.found && next.found && (uint32_t)(next.value - head.value) <= NODE_PENDING_CAPACITY) {
        candidate.head = head.value;
        candidate.next = next.value;
        candidate.count = (uint32_t)(next.value - head.value);
        candidate_ready = true;
        if (!count.found || count.value != candidate.count) {
            ESP_LOGW(TAG, "LEGACY inconsistent count stored=%s%" PRIu32
                          " derived=%" PRIu32 "; endpoints win",
                     count.found ? "" : "missing/", count.value, candidate.count);
        }
    } else if (head.found && count.found && count.value <= NODE_PENDING_CAPACITY) {
        candidate.head = head.value;
        candidate.next = head.value + count.value;
        candidate.count = count.value;
        candidate_ready = true;
        ESP_LOGW(TAG, "LEGACY next missing/inconsistent; derived from head+count");
    } else if (next.found && count.found && count.value <= NODE_PENDING_CAPACITY) {
        candidate.head = next.value - count.value;
        candidate.next = next.value;
        candidate.count = count.value;
        candidate_ready = true;
        ESP_LOGW(TAG, "LEGACY head missing/inconsistent; derived from next-count");
    }

    if (!candidate_ready || !state_layout_valid(&candidate)) {
        err = reconstruct_legacy_from_records(next.value, next.found, &candidate);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = persist_state(&candidate, "legacy_migration");
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGW(TAG, "LEGACY migrated to state2 head=%" PRIu32 " next=%" PRIu32
                  " count=%" PRIu32,
             candidate.head, candidate.next, candidate.count);
    cleanup_legacy_keys();
    *state_out = candidate;
    return ESP_OK;
}

static esp_err_t adopt_expected_orphan(queue_state_t *state)
{
    if (state->count >= NODE_PENDING_CAPACITY) {
        return ESP_OK;
    }

    sz2_payload_t payload;
    esp_err_t detail = ESP_OK;
    const char *why = NULL;
    record_status_t status = read_record(state->next, &payload, &detail, &why);
    if (status == RECORD_MISSING) {
        return ESP_OK;
    }
    if (status == RECORD_IO_ERROR) {
        return detail;
    }
    if (status == RECORD_CORRUPT) {
        ESP_LOGE(TAG, "ORPHAN invalid expected_seq=%" PRIu32 " reason=%s; erase",
                 state->next, why != NULL ? why : "unknown");
        return erase_record_range(state->next, 1, "invalid_orphan");
    }

    queue_state_t candidate = *state;
    if (candidate.count == 0) {
        candidate.head = candidate.next;
    }
    candidate.next++;
    candidate.count++;
    esp_err_t err = persist_state(&candidate, "orphan_adopt");
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGW(TAG, "ORPHAN adopted seq=%" PRIu32 " head=%" PRIu32
                  " next=%" PRIu32 " count=%" PRIu32,
             payload.seq, candidate.head, candidate.next, candidate.count);
    *state = candidate;
    return ESP_OK;
}

esp_err_t node_buffer_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init_partition(NODE_BUFFER_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf NVS init hiba: %s; partition preserved, nincs auto-erase",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_open_from_partition(NODE_BUFFER_PARTITION, NODE_BUFFER_NAMESPACE,
                                  NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf NVS open hiba: %s", esp_err_to_name(err));
        return err;
    }

    queue_state_t state;
    esp_err_t detail = ESP_OK;
    state2_status_t status = load_state2(&state, &detail);
    if (status == STATE2_NOT_FOUND) {
        err = migrate_legacy_state(&state);
    } else if (status == STATE2_CORRUPT) {
        err = detail != ESP_OK ? detail : ESP_ERR_INVALID_CRC;
    } else if (status == STATE2_IO_ERROR) {
        err = detail;
    } else {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf state init hiba: %s; queue erintetlen",
                 esp_err_to_name(err));
        nvs_close(s_nvs);
        return err;
    }

    sz2_payload_t front;
    bool has_front = false;
    err = repair_front(&state, &front, &has_front);
    if (err == ESP_OK) {
        err = adopt_expected_orphan(&state);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf recovery hiba: %s; RAM state nem lepett",
                 esp_err_to_name(err));
        nvs_close(s_nvs);
        return err;
    }
    has_front = state.count > 0;

    apply_state(&state);
    s_ready = true;
    ESP_LOGI(TAG, "nodebuf ready state2: head=%" PRIu32 " next=%" PRIu32
                  " count=%" PRIu32 " cap=%u front=%s",
             s_head, s_next, s_count, NODE_PENDING_CAPACITY, has_front ? "VALID" : "EMPTY");
    return ESP_OK;
}

uint32_t node_buffer_next_seq(void)
{
    return s_next;
}

uint32_t node_buffer_count(void)
{
    return s_count;
}

esp_err_t node_buffer_enqueue(const sz2_payload_t *payload)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= NODE_PENDING_CAPACITY) {
        ESP_LOGE(TAG, "pending queue tele: count=%" PRIu32 " cap=%u, seq=%" PRIu32
                      " nincs mentve",
                 s_count, NODE_PENDING_CAPACITY, payload->seq);
        return ESP_ERR_NO_MEM;
    }
    if (payload->seq != s_next) {
        ESP_LOGE(TAG, "ENQUEUE seq contract fail payload=%" PRIu32 " expected=%" PRIu32,
                 payload->seq, s_next);
        return ESP_ERR_INVALID_ARG;
    }
    const char *why = NULL;
    if (!payload_valid_for_seq(payload, s_next, &why)) {
        ESP_LOGE(TAG, "ENQUEUE invalid payload seq=%" PRIu32 " reason=%s", s_next, why);
        return ESP_ERR_INVALID_CRC;
    }

    sz2_payload_t existing;
    esp_err_t detail = ESP_OK;
    record_status_t existing_status = read_record(s_next, &existing, &detail, &why);
    if (existing_status == RECORD_VALID) {
        ESP_LOGE(TAG, "ENQUEUE blocked: valid orphan already at expected seq=%" PRIu32
                      "; reboot/init will adopt",
                 s_next);
        return ESP_ERR_INVALID_STATE;
    }
    if (existing_status == RECORD_IO_ERROR) {
        return detail;
    }
    if (existing_status == RECORD_CORRUPT) {
        ESP_LOGW(TAG, "ENQUEUE replaces invalid expected key seq=%" PRIu32 " reason=%s",
                 s_next, why != NULL ? why : "unknown");
    }

    char key[NVS_KEY_NAME_MAX_SIZE];
    record_key(payload->seq, key);
    esp_err_t err = nvs_set_blob(s_nvs, key, payload, sizeof(*payload));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RECORD commit fail seq=%" PRIu32 " err=%s; RAM state valtozatlan",
                 payload->seq, esp_err_to_name(err));
        return err;
    }

    queue_state_t candidate = current_state();
    if (candidate.count == 0) {
        candidate.head = payload->seq;
    }
    candidate.next = payload->seq + 1U;
    candidate.count++;
    err = persist_state(&candidate, "enqueue");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ORPHAN retained seq=%" PRIu32
                      " after state fail; RAM next=%" PRIu32 " unchanged",
                 payload->seq, s_next);
        return err;
    }

    apply_state(&candidate);
    ESP_LOGI(TAG, "pending committed: seq=%" PRIu32 " head=%" PRIu32
                  " next=%" PRIu32 " count=%" PRIu32,
             payload->seq, s_head, s_next, s_count);
    return ESP_OK;
}

esp_err_t node_buffer_peek_oldest(sz2_payload_t *payload_out)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    queue_state_t candidate = current_state();
    bool has_front = false;
    esp_err_t err = repair_front(&candidate, payload_out, &has_front);
    if (err != ESP_OK) {
        return err;
    }
    apply_state(&candidate);
    return has_front ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t node_buffer_clear_until(uint32_t seq)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    queue_state_t candidate = current_state();
    uint32_t old_head = candidate.head;
    uint32_t cleared = 0;
    while (candidate.count > 0 && seq_leq(candidate.head, seq)) {
        candidate.head++;
        candidate.count--;
        cleared++;
    }
    if (candidate.count == 0) {
        candidate.head = candidate.next;
    }
    if (cleared == 0) {
        return ESP_OK;
    }

    /* Csak a master durable ACK-jával lefedett rekordok: erase commit mindig state commit előtt. */
    esp_err_t err = erase_record_range(old_head, cleared, "clear_until");
    if (err != ESP_OK) {
        return err;
    }
    err = persist_state(&candidate, "clear_until");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CLEAR state fail old_head=%" PRIu32 " target_head=%" PRIu32
                      "; RAM regi, front repair fogja ujraepiteni",
                 old_head, candidate.head);
        return err;
    }

    apply_state(&candidate);
    ESP_LOGI(TAG, "pending cleared: clear_until=%" PRIu32 " cleared=%" PRIu32
                  " head=%" PRIu32 " remain=%" PRIu32,
             seq, cleared, s_head, s_count);
    return ESP_OK;
}
