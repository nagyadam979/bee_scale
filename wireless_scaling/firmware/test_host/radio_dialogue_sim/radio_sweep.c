#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_host.h"

static void check(bool ok, const char *msg) { if (!ok) { fprintf(stderr,"FAIL %s\n",msg); exit(1); } }
static const uint8_t MASTER[6]={0x10,1,2,3,4,5}, OTHER[6]={0x20,1,2,3,4,5};

typedef struct {
    bool have_boot, have_seq;
    uint32_t boot, last_seq, stored, acked, dup, resets, gaps;
    bool durable_available;
} dialogue_t;

static bool hello(dialogue_t *d, uint32_t boot, bool crc_ok, bool version_ok)
{
    if (!version_ok || !crc_ok) return false;
    if (!d->have_boot) d->have_boot=true;
    else if (boot!=d->boot) { d->have_seq=false; d->resets++; }
    d->boot=boot; d->acked++; return true;
}

static bool data(dialogue_t *d, uint32_t boot, uint32_t seq, bool crc_ok, bool version_ok)
{
    if (!version_ok || !crc_ok) return false;
    bool changed=d->have_boot && boot!=d->boot;
    bool fresh=changed || !d->have_seq || seq>d->last_seq;
    if (fresh) {
        if (!d->durable_available) return false;
        if (!changed && d->have_seq) d->gaps += seq-d->last_seq-1u;
        if (changed) d->resets++;
        d->have_boot=d->have_seq=true; d->boot=boot; d->last_seq=seq; d->stored++;
    } else d->dup++;
    d->acked++; return true;
}

static sz2_ack_t ack(uint8_t node,uint32_t seq)
{
    sz2_ack_t a={0}; a.protocol_version=SZ2_PROTO_VERSION; a.packet_type=SZ2_PKT_ACK;
    a.node_id=node; a.ack_last_seq=seq; a.clear_buffer_until_seq=seq; return a;
}

int main(void)
{
    prod_node_ack_reset(); prod_node_ack_wait(7,42);
    sz2_ack_t a=ack(7,42), out; uint8_t src[6];
    check(prod_node_ack_inject(MASTER,&a,sizeof(a),&out,src),"valid ACK rejected");
    check(memcmp(src,MASTER,6)==0 && out.clear_buffer_until_seq==42,"valid ACK contents");
    a.protocol_version++; check(!prod_node_ack_inject(MASTER,&a,sizeof(a),0,0),"bad version ACK accepted");
    a=ack(7,42); a.packet_type=SZ2_PKT_DATA; check(!prod_node_ack_inject(MASTER,&a,sizeof(a),0,0),"bad type ACK accepted");
    a=ack(8,42); check(!prod_node_ack_inject(MASTER,&a,sizeof(a),0,0),"wrong node ACK accepted");
    a=ack(7,41); check(!prod_node_ack_inject(MASTER,&a,sizeof(a),0,0),"stale seq ACK accepted");
    a=ack(7,42); check(!prod_node_ack_inject(MASTER,&a,sizeof(a)-1,0,0),"wrong length ACK accepted");
    printf("PASS exact node ACK filter: length/version/type/node/seq\n");

    /* Produkciós recv_cb jelenleg nem hitelesíti az ACK forrását. */
    prod_node_ack_wait(7,42);
    check(prod_node_ack_inject(OTHER,&a,sizeof(a),&out,src),"source behavior changed");
    check(memcmp(src,OTHER,6)==0,"foreign ACK source not learned");
    printf("OBSERVED foreign-source matching ACK accepted (exact production recv_cb)\n");
    /* boot_id nincs az ACK-ban: seq újrahasználatnál egy régi boot késleltetett ACK-ja megkülönböztethetetlen. */
    prod_node_ack_wait(7,1); a=ack(7,1);
    check(prod_node_ack_inject(MASTER,&a,sizeof(a),0,0),"delayed old-boot ACK not reproduced");
    printf("OBSERVED delayed old-boot ACK accepted after seq reuse (wire limitation)\n");

    prod_master_registry_reset();
    for (uint8_t i=0;i<16;i++) { uint8_t mac[6]={2,0,0,0,0,i}; check(prod_master_node_get(i,mac)!=0,"registry early full"); }
    uint8_t m17[6]={2,0,0,0,0,17}; check(prod_master_node_get(17,m17)==0,"17th registry entry accepted");
    printf("PASS exact master registry capacity: 16, then NULL/no-ACK path\n");
    uint8_t original[6]={3,1,1,1,1,1}, collision[6]={4,2,2,2,2,2};
    prod_master_registry_reset(); host_node_state_t *n=prod_master_node_get(9,original);
    check(n && prod_master_node_get(9,collision)==n && memcmp(n->mac,collision,6)==0,"node-id takeover reset missing");
    printf("PASS same node_id with a new source MAC takes over and resets registry state\n");

    dialogue_t d={.durable_available=true};
    check(hello(&d,100,true,true),"HELLO");
    check(data(&d,100,1,true,true),"DATA 1");
    /* elveszett ACK: ugyanaz a DATA újraérkezik, nem tárolódik kétszer, de újra ACK-olódik */
    uint32_t stored=d.stored; check(data(&d,100,1,true,true),"duplicate after lost ACK");
    check(d.stored==stored && d.dup==1,"lost ACK idempotence");
    /* elveszett DATA: nincs state change; későbbi retry normál új rekord */
    check(data(&d,100,2,true,true),"retry after lost DATA");
    /* queue full: nincs ACK és last_seq sem lép; felszabadulás után retry elfogadható */
    d.durable_available=false; check(!data(&d,100,3,true,true),"queue-full DATA ACKed");
    check(d.last_seq==2,"queue-full advanced seq"); d.durable_available=true;
    check(data(&d,100,3,true,true),"queue recovery retry");
    /* CRC/verzió eldobás; bootváltás seq resetet enged */
    check(!data(&d,100,4,false,true) && !data(&d,100,4,true,false),"invalid DATA accepted");
    check(hello(&d,200,true,true) && data(&d,200,0,true,true),"boot reset/seq restart");
    check(d.last_seq==0 && d.resets>=1,"boot reset state");
    printf("PASS reference-model dialogue sweep (not exact rx_consumer_task): HELLO/DATA/ACK, lost DATA/ACK, duplicate, durable-full, CRC/version, reset\n");
    return 0;
}
