#!/usr/bin/env python3
"""Extract exact node ACK filter and master registry function bodies for host tests."""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
NODE = (HERE / "../../node/main/espnow_tx.c").resolve()
MASTER = (HERE / "../../master/main/master.c").resolve()
OUT = HERE / "generated_radio.c"

def func(text: str, name: str) -> str:
    m = re.search(r"^static\s+[^\n]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", text, re.M | re.S)
    if not m: raise SystemExit(f"missing {name}")
    start=m.start(); i=text.find("{", m.start(), m.end()); depth=0
    in_s=in_c=esc=False
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ""
        if esc: esc=False
        elif (in_s or in_c) and c=="\\": esc=True
        elif not in_c and c=='"': in_s=not in_s
        elif not in_s and c=="'": in_c=not in_c
        elif not in_s and not in_c and c=="/" and n=="*": i=text.index("*/",i+2)+1
        elif not in_s and not in_c and c=="/" and n=="/": i=text.find("\n",i+2)
        elif not in_s and not in_c:
            if c=="{": depth+=1
            elif c=="}":
                depth-=1
                if depth==0: return text[start:i+1]+"\n"
        i+=1
    raise SystemExit(f"unterminated {name}")

n=NODE.read_text(); m=MASTER.read_text()
node_recv=func(n,"recv_cb")
master_get=func(m,"node_get")

src = '''/* GENERATED from unchanged production sources. */
#include "radio_host.h"
#define MASTER_MAX_NODES 16
unsigned g_sem_gives;
static SemaphoreHandle_t s_ack_sem;
static volatile uint32_t s_ack_wait_seq;
static volatile uint8_t s_ack_wait_node;
static volatile uint32_t s_ack_wait_boot;
static uint8_t s_master_mac[ESP_NOW_ETH_ALEN];
static uint8_t s_master_known;
static uint32_t s_registry_full_noack;
#define TAG "RADIO_SIM"
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
static sz2_ack_t s_ack;
static uint8_t s_ack_src[ESP_NOW_ETH_ALEN];
typedef host_node_state_t node_state_t;
static node_state_t s_nodes[MASTER_MAX_NODES];
''' + node_recv + master_get + r'''
void prod_node_ack_reset(void)
{ s_ack_sem=(void*)1; s_ack_wait_seq=0; s_ack_wait_node=0; s_ack_wait_boot=0; s_master_known=0; memset(s_master_mac,0,6); memset(&s_ack,0,sizeof(s_ack)); memset(s_ack_src,0,6); g_sem_gives=0; }
void prod_node_ack_wait(uint8_t node, uint32_t seq) { s_ack_wait_node=node; s_ack_wait_seq=seq; }
bool prod_node_ack_inject(const uint8_t src[6], const void *data, int len,
                          sz2_ack_t *accepted, uint8_t accepted_src[6])
{
    unsigned before=g_sem_gives; esp_now_recv_info_t i={.src_addr=src}; recv_cb(&i,data,len);
    if (g_sem_gives==before) return false;
    if (accepted) *accepted=s_ack; if (accepted_src) memcpy(accepted_src,s_ack_src,6); return true;
}
void prod_master_registry_reset(void) { memset(s_nodes,0,sizeof(s_nodes)); }
host_node_state_t *prod_master_node_get(uint8_t node, const uint8_t mac[6])
{ return (host_node_state_t*)node_get(node,mac); }
'''
OUT.write_text(src)
print(f"extracted recv_cb from {NODE} and node_get from {MASTER}")
