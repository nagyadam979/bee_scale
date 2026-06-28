/*
 * Sz2 — Közös wire protocol / header (Sz0-csíra)
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz2_espnow_low_level/common
 *
 * MI EZ
 *   A node (TX, ESP32-C3) és a master (RX, classic ESP32) KÖZÖS csomag-definíciója: a
 *   chunkok közti SZERZŐDÉS. Mindkét firmware ezt a headert includolja, így bit-azonos
 *   struktúrát lát. Ez az Sz0 (közös header) minimál-megvalósítása az Sz2 stressz-teszthez,
 *   a stressz-mezőkkel (boot_id, packet_idx, total_packets, send_timestamp_ms) kiegészítve.
 *
 * MIÉRT packed + fixed-size
 *   Nincs string/parse: a payloadot a node `memcpy`-vel a wire-re teszi, a master onnan
 *   olvassa. `__attribute__((packed))` -> nincs fordítófüggő padding, így a két oldal
 *   méret- és offset-azonos. Mindkét cél little-endian (RISC-V C3 + Xtensa classic ESP32),
 *   és a `uint32_t`/`float` 4 byte mindkettőn -> a struct bit-azonos a dróton. (Encrypt
 *   nélküli ESP-NOW payload max 250 byte; ez bőven alatta van.)
 *
 * CRC
 *   A `crc` mező CRC16-CCITT (poly 0x1021, init 0xFFFF) a struct ÖSSZES byte-ján a `crc`
 *   mezőt KIVÉVE, azaz `sizeof(sz2_payload_t) - sizeof(uint16_t)` byte-on. A node a küldés
 *   előtt számolja, a master a vételkor ellenőrzi. Helper: `sz2_payload_crc()` lent.
 *
 * AZ ACK
 *   Az `sz2_ack_t` (master->node) az Sz0-csíra a későbbi app-szintű ACK-hez (Sz3). Itt
 *   ELŐRE DEFINIÁLJUK a mező-layoutot, hogy a header verzió-stabil legyen, de az Sz2-ben
 *   NEM HASZNÁLJUK (nincs visszacsatorna a stressz-tesztben — tűzz-és-felejts broadcast).
 *
 * VERZIÓ
 *   A `protocol_version` mindkét csomag első byte-ja -> a master eldobhatja az ismeretlen
 *   verziójú csomagot. Az Sz2 verziója SZ2_PROTO_VERSION = 1.
 */
#ifndef SZ2_PROTOCOL_H
#define SZ2_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A protokoll verziója — mindkét oldal ezt írja/várja. */
#define SZ2_PROTO_VERSION   1

/* packet_type értékek. Az ACK az Sz0-csíra, az Sz2-ben NEM gyakoroljuk. */
#define SZ2_PKT_DATA        1   /* node -> master: mérés-payload (sz2_payload_t)   */
#define SZ2_PKT_ACK         2   /* master -> node: visszaigazolás (sz2_ack_t), Sz3 */

/*
 * Mérés-payload: node -> master. PACKED, fixed-size.
 *
 * A docs "Adatformátum" tábla (new_base_components.md) mezői: node_id (=unit_id), seq,
 * weight, temp, vbat — ezek itt megvannak. A TÖBBI mező az Sz2 stressz-teszt /
 * provisioning kiegészítése:
 *   - node_mac[6]      : a node SAJÁT MAC-je. Broadcast provisioning: a master ebből +
 *                        a recv-info src_addr-jából TANULJA a node_id <-> MAC párost.
 *   - boot_id          : bootonként változó azonosító (esp_random). A master ebből látja a
 *                        node-resetet -> a seq-számlálás újraindul (0-ról), nem hiány/dup.
 *   - packet_idx /
 *     total_packets    : a stressz-futás indexe és hossza (a master ebből tudja a várt db-ot
 *                        és a hiányzó seq-tartományt). Az Sz2-ben packet_idx == seq.
 *   - send_timestamp_ms: a node ms-órája küldéskor (átviteli idő / sorrend diagnosztika).
 *   - crc              : CRC16-CCITT az ELŐZŐ byte-okon (lásd fent). MINDIG az utolsó mező.
 */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;  /* = SZ2_PROTO_VERSION                                  */
    uint8_t  packet_type;       /* = SZ2_PKT_DATA                                       */
    uint8_t  node_id;           /* docs: unit_id — melyik node/kaptár                   */
    uint8_t  node_mac[6];       /* a node saját STA MAC-je (provisioning)               */
    uint32_t boot_id;           /* bootonkent valtozo (reset-detektalas a masteren)     */
    uint32_t seq;               /* szekvencia — hianyzo/dup csomag detektalas           */
    uint32_t packet_idx;        /* a futason beluli index (Sz2: == seq)                 */
    uint32_t total_packets;     /* a stressz-futas teljes hossza (varhato db)           */
    uint32_t send_timestamp_ms; /* node ms-ora kuldeskor                                */
    float    weight;            /* docs: nyers/kalibralt terheles (Sz2: dummy)          */
    float    temp;              /* docs: homerseklet, drift-korrekcio (Sz2: dummy)      */
    uint16_t vbat;              /* docs: elemfeszultseg mV-ban (Sz2: dummy)             */
    uint16_t crc;               /* CRC16-CCITT az osszes elozo byte-on — UTOLSO mezo    */
} sz2_payload_t;

/*
 * ACK: master -> node. PACKED, fixed-size. Sz0-CSÍRA — az Sz2-ben NEM HASZNÁLT.
 * A mezők az Sz0 README ACK-spec szerint (előre definiálva, hogy a header verzió-stabil).
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

/*
 * CRC16-CCITT (poly 0x1021, init 0xFFFF, nincs reflektalas, nincs xorout). Inline, hogy
 * mindkét oldal pontosan ugyanazt számolja, külön .c nélkül. A két little-endian, azonos
 * int-szélességű arch közt determinisztikus.
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
 * A node ezt írja `p->crc`-be küldés előtt; a master ezzel veti össze a vett `p->crc`-t.
 */
static inline uint16_t sz2_payload_crc(const sz2_payload_t *p)
{
    return sz2_crc16((const uint8_t *)p, sizeof(sz2_payload_t) - sizeof(uint16_t));
}

#ifdef __cplusplus
}
#endif

#endif /* SZ2_PROTOCOL_H */
