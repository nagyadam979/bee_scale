#!/usr/bin/env python3
"""Build-time extraction of the unchanged backup-ring function bodies from master.c."""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
SRC = (HERE / "../../master/main/master.c").resolve()
OUT = HERE / "generated_master_ring.c"

text = SRC.read_text()

def function(name: str) -> str:
    m = re.search(r"^static\s+[^\n]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", text, re.M | re.S)
    if not m:
        raise SystemExit(f"cannot find function {name} in {SRC}")
    start = m.start()
    brace = text.find("{", m.start(), m.end())
    depth = 0
    i = brace
    in_string = in_char = False
    escape = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if escape:
            escape = False
        elif (in_string or in_char) and c == "\\":
            escape = True
        elif not in_char and c == '"':
            in_string = not in_string
        elif not in_string and c == "'":
            in_char = not in_char
        elif not in_string and not in_char and c == "/" and n == "*":
            end = text.find("*/", i + 2)
            if end < 0: raise SystemExit("unterminated comment")
            i = end + 1
        elif not in_string and not in_char and c == "/" and n == "/":
            end = text.find("\n", i + 2)
            i = len(text) if end < 0 else end
        elif not in_string and not in_char:
            if c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1] + "\n"
        i += 1
    raise SystemExit(f"unterminated function {name}")

names = [
    "crc32_update", "rain_store_count", "rain_apply_durable_cursor",
    "rain_get_durable_cursor", "backup_store_index_values", "backup_count_unlocked",
    "master_next_seq", "backup_init", "backup_slot_decode", "backup_slot_erased",
    "backup_prepare_head_unlocked", "backup_enqueue", "backup_peek", "backup_pop",
    "master_record_key",
]

record_start = text.index("typedef struct __attribute__((packed)) {\n    uint8_t type;")
record_end_anchor = "} backup_index_legacy_t;"
record_end = text.index(record_end_anchor, record_start) + len(record_end_anchor)
types = text[record_start:record_end]

preamble = r'''/* GENERATED: do not edit; exact function bodies extracted from master.c. */
#include "ring_host.h"

static const char *TAG = "MASTER_RING_SIM";
'''

globals_ = r'''
_Static_assert(sizeof(host_master_record_t) == sizeof(master_record_t),
               "host/production master_record layout mismatch");
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
'''

exports = r'''
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
int prod_master_record_key(char *out, size_t cap, const host_master_record_t *rec)
{ return master_record_key(out, cap, (const master_record_t *)rec); }
'''

body = "\n".join(function(n) for n in names)
OUT.write_text(preamble + types + "\n" + globals_ + "\n" + body + exports)
print(f"extracted {len(names)} production functions from {SRC}")
