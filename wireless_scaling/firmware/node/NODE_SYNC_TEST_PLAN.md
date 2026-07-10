# Node sync test plan

## Cel

A node-ok elesben 20 percenkent mernek (`NODE_WAKE_PERIOD_SEC=1200`), de a master ACK-ban kapott
`next_slot_offset_s` alapjan slotoltan ebrednek vissza. Tesztben ugyanez a logika 60 masodperces
periodussal fut, hogy gyorsan lathato legyen a szinkron es az edgecase viselkedes.

## Build/env

- Master teszt: `firmware/master`, env `esp32-s3-zero-cloud-test`
  - ESPNOW channel: `bee_test.espnow_channel`
  - node period: 60 s
  - slot spacing: 5 s
  - Firebase target: `experiments/sz6_overnight`, csak node upload
- Node teszt: `firmware/node`, env `node1-test`, `node2-test`, `node3-test`
  - ESPNOW channel: `bee_test.espnow_channel`
  - wake period: 60 s
  - fallback jitter: 0..5 s
- Node prod: `node1`, `node2`, `node3`
  - wake period: 1200 s
  - fallback jitter: 0..30 s
  - ACK slot elfogadas: 5..1500 s

## Varhato logika

1. A node minden ebredeskor mer, majd a mintat elobb flash/NVS pending queue-ba menti.
2. A radio csak a flash mentes utan indul.
3. A node a pending queue-t legregebbitol kuldi, maximum `NODE_DRAIN_MAX_RECORDS` rekordig.
4. Sikeres master ACK utan a node torli a `clear_buffer_until_seq`-ig atvett rekordokat.
5. Ha az ACK `next_slot_offset_s` ertelmes, a node ezzel alszik vissza.
6. Ha nincs ACK, vagy a slot irrealis, a node `NODE_WAKE_PERIOD_SEC + jitter` fallback alvassal megy tovabb.

## Szinkron tesztek

### 1. Mindharom node egyszerre indul

Elvart:
- Az elso korben lehet utkozes vagy retry.
- Sikeres ACK utan a kovetkezo korok szetcsusznak:
  - node1 kb. 5 s fazis
  - node2 kb. 10 s fazis
  - node3 kb. 15 s fazis
- Master logban: `ACK node=<id> seq=<seq> next_slot_s=<...>`.
- Node logban: `DRAIN summary ... source=ACK_SLOT`.

### 2. Egy node hianyzik vagy ki van kapcsolva

Elvart:
- A tobbi node tovabbra is kap ACK-ot es slotot.
- Nincs globalis barrier, tehat egy kiesett node nem allitja meg a tobbit.
- A hianyzo node visszakapcsolas utan sajat pending queue-bol folytatja.

### 3. Master offline

Elvart:
- Node-ok mernek es mentenek NVS pending queue-ba.
- `ACK FAIL` utan fallback sleep jon, jitterrel.
- A fallback jitter miatt nem ragadnak tartosan ugyanabba az utkozo fazisba.
- A pending queue kapacitas 512 rekord, ami 20 perces prod periodusnal kb. 7 nap.

### 4. Master visszajon offline utan

Elvart:
- A node legregebbi pending rekorddal kezd.
- Egy ebredesben legfeljebb 32 rekordot drainel.
- ACK utan `clear_buffer_until_seq` torli az atvett tartomanyt.
- Firebase oldalon a rekordkulcs boot/seq alapu, tehat ujrakuldesnel idempotens PUT-tal ir.

### 5. Node regi master MAC-et ismer

Elvart:
- Elso probak unicasttal mehetnek a regi MAC-re.
- A retry logika broadcast fallbackre valt.
- Sikeres ACK-bol ujra megtanulja a master MAC-et.

### 6. Node reset / uj boot_id

Elvart:
- Master node state boot_id alapjan uj sorozatkent kezeli.
- A seq dedup/gap logika nem keveri ossze az elozo boot ciklussal.
- Firebase kulcsban a boot_id is benne van.

### 7. Szenzorhiba

Elvart:
- HX711/DS18B20 hiba eseten a node tovabbra is payloadot kuld, a logban ERR/NAN latszik.
- Hive2 prod feltoltes node oldalrol csak ervenyes HX711 adattal keszit webappra szant rod rekordot.
- Teszt/diagnosztikai logban a hibas szenzorallapot lathato.

### 8. Channel mismatch

Elvart:
- Ha master es node mas ESPNOW csatornan van, nincs ACK.
- Node pending queue no, fallback sleep fut.
- Tesztnel mindket oldalon `bee_test.espnow_channel` az igazsagforras; prod default ch6.

### 9. Pending queue megtelik

Elvart jelenlegi kod szerint:
- Ha 512 rekord utan sem tud feltolteni, az uj minta mentese hibaval leall arra a ciklusra.
- Ez adatvesztesi kockazat hosszu master/radio kiesesnel.
- Elesites elott dontendo, hogy jobb-e drop-oldest strategia az uj minta megtartasara.

## Minimum elfogadas

- Mindharom teszt node legalabb 5 egymast koveto ciklusban ACK_SLOT-tal alszik vissza.
- Master logban latszik mindharom node ACK-ja es nincs tartos `gap`.
- Egy node kikapcsolasa utan a masik ketto tovabbra is jon.
- Master 2-3 perces kikapcsolasa utan a node-ok backlogot kuldenek, majd visszaallnak slotolt mukodesre.
- Firebase teszt agon a node rekordok tovabbra is felkerulnek.
