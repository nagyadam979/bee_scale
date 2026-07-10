/*
 * protocol.h — a node <-> master KÖZÖS wire-szerződés (befagyasztott kontraktus)
 * bee_scale / wireless_scaling · 2. fázis · firmware/common
 *
 * EREDET / STÁTUSZ
 *   Ez a header az experiments/Sz2_espnow_low_level/common/sz2_protocol.h ÉLES átvétele —
 *   a mező-layout BIT-AZONOS a kísérleti (Sz2/M1) forrással, hogy a padon 1000/1000-re
 *   validált vétel és a majdani master ugyanazt a struktúrát lássa. A típusnevek (sz2_*)
 *   és a SZ2_* makrók szándékosan VÁLTOZATLANOK: a wire-formátum befagyasztott. Ha valaha
 *   mezőt kell változtatni, a protocol_version-t emeljük, és MINDKÉT oldalt együtt frissítjük.
 *
 * MIÉRT packed + fixed-size
 *   Nincs string/parse: a node `memcpy`-vel a wire-re teszi, a master onnan olvassa.
 *   `__attribute__((packed))` -> nincs fordítófüggő padding, méret- és offset-azonos.
 *   Mindkét cél little-endian (C3 node RISC-V, S3 master Xtensa), uint32_t/float 4 byte
 *   -> bit-azonos a dróton. Encrypt nélküli ESP-NOW payload max 250 byte; ez bőven alatta.
 *
 * CRC
 *   A `crc` mező CRC16-CCITT (poly 0x1021, init 0xFFFF) a struct ÖSSZES byte-ján a `crc`-t
 *   KIVÉVE (sizeof - sizeof(uint16_t)). A node a küldés előtt számolja, a master vételkor
 *   ellenőrzi. Helper: sz2_payload_crc() lent.
 *
 * VERZIÓ
 *   A protocol_version mindkét csomag első byte-ja -> a master eldobhatja az ismeretlen
 *   verziójú csomagot. Aktuális: SZ2_PROTO_VERSION = 1.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#define SZ2_STATIC_ASSERT static_assert
#else
#define SZ2_STATIC_ASSERT _Static_assert
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A protokoll verziója — mindkét oldal ezt írja/várja. */
#define SZ2_PROTO_VERSION   1

/* packet_type értékek. Az ACK az Sz0-csíra, a tűzz-és-felejts node-ban NEM használt. */
#define SZ2_PKT_DATA        1   /* node -> master: mérés-payload (sz2_payload_t)   */
#define SZ2_PKT_ACK         2   /* master -> node: visszaigazolás (sz2_ack_t), Sz3 */

/*
 * Mérés-payload: node -> master. PACKED, fixed-size.
 *
 * A docs "Adatformátum" mezői (node_id=unit_id, seq, weight, temp, vbat) itt megvannak.
 * A többi mező provisioning / diagnosztika:
 *   - node_mac[6]      : a node SAJÁT MAC-je. A master ebből + a recv src_addr-ból tanulja
 *                        a node_id <-> MAC párost.
 *   - boot_id          : bootonként változó azonosító (esp_random). A master ebből látja a
 *                        node-resetet -> a seq-számlálás újraindul, nem hiány/dup.
 *   - packet_idx /
 *     total_packets    : futás-index / futás-hossz (a node-ban: packet_idx == seq, a
 *                        total_packets a diagnosztika/BENCH-futás jelölésére szabad).
 *   - send_timestamp_ms: a node ms-órája küldéskor (átviteli idő / sorrend diagnosztika).
 *   - crc              : CRC16-CCITT az ELŐZŐ byte-okon. MINDIG az utolsó mező.
 */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;  /* = SZ2_PROTO_VERSION                                  */
    uint8_t  packet_type;       /* = SZ2_PKT_DATA                                       */
    uint8_t  node_id;           /* docs: unit_id — melyik node/kaptár                   */
    uint8_t  node_mac[6];       /* a node saját STA MAC-je (provisioning)               */
    uint32_t boot_id;           /* bootonkent valtozo (reset-detektalas a masteren)     */
    uint32_t seq;               /* szekvencia — hianyzo/dup csomag detektalas           */
    uint32_t packet_idx;        /* futason beluli index (node: == seq)                  */
    uint32_t total_packets;     /* futas-hossz / diagnosztika (node: szabad)            */
    uint32_t send_timestamp_ms; /* node ms-ora kuldeskor                                */
    float    weight;            /* docs: nyers/kalibralt terheles; NAN = HX711 hiba     */
    float    temp;              /* docs: homerseklet; NAN = nincs/hibas DS18B20         */
    uint16_t vbat;              /* docs: elemfeszultseg mV-ban; 0 = nincs/tiltott VBAT  */
    uint16_t crc;               /* CRC16-CCITT az osszes elozo byte-on — UTOLSO mezo    */
} sz2_payload_t;

/*
 * ACK: master -> node. PACKED, fixed-size. Sz0-CSÍRA — a tűzz-és-felejts node-ban NEM
 * használt, de a layout előre definiálva, hogy a header verzió-stabil legyen (Sz3).
 */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;        /* = SZ2_PROTO_VERSION                            */
    uint8_t  packet_type;             /* = SZ2_PKT_ACK                                  */
    uint8_t  node_id;                 /* melyik node-nak szol                           */
    uint32_t ack_last_seq;            /* az utolso hibatlanul vett seq                  */
    uint32_t clear_buffer_until_seq;  /* a node eddig torolheti a buffert               */
    int32_t  clock_correction_s;      /* ora-korrekcio masodpercben (elojeles)          */
    uint32_t next_slot_offset_s;      /* a kovetkezo ebredesi slot eltolasa             */
    uint8_t  retry_policy;            /* retransmit-stategia (Sz3 donti el)             */
} sz2_ack_t;

SZ2_STATIC_ASSERT(sizeof(sz2_payload_t) == 41, "sz2_payload_t wire meret valtozott");
SZ2_STATIC_ASSERT(sizeof(sz2_ack_t) == 20, "sz2_ack_t wire meret valtozott");

/*
 * CRC16-CCITT (poly 0x1021, init 0xFFFF, nincs reflektalas, nincs xorout). Inline, hogy
 * mindkét oldal pontosan ugyanazt számolja, külön .c nélkül.
 */
static inline uint16_t sz2_crc16(const uint8_t *p, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/*
 * A payload CRC-je: az ÖSSZES byte a `crc` mezőt KIVÉVE (az utolsó 2 byte nélkül).
 * A node ezt írja p->crc-be küldés előtt; a master ezzel veti össze a vett p->crc-t.
 */
static inline uint16_t sz2_payload_crc(const sz2_payload_t *p)
{
    return sz2_crc16((const uint8_t *)p, sizeof(sz2_payload_t) - sizeof(uint16_t));
}

#ifdef __cplusplus
}
#endif

#undef SZ2_STATIC_ASSERT

#endif /* PROTOCOL_H */
