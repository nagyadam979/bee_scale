#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ring_host.h"

fsim_shared_t *g_fsim;
int g_mock_log_enabled;
static esp_partition_t g_part = { .size = FSIM_FLASH_SIZE };
static fsim_nvs_entry_t g_stage;
static bool g_stage_valid;
static int g_fail_next_read;

void fsim_attach(fsim_shared_t *s)
{
    g_fsim = s;
    g_stage_valid = false;
    g_fail_next_read = 0;
}

uint64_t fsim_rand(void)
{
    uint64_t x = g_fsim->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_fsim->rng = x;
    return x * 2685821657736338717ULL;
}

int fsim_mutation_should_cut(void)
{
    g_fsim->mutation_ops++;
    if (g_fsim->cut_countdown < 0) return 0;
    if (g_fsim->cut_countdown-- == 0) {
        g_fsim->cuts_fired++;
        return 1;
    }
    return 0;
}

void fsim_die_cut(void) { _exit(FSIM_EXIT_CUT); }

void fsim_log(const char *tag, const char *fmt, ...)
{
    fprintf(stderr, "[%s] ", tag);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static fsim_nvs_entry_t *nvs_find(const char *key)
{
    for (unsigned i = 0; i < FSIM_NVS_MAX; ++i)
        if (g_fsim->nvs[i].used && strcmp(g_fsim->nvs[i].key, key) == 0) return &g_fsim->nvs[i];
    return NULL;
}

static fsim_nvs_entry_t *nvs_alloc(const char *key)
{
    fsim_nvs_entry_t *e = nvs_find(key);
    if (e) return e;
    for (unsigned i = 0; i < FSIM_NVS_MAX; ++i) if (!g_fsim->nvs[i].used) {
        e = &g_fsim->nvs[i]; memset(e, 0, sizeof(*e)); e->used = 1;
        snprintf(e->key, sizeof(e->key), "%s", key); return e;
    }
    return NULL;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{ (void)type; (void)subtype; return strcmp(label, "backup") == 0 ? &g_part : NULL; }

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    (void)p; g_fsim->read_ops++;
    if (off + len > FSIM_FLASH_SIZE) { g_fsim->oob_accesses++; return ESP_ERR_INVALID_SIZE; }
    if (g_fail_next_read) { g_fail_next_read = 0; return ESP_FAIL; }
    memcpy(dst, g_fsim->flash + off, len); return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    (void)p;
    if (off + len > FSIM_FLASH_SIZE) { g_fsim->oob_accesses++; return ESP_ERR_INVALID_SIZE; }
    bool cut = fsim_mutation_should_cut();
    size_t n = cut ? (size_t)(fsim_rand() % (len + 1U)) : len;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; ++i) {
        if ((g_fsim->flash[off + i] & s[i]) != s[i]) {
            g_fsim->program_violations++;
            if (g_fsim->program_violations == 1) g_fsim->violation_first_off = (uint32_t)(off + i);
        }
        g_fsim->flash[off + i] &= s[i];
    }
    if (cut) fsim_die_cut();
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    (void)p;
    if ((off % FSIM_SECTOR) || (len % FSIM_SECTOR)) { g_fsim->erase_misaligned++; return ESP_ERR_INVALID_ARG; }
    if (off + len > FSIM_FLASH_SIZE) { g_fsim->oob_accesses++; return ESP_ERR_INVALID_SIZE; }
    bool cut = fsim_mutation_should_cut();
    size_t n = cut ? (size_t)(fsim_rand() % (len + 1U)) : len;
    memset(g_fsim->flash + off, 0xff, n);
    if (cut) fsim_die_cut();
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h; fsim_nvs_entry_t *e = nvs_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->blob_len; return ESP_OK; }
    if (*len < e->blob_len) { *len = e->blob_len; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, e->blob, e->blob_len); *len = e->blob_len; return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    (void)h; if (len > sizeof(g_stage.blob)) return ESP_ERR_INVALID_SIZE;
    memset(&g_stage, 0, sizeof(g_stage)); g_stage.used = 1; g_stage.type = 2;
    snprintf(g_stage.key, sizeof(g_stage.key), "%s", key);
    memcpy(g_stage.blob, v, len); g_stage.blob_len = (uint32_t)len; g_stage_valid = true;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{ (void)h; fsim_nvs_entry_t *e = nvs_find(key); if (!e) return ESP_ERR_NVS_NOT_FOUND; *out = e->u32; return ESP_OK; }

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{
    (void)h; memset(&g_stage, 0, sizeof(g_stage)); g_stage.used = 1; g_stage.type = 1;
    snprintf(g_stage.key, sizeof(g_stage.key), "%s", key); g_stage.u32 = v; g_stage_valid = true; return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h; if (!g_stage_valid) return ESP_OK;
    bool cut = fsim_mutation_should_cut();
    bool publish = !cut || (fsim_rand() & 1U);
    if (publish) { fsim_nvs_entry_t *e = nvs_alloc(g_stage.key); if (!e) return ESP_FAIL; *e = g_stage; }
    g_stage_valid = false;
    if (cut) fsim_die_cut();
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
void fsim_fail_next_read(void) { g_fail_next_read = 1; }
