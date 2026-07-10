/*
 * node_buffer.c — flash-backed pending queue NVS partíción.
 */
#include "node_buffer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "node_config.h"

#define NODE_BUFFER_PARTITION "nodebuf"
#define NODE_BUFFER_NAMESPACE "queue"

static const char *TAG = "node_buffer";

static nvs_handle_t s_nvs;
static bool s_ready = false;
static uint32_t s_head = 0;
static uint32_t s_next = 0;
static uint32_t s_count = 0;

static void record_key(uint32_t seq, char key[16])
{
    snprintf(key, 16, "r%08" PRIX32, seq);
}

static bool seq_leq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static esp_err_t save_meta(void)
{
    esp_err_t err = nvs_set_u32(s_nvs, "head", s_head);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(s_nvs, "next", s_next);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(s_nvs, "count", s_count);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs);
}

static esp_err_t load_meta(void)
{
    bool dirty = false;

    if (nvs_get_u32(s_nvs, "head", &s_head) == ESP_ERR_NVS_NOT_FOUND) {
        s_head = 0;
        dirty = true;
    }
    if (nvs_get_u32(s_nvs, "next", &s_next) == ESP_ERR_NVS_NOT_FOUND) {
        s_next = 0;
        dirty = true;
    }
    if (nvs_get_u32(s_nvs, "count", &s_count) == ESP_ERR_NVS_NOT_FOUND) {
        s_count = 0;
        dirty = true;
    }

    while (s_count > NODE_PENDING_CAPACITY) {
        char key[16];
        record_key(s_head, key);
        nvs_erase_key(s_nvs, key);
        s_head++;
        s_count--;
        dirty = true;
    }
    if (s_count == 0) {
        s_head = s_next;
    }

    return dirty ? save_meta() : ESP_OK;
}

esp_err_t node_buffer_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init_partition(NODE_BUFFER_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NODE_BUFFER_PARTITION));
        err = nvs_flash_init_partition(NODE_BUFFER_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf NVS init hiba: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open_from_partition(NODE_BUFFER_PARTITION, NODE_BUFFER_NAMESPACE,
                                  NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf NVS open hiba: %s", esp_err_to_name(err));
        return err;
    }

    err = load_meta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nodebuf meta hiba: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "nodebuf ready: head=%" PRIu32 " next=%" PRIu32 " count=%" PRIu32
                  " cap=%u",
             s_head, s_next, s_count, NODE_PENDING_CAPACITY);
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
    if (s_count >= NODE_PENDING_CAPACITY) {
        ESP_LOGE(TAG, "pending queue tele: count=%" PRIu32 " cap=%u, seq=%" PRIu32
                      " nincs mentve",
                 s_count, NODE_PENDING_CAPACITY, payload->seq);
        return ESP_ERR_NO_MEM;
    }

    char key[16];
    record_key(payload->seq, key);
    esp_err_t err = nvs_set_blob(s_nvs, key, payload, sizeof(*payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "record write hiba seq=%" PRIu32 ": %s",
                 payload->seq, esp_err_to_name(err));
        return err;
    }

    if (s_count == 0) {
        s_head = payload->seq;
    }
    if (seq_leq(s_next, payload->seq)) {
        s_next = payload->seq + 1;
    }
    s_count++;

    err = save_meta();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "pending mentve: seq=%" PRIu32 " count=%" PRIu32,
                 payload->seq, s_count);
    } else {
        ESP_LOGE(TAG, "meta commit hiba seq=%" PRIu32 ": %s",
                 payload->seq, esp_err_to_name(err));
    }
    return err;
}

esp_err_t node_buffer_peek_oldest(sz2_payload_t *payload_out)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    bool dirty = false;

    while (s_count > 0) {
        char key[16];
        record_key(s_head, key);
        size_t len = sizeof(*payload_out);
        esp_err_t err = nvs_get_blob(s_nvs, key, payload_out, &len);
        if (err == ESP_OK && len == sizeof(*payload_out)) {
            if (dirty) {
                esp_err_t meta_err = save_meta();
                if (meta_err != ESP_OK) return meta_err;
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG, "hianyzo/hibas pending rekord: seq=%" PRIu32 " err=%s len=%u",
                 s_head, esp_err_to_name(err), (unsigned)len);
        nvs_erase_key(s_nvs, key);
        s_head++;
        s_count--;
        dirty = true;
    }

    s_head = s_next;
    if (dirty) {
        esp_err_t err = save_meta();
        if (err != ESP_OK) return err;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t node_buffer_clear_until(uint32_t seq)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t cleared = 0;
    while (s_count > 0 && seq_leq(s_head, seq)) {
        char key[16];
        record_key(s_head, key);
        esp_err_t err = nvs_erase_key(s_nvs, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "record erase hiba seq=%" PRIu32 ": %s",
                     s_head, esp_err_to_name(err));
            return err;
        }
        s_head++;
        s_count--;
        cleared++;
    }

    if (s_count == 0) {
        s_head = s_next;
    }

    esp_err_t err = save_meta();
    if (err == ESP_OK && cleared > 0) {
        ESP_LOGI(TAG, "pending torolve: clear_until=%" PRIu32 " cleared=%" PRIu32
                      " remain=%" PRIu32,
                 seq, cleared, s_count);
    }
    return err;
}
