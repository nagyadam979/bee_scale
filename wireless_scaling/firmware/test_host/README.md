# Host-oldali tartóssági és rádiós szimulációk

Utolsó ellenőrzés: 2026-07-11. Minden host bináris Clang ASan+UBSan alatt fordul.

## `node_queue_sim`

A harness a változatlan `firmware/node/main/node_buffer.c` fájlt fordítja. Minden boot külön
`fork()`, ezért a produkciós statikus RAM-állapot elveszik, a megosztott mock NVS megmarad.
Az orákulum adatvesztést, ACK-olt rekord visszatérését, sorrend-/tartalomhibát, orphan-adoptot,
kapacitást és a 32 bites seq wrapot ellenőrzi.

```sh
cd firmware/test_host/node_queue_sim
make clean && make && make run
```

Determinista egyedi reprodukció:

```sh
./fuzz --seed 11 --boots 6000 --base 0xFFFFFF00
```

A teljes suite seedjei: `1, 2, 3` (`base=0`) és `11, 12`
(`base=0xFFFFFF00`), seedenként 6000 boot. A korábbi félbehagyott harness egyetlen saját
buildhibája a `g_sim_log_enabled` hiányzó `extern` deklarációja volt; a produkciós forrás nem
változott.

2026-07-11-i teljes sanitizeres eredmény: `PASS`, 30 000 boot, 7014 injektált cut,
793 938 mock NVS API-hívás, 52 696 sikeres enqueue és 28 860 sikeres clear; invariánssértés,
ASan- vagy UBSan-hiba nincs. A `11` és `12` seed futása közben a seq átfordult
`0xFFFFFFFF -> 0` értéken.

## `master_ring_sim`

A build script 15 kijelölt függvénytestet közvetlenül, változtatás nélkül emel ki a
`firmware/master/main/master.c` fájlból. A host és produkciós rekord-layout egyezését
`_Static_assert` védi. A modell:

- 2 490 368 byte-os NOR backup partíció, 4096 byte sector, 256 byte slot;
- csak `1 -> 0` programozás, részleges/torn write és erase;
- NVS commit áramszünetnél atomi régi-vagy-új állapot;
- rebootonként elvesző firmware RAM `fork()` segítségével;
- injektálható átmeneti partition-read hiba.

```sh
cd firmware/test_host/master_ring_sim
make clean && make run
```

Ellenőrzött invariánsok és 2026-07-11-i eredmény:

- `PASS` 9727 rekordos teljes kapacitás, full-refusal, teljes drain;
- `PASS` wrap és sector-boundary stale-sector erase, NOR/OOB/alignment sértés nélkül;
- `PASS` ismételt peek, átmeneti peek-read hiba tail-mozgatás nélkül;
- `PASS` teljes, de még nem indexelt slot orphan-adoptja és node-retry idempotencia;
- `PASS` pop közbeni NVS-cut régi-vagy-új atomi kimenetele;
- `PASS` 28 elemű bounded cut-position × torn-seed mátrix:
  16 enqueue eset (slot-write és index-commit, 8-8 seed), 8 pop-commit seed, valamint
  4 valódi stale-sector boundary erase seed pending előző szektor mellett. Minden eset rebootot,
  countot és teljes sorrendi draint ellenőriz; a korábbi rekord változatlan, az incoming pontosan
  egyszer jelenik meg a helyreállítás után, popnál csak a régi front vagy közvetlen utód jöhet;
- `PASS` master seq `1`, reboot után `2`, az indexből visszatöltve;
- `REPRO` a javítás előtti kóddal: egy normál teljes ring-lap után a `state2` elvesztése
  újra `seq=1` értéket adott, tehát a régi `master_1` cloud-kulcs ütközött;
- `PASS` a javítás után: régi `boot_id=0` backlog kulcsa kompatibilisen `master_1`, az új
  rekord boot-scoped kulcsa például `master_B00DAE62_1`, így nincs felülírás.

Korlátok: a NOR torn művelet prefixet ír/töröl, nem tetszőleges bites mintát; az NVS
wear-leveling belső oldalstruktúráját nem modellezi; párhuzamos task-interleaving nincs, a backup
mutex által sorosított műveleteket vizsgálja.

## `radio_dialogue_sim`

```sh
cd firmware/test_host/radio_dialogue_sim
make clean && make run
```

Két külön bizonyítási szint van:

1. Az `exact` tesztek buildkor közvetlenül kiemelik a node produkciós `recv_cb` ACK-szűrőjét
   és a master produkciós `node_get` registry-függvényét.
2. A lost DATA/ACK, durable-full és reset eseménysor egy kézzel írt determinisztikus
   referencia-modell. Ez a kívánt protokoll-orákulumot ellenőrzi, de nem a teljes
   `rx_consumer_task` produkciós kódútvonala.

Eredmények:

- `PASS exact`: hibás hossz/verzió/típus/node/seq ACK eldobása;
- `OBSERVED exact`: más forrás-MAC-ről érkező, node+seq szerint egyező ACK elfogadható és az
  idegen MAC megtanulható;
- `OBSERVED exact`: bootváltás utáni seq-újrahasználatnál egy régi, késleltetett ACK a jelenlegi
  20 byte-os ACK wire-formátumból nem különböztethető meg;
- `PASS exact`: 16 registry-bejegyzés után a 17. `NULL` eredménnyel a no-ACK ágra kerül;
- `OBSERVED exact`: azonos `node_id`, eltérő source MAC a meglévő registry-elemre aliasol;
- `PASS model`: HELLO, DATA, duplicate, elveszett DATA/ACK, durable-full/no-ACK,
  CRC/verzió-eldobás és boot_id reset forgatókönyvek.

## Produkciós változás

A master NVS-loss reprodukciója után a helyi master rekordok indulásonként véletlen
`boot_id`-t kapnak, és minden master cloud séma a `master_<boot_id>_<seq>` kulcsot használja.
A régi flash backlog `boot_id=0` rekordjai megtartják a legacy `master_<seq>` kulcsot, ezért a
korábban feltöltött backlog retry idempotenciája nem változik.

Firmware build ellenőrzés:

```sh
cd firmware/master
~/.platformio/penv/bin/pio run -e esp32-s3-zero-cloud
```

Eredmény: mind a 7 env `SUCCESS`:

- `esp32-s3-zero`
- `esp32-s3-zero-local-status`
- `esp32-s3-zero-test`
- `esp32-s3-zero-local-status-test`
- `esp32-s3-zero-test-scan`
- `esp32-s3-zero-cloud`
- `esp32-s3-zero-cloud-test`

A repóban már a munkamenet előtt is voltak felhasználói web-status módosítások a
`master.c`, `platformio.ini`, `CMakeLists.txt`, `firmware/master/README.md`, `.gitignore` és
`wifi_config.example.h` körül. A szimulációs munka ezeket nem módosította vagy vonta vissza;
a produkciós változás kizárólag a reprodukált master cloud-kulcs ütközés javítása.
