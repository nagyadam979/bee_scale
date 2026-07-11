/*
 * nvs_sim.c — mock NVS backend a node_buffer.c host-oldali fuzzolásához.
 *
 * Szemantika:
 *   - Egyetlen namespace ("queue"), kulcs->typed blob map a megosztott memóriában.
 *   - Minden SIKERESEN visszatért nvs_set_blob / nvs_erase_key DURABLE: a valós NVS
 *     bejegyzés-szinten CRC-vel atomi, ezért a mockban a mutáció egy lépésben,
 *     teljesen történik meg. nvs_commit no-op siker (az IDF NVS-ben is lényegében az).
 *   - Áramszünet-injektálás: tick() minden API hívásnál fogyaszt a cut_budget-ből;
 *     mutáló művelet a művelet ELŐTT és UTÁN is tickel. 0-nál _exit(SIM_EXIT_CUT):
 *     a gyerek-processz RAM-ja (a node_buffer.c összes static state-je) elveszik,
 *     a map (flash) megmarad — architektúrálisan hű reset.
 *   - A get/iterátor hívások is tickelnek: az ott ütő vágás ekvivalens a "következő
 *     mutáció előtti" vágással, de az init/recovery útvonalakat is megszakítja.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sim_shared.h"

int g_sim_log_enabled = 0;

static sim_shared_t *g;

void nvs_sim_attach(sim_shared_t *shm)
{
    g = shm;
}

static void tick(void)
{
    if (g == NULL) {
        fprintf(stderr, "nvs_sim: nincs attach-olt shared state\n");
        abort();
    }
    g->api_calls_total++;
    if (g->cut_armed && --g->cut_budget <= 0) {
        g->cut_armed = false;
        g->cuts_total++;
        _exit(SIM_EXIT_CUT); /* "áramszünet": RAM elveszik, a map (flash) megmarad */
    }
}

static sim_entry_t *lookup(const char *key)
{
    for (int i = 0; i < SIM_MAX_ENTRIES; i++) {
        if (g->entries[i].used && strncmp(g->entries[i].key, key, SIM_KEY_SIZE) == 0) {
            return &g->entries[i];
        }
    }
    return NULL;
}

static sim_entry_t *alloc_entry(void)
{
    for (int i = 0; i < SIM_MAX_ENTRIES; i++) {
        if (!g->entries[i].used) {
            return &g->entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_flash_init_partition(const char *partition_label)
{
    (void)partition_label;
    tick();
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *part_name, const char *namespace_name,
                                  nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)part_name; (void)namespace_name; (void)open_mode;
    tick();
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    tick();
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    tick(); /* vágás a művelet ELŐTT: semmi nem íródott */
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
        return ESP_ERR_NVS_KEY_TOO_LONG;
    }
    if (length > SIM_DATA_MAX) {
        fprintf(stderr, "nvs_sim: blob len=%zu > SIM_DATA_MAX\n", length);
        abort();
    }
    sim_entry_t *e = lookup(key);
    if (e == NULL) {
        e = alloc_entry();
        if (e == NULL) {
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        memset(e->key, 0, SIM_KEY_SIZE);
        memcpy(e->key, key, strlen(key));
    }
    e->type = (uint8_t)NVS_TYPE_BLOB;
    e->len = (uint16_t)length;
    memcpy(e->data, value, length);
    e->used = true;
    tick(); /* vágás a művelet UTÁN: az írás már durable */
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void)handle;
    tick();
    if (key == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sim_entry_t *e = lookup(key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (e->type != (uint8_t)NVS_TYPE_BLOB) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out_value == NULL) {
        *length = e->len;
        return ESP_OK;
    }
    if (*length < e->len) {
        *length = e->len; /* az IDF is a tárolt méretet adja vissza ilyenkor */
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, e->data, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    (void)handle;
    tick();
    if (key == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sim_entry_t *e = lookup(key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (e->type != (uint8_t)NVS_TYPE_U32 || e->len != sizeof(uint32_t)) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    memcpy(out_value, e->data, sizeof(uint32_t));
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    tick(); /* vágás a művelet ELŐTT */
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sim_entry_t *e = lookup(key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    e->used = false;
    tick(); /* vágás a művelet UTÁN: a törlés már durable */
    return ESP_OK;
}

/* ---------------- iterátor API (a legacy rekord-scanhez) ---------------- */

struct nvs_opaque_iterator_t {
    int idx;
    uint8_t type;
};

static int next_match(int from, uint8_t type)
{
    for (int i = from; i < SIM_MAX_ENTRIES; i++) {
        if (g->entries[i].used &&
            (type == (uint8_t)NVS_TYPE_ANY || g->entries[i].type == type)) {
            return i;
        }
    }
    return -1;
}

esp_err_t nvs_entry_find_in_handle(nvs_handle_t handle, nvs_type_t type,
                                   nvs_iterator_t *output_iterator)
{
    (void)handle;
    tick();
    if (output_iterator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *output_iterator = NULL;
    int idx = next_match(0, (uint8_t)type);
    if (idx < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_iterator_t it = calloc(1, sizeof(*it));
    if (it == NULL) {
        return ESP_ERR_NO_MEM;
    }
    it->idx = idx;
    it->type = (uint8_t)type;
    *output_iterator = it;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *iterator)
{
    tick();
    if (iterator == NULL || *iterator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = next_match((*iterator)->idx + 1, (*iterator)->type);
    if (idx < 0) {
        free(*iterator);
        *iterator = NULL; /* az IDF is felszabadít és NULL-oz a lista végén */
        return ESP_ERR_NVS_NOT_FOUND;
    }
    (*iterator)->idx = idx;
    return ESP_OK;
}

esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info)
{
    tick();
    if (iterator == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const sim_entry_t *e = &g->entries[iterator->idx];
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->namespace_name, sizeof(out_info->namespace_name), "queue");
    memcpy(out_info->key, e->key, SIM_KEY_SIZE);
    out_info->type = (nvs_type_t)e->type;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t iterator)
{
    free(iterator); /* free(NULL) safe — a node_buffer.c NULL-lal is hívja */
}

/* ---------------- esp_err_to_name ---------------- */

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                       return "ESP_OK";
    case ESP_FAIL:                     return "ESP_FAIL";
    case ESP_ERR_NO_MEM:               return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:          return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:         return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:            return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_INVALID_CRC:          return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_NVS_NOT_FOUND:        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_TYPE_MISMATCH:    return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_INVALID_LENGTH:   return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_KEY_TOO_LONG:     return "ESP_ERR_NVS_KEY_TOO_LONG";
    default:                           return "ESP_ERR_(ismeretlen)";
    }
}
