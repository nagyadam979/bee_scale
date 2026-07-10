# Firmware handoff - 2026-07-10

Ez a dokumentum a `dev_02` agon, a `d99e0ed` (`firmware: add node sync handoff and
hive2 cloud schema`) commit utan keszult node es master megbizhatosagi javitasokat irja le.
A cel az, hogy egy kovetkezo agent a kod, a flash-allapot es az uzemi invariansok ismereteben
tudja folytatni a munkat.

## Miert kellett javitani

Az audit az alabbi valos adatvesztesi es kompatibilitasi kockazatokat talalta:

- A master a node DATA rekordjat a tartos flash-mentes elott ACK-olhatta. Aramszunetnel a node
  mar torolhette a sajat peldanyat, mikozben a master RAM queue-ja elveszett.
- A master flash ring megteleskor vagy szektorujrahasznalatkor olvasatlan rekordokat dobhatott.
- A flash slot rekordmerete a `d99e0ed` commitban 82-rol 86 byte-ra nott a
  `rain_mm_delta` mezovel, de a formatum magic nem valtozott. A regi backup dekodolasa emiatt
  nem volt egyertelmu.
- A ring `head`/`tail`/`write_seq`, a master meresi szekvencia es az eso jelentett kurzora
  kulon NVS muveletekben elt el. Aramszuneti ablakban rekordismetles, rain delta ismetles vagy
  mar megirt, de nem indexelt NOR slot maradhatott.
- Felhobe nem kuldheto, ervenytelen szenzorrekord vegtelen retryval a queue elejen tarthatta a
  mogotte levo jo rekordokat.
- A node cold boot HELLO elott inditotta a radiot, ezert HELLO-hibanal az adott ebredes merese
  elmaradhatott. A backlog rekordok idobelyege kuldesi/atveteli ido volt, nem meresi ido.
- A node 64 KiB-os `nodebuf` NVS particioja nem adott biztonsagos helyet az elvart 512
  rekordnak.
- A master eso debounce `uint32_t` millisekundumos idobazisa kb. 49,7 nap utan atfordult, a
  rain delta kurzor pedig a tartos rekord elkeszulte elott elorelepett.
- A master DS18B20 olvasas nem szurte az all-`0x00`, all-`0xFF` es tartomanyon kivuli
  scratchpadot, es hibanal nem keresett ujra eszkozoket.

Fontos helyszini teny: **csak a node1 VBAT merese van fizikailag lekapcsolva**. A node2 es
node3 VBAT aramkore mukodik, ezert azok env-jeiben `NODE_VBAT_ENABLED=1` maradt. A hive2
rekordok a felhoben tenylegesen jelen vannak; a javitas nem valtoztatja meg a hive2 RTDB
semat.

## Erintett fajlok

- `firmware/common/protocol.h`
- `firmware/node/main/main.c`
- `firmware/node/main/espnow_tx.c`, `espnow_tx.h`
- `firmware/node/main/node_sleep.c`, `node_sleep.h`
- `firmware/node/main/node_buffer.c`
- `firmware/node/main/node_config.h`
- `firmware/node/partitions.csv`
- `firmware/node/NODE_SYNC_TEST_PLAN.md`
- `firmware/master/main/master.c`

## Wire protokoll es ido

A wire verzio es a packed meretek **nem valtoztak**:

- `SZ2_PROTO_VERSION = 1`
- `sizeof(sz2_payload_t) = 41`
- `sizeof(sz2_ack_t) = 20`
- `sizeof(sz2_hello_t) = 33`

Ket legacy mezonev uj, kompatibilis jelentest kapott:

- `sz2_payload_t.send_timestamp_ms`: a nev ellenere a **meres Unix epoch ideje
  masodpercben**; `0`, ha a node meg nem kapott ervenyes master-idot.
- `sz2_ack_t.clock_correction_s`: a nev ellenere a master aktualis Unix epoch masodperce;
  `0`, ha az SNTP ido meg nem ervenyes.

A node az ACK epochot RTC memoriaban tartja, es az aktiv ido, valamint a kert deep-sleep ido
alapjan becsuli a kovetkezo timer-wake meresi idot. Cold resetnel az RTC idohorgony ervenytelen,
ezert az elso minta idobelyege szandekosan `0`. Az elso ervenyes HELLO vagy DATA ACK utan a
kovetkezo timer-wake minta mar becsult epochot kap. A master csak
`send_timestamp_ms >= 1700000000` eseten hasznalja a node idejet; egyebkent a veteli epochra
esik vissza.

Megjegyzes: a wire mezo `int32_t`, ezert az epoch atadas 2038-ig ervenyes. Ez jelenleg nem
blokkolja a telepitest, de egy kesobbi wire-verzioban javitando.

## Node adatfolyam es invariansok

FIELD profilban minden ebredes sorrendje:

1. RTC wake-allapot es becsult epoch felvetele.
2. Szenzormeres WiFi/radio nelkul: HX711, DS18B20, VBAT.
3. Payload eloallitasa es tartos pending queue commit.
4. HX711 explicit power-down/hold.
5. WiFi es ESP-NOW inicializalas.
6. Cold boot eseten HELLO/szinkron.
7. Pending rekordok oldest-first kuldese, ebredesenkent legfeljebb 32 rekord.
8. Rekordtorles csak exact DATA ACK utan: `ack_last_seq == pending.seq` es
   `clear_buffer_until_seq == pending.seq` egyarant kotelezo.
9. Radio deinit, VBAT sleep-elokeszites, ujabb explicit HX711 power-down, deep sleep.

Fo invariansok:

- A meres es a helyi tartos commit mindig a radio bekapcsolasa elott tortenik.
- Cold HELLO hiba nem veszti el az aktualis mintat: az mar a queue-ban van, a node 30
  masodperces rovid retry sleepbe megy.
- DATA ACK hianya eseten a rekord a queue-ban marad; a node periodus + jitter fallbackkel alszik.
- Az elso DATA probalkozas ACK timeoutja 2000 ms, mert a masternek NOR erase/write/readback es
  NVS commit is lehet az ACK elott. A tovabbi DATA probalkozasok es a HELLO timeoutja 120 ms.
- A kuldesi retry csak a `packet_idx` mezot valtoztatja. A meresi timestamp minden retrynal
  valtozatlan.
- A master MAC tanulas es unicast gyorsut megmaradt; sikertelen unicast utan broadcast fallback
  kovetkezik.
- ACK contract mismatch eseten a drain azonnal leall, nincs epoch sync es nincs helyi torles.
  Ez megakadalyozza, hogy hibas vagy masik tranzakciohoz tartozo ACK elore torolje a queue-t.
- Node1: VBAT tiltva. Node2/node3: VBAT engedelyezve. A TX power beallitas nem valtozott.

### Node pending queue es aramszuneti helyreallas

A `nodebuf` tovabbra is ESP-IDF NVS rekordblobokat hasznal. A rekordkulcs
`r%08X` (9 karakter), az aktiv logikai tartomany `[head, next)`. A packed, 22 byte-os
`queue/state2` blob mezoi:

- magic `0x3246424E` (`NBF2`), version `2`, sajat `size`;
- `head`, `next`, `count`;
- CRC16 a CRC mezot megelozo teljes blobon.

Tartos invarians: modulo `uint32_t` aritmetikaval `next - head == count`, es
`count <= NODE_PENDING_CAPACITY == 512`. A queue a sequence wrap kornyeken is a fel
`uint32_t` tartomanynal joval kisebb, ezert a signed-difference osszehasonlitas ervenyes. Minden
beolvasott rekordnal ellenorzodik a tarolt meret, wire protokollverzio, packet type, sajat `seq`
es payload CRC. A RAM `head/next/count` csak sikeres `state2` commit utan lephet.

Enqueue sorrend:

1. A `r<seq>` payload NVS set + commit.
2. Az uj `state2` set + commit.
3. Csak ezutan a RAM state frissitese.

Ha az 1. es 2. commit kozott megy el a tap, pontosan a regi `next` kulcson marad egy orphan.
Initkor a firmware validalja a payloadot, es ha ervenyes, state commit mellett adoptalja. Ugyanez
a seq nem irodik felul. Ervenytelen expected orphan torolheto; I/O hiba fail-closed.

Exact ACK utani clear sorrend:

1. Az ACK altal lefedett front rekordkulcs(ok) erase + commit.
2. Az uj `state2` set + commit.
3. Csak ezutan a RAM state frissitese.

Ha az erase es a state commit kozott megy el a tap, a regi state hianyzo frontra mutat. Az init
vagy a kovetkezo peek `FRONT_REPAIR` utja a bizonyitottan hianyzo/ervenytelen frontot
erase-first, majd state-commit sorrendben lepteti at. A repair ujabb aramszunet utan idempotensen
ujrafuthat. Egy CRC-hibas payload mar nem visszaallithato, ezt explicit corrupt log mellett
lepteti at; altalanos I/O hibat nem kezel adatkorruptiokent.

Legacy node meta migracio csak akkor indul, ha `state2` tenylegesen nem letezik:

1. Beolvassa a regi kulon `head`, `next`, `count` kulcsokat, es az endpointokbol ellenorzi vagy
   ujraszamolja a konzisztens allapotot.
2. Ha ez nem eleg, vegigolvassa a valid `r%08X` rekordkulcsokat, es legfeljebb 512-es spanbol
   rekonstrual. A span belso lyukait kesobb a front repair kezeli.
3. Eloszor commitolja az uj `state2` blobot, es csak ezutan takaritja a legacy meta kulcsokat.

Ambiguous, 512-nel nagyobb span, NVS I/O hiba vagy hibas meretu/CRC-ju letezo `state2` eseten
init hibat ad, a particiot es rekordokat erintetlenul hagyja. Korrupt `state2`-rol szandekosan
nincs legacy fallback, es `ESP_ERR_NVS_NO_FREE_PAGES`/formatumhiba eseten sincs automatikus
particioerase. Ez adatmentesi szempontbol fail-closed viselkedes.

Az uj 80 KiB-os particio logical capacity-je 512 rekord. Tele queue-nal az uj minta nem irodik
felul es nincs drop-oldest; az adott ebredes enqueue hibaval zarul. 20 perces production
periodusnal 512 rekord kb. 7,1 nap backlog.

## Master adatfolyam es ACK-invarians

Cloud buildben a flash backup ring az egyetlen cloud queue; a korabbi RAM cloud queue
megszunt. Egy uj node DATA feldolgozasa:

1. Wire hossz, protokollverzio es CRC ellenorzese.
2. `boot_id` + `seq` alapu uj/duplikalt besorolas.
3. Uj rekordnal `master_record_t` eloallitasa.
4. Tartos backup slot iras, teljes readback-ellenorzes, majd CRC-zett `state2` index commit.
5. Csak sikeres tartos commit utan frissul a master dedup allapota es csak ezutan megy DATA ACK.

Ha a tartos mentes hibazik vagy a ring tele van, a master `RX_NOT_ACKED`-et logol, nem lepteti a
node dedup allapotat es nem ACK-ol. A node ugyanazt a rekordot kesobb ujrakuldi. Egy mar tartosan
elfogadott rekord retransmitje duplikatum, ez azonnal ujra ACK-olhato.

Nem-cloud diagnosztikai buildben nincs felho durability kovetelmeny: a rekord
`RECORD_STORE_SKIPPED` eredmennyel elfogadott. Az eles adatmegorzesre az
`esp32-s3-zero-cloud` env valo.

### Master backup ring

- Particio: `backup`, offset `0x190000`, meret `0x260000` (2432 KiB).
- Slotmeret: 256 byte; 9728 fizikai slot, ring miatt legfeljebb 9727 pending rekord.
- Szektormeret: 4096 byte, 16 slot/szektor.
- Az uj slot teljes 256 byte-ja kiirasra kerul, majd byte-pontos readback ellenorzes tortenik.
- A ring nem dob oldest rekordot. Tele ringnel vagy olvasatlan tail szektort erinto erase-nel az
  uj rekordot visszautasitja, igy a node nem kap ACK-ot.
- A cloud task mindig a tartos ring tailjet olvassa. Sikeres feltoltes vagy terminalis SKIP utan
  CRC-zett index commit lepteti a tailt.

Az uj `state2` NVS blob egyetlen CRC-zett allapottranzakcioban tartja:

- `magic`, `version`
- `head`, `tail`, `write_seq`
- `master_seq`
- `rain_reported`
- `crc32`

Master meresi rekordnal a slot, a kovetkezo master sequence es a tartos rain kurzor egyetlen
index-commitban valik lathatova. A rain delta csak a meg tartosan nem jelentett tippekbol keszul;
ha az enqueue hibazik, a kurzor nem lep elore. A rain debounce mar 64 bites monotonic
millisekundumot hasznal, igy nincs 49,7 napos wrap.

### Power-loss recovery

A NOR slot irasa es az NVS index commit kozott szandekosan van recoverable ablak:

- Ha a `head` slot ervenyes CRC-ju es `write_seq == state.write_seq + 1`, akkor ez egy teljesen
  kiirt, de meg nem indexelt orphan slot. A kovetkezo enqueue adoptalja, elorelepteti a headet es
  commitolja a state-et.
- Node rekordnal az azonos `type + unit_id + boot_id + seq` incoming rekord mar tartosnak
  minosul, ezert nem keszul masodik peldany.
- Master rekord orphan recoveryje a slotbol helyreallitja a `master_seq` es `rain_reported`
  kurzort is.
- Reszlegesen irt vagy varatlan slotot a firmware nem ir felul. Ha van pending backlog, fail
  closed modon leallitja az uj irast. Csak ures ringnel torolheti az erintett serult szektort.
- Ervenytelen tail slotot `backup_bad` diagnosztika mellett csak sikeres state commit utan lep at.

## Backup formatumok es migracio

### Rekord slot

1. **V1, `d99e0ed` elotti:** magic `0x42534D31`, 82 byte-os `master_record_v1_t`; nincs
   `rain_mm_delta` float.
2. **d99 V1:** ugyanaz a magic, de 86 byte-os rekord a `rain_mm_delta` mezovel. Ez volt a
   kompatibilitasi hiba forrasa.
3. **V2:** magic `0x42534D32`, `format_version=2`, 86 byte-os rekord.

A decoder eloszor a V2 magic/verzio/86-byte CRC-t ellenorzi. V1 magicnel kulon probalja a d99
86-byte CRC-t, majd a pre-d99 82-byte CRC-t. Pre-d99 rekordnal a hianyzo
`rain_mm_delta = rain_tips_delta * 0.2794` ertekkel kepzodik. A slotokat olvasaskor migralja a
memoriabeli rekordra; a flashen nem irja at oket helyben.

### Ring metadata

Bootkor az olvasasi sorrend:

1. ervenyes 32 byte-os, magic/version/CRC-vel vedett `backup/state2`;
2. korabbi 16 byte-os CRC-zett `state2` (`head`, `tail`, `write_seq`, CRC), majd azonnali
   migracio az uj formatumra;
3. ha nincs `state2`, a regi kulon `backup/head`, `backup/tail`, `backup/wseq` kulcsok, majd
   migracio az uj blobba.

A legacy `master/seq`, `rain/tips` es `rain/reported` ertekek a migracio bemenetei. Ha a
`rain/reported` meg nem letezik, elso indulaskor az aktualis `rain/tips` ertek lesz a baseline,
igy regi firmware utan nem gyart visszamenoleges, ismeretlen intervallumu eso delta rekordot.
Ervenytelen metadata vagy sikertelen migracios commit eseten a tartos backlog letilt, nem
nullazza es nem irja felul automatikusan a flash tartalmat.

## Cloud eredmenyek es Hive2 kompatibilitas

A cloud feltoltes harom terminalis/allapot eredmenyt hasznal:

- `CLOUD_RESULT_OK`: az osszes eloirt PUT sikerult; a tail poppolhato.
- `CLOUD_RESULT_SKIP`: a rekord a cel schema szerint terminalisan nem feltoltheto vagy az adott
  env szandekosan nem tolti fel; a tail poppolhato, hogy ne blokkolja a sort.
- `CLOUD_RESULT_RETRY`: nincs IP/token, HTTP/auth/network hiba vagy atmeneti JSON/path eloallitasi
  hiba; a rekord a flash ringben marad.

Hive2 prod node rekord csak ervenyes HX711 adattal toltheto fel. Az experiment cloud-test agon a
node rekordhoz ervenyes HX711 es DS18B20 kell, master rekord feltoltes tiltott. Az ervenytelen
szenzorrekord ettol meg tartosan atveheto a nodetol es diagnosztizalhato, majd SKIP-pel kikerul a
cloud queue-bol.

**A Hive2 RTDB schema valtozatlan.** Kulonosen az env rekord tovabbra is
`ts` + `rain_mm`, valamint opcionlis `air_t_c`, `air_rh`, `bmp_t_c`, `pressure_pa` mezoket ir.
Nem kerult bele `rain_tips_total`, mert a tracked `firebase_hive2_rules.md` az ismeretlen mezot
`$other.validate=false` szaballyal elutasitana. A meglevo hive2 cloud rekordokat es szabalyokat
nem kell migralni.

## DS18B20 javitas es ismert korlat

A master minden meresi ciklusban ujrakeres, ha nem a vart ket eszkozt latja. Olvasasi hiba utan
egyszer ujrakeres es ujraprobal. Ervenytelen az all-`0x00`, all-`0xFF`, hibas CRC-ju vagy
`[-55, 125]` Celsius tartomanyon kivuli scratchpad.

**Residual kockazat:** a ket master DS18B20 szerepe tovabbra is a lexikografikusan rendezett
ROM-indexhez kotott: `ds_t[0] -> hive_c`, `ds_t[1] -> scale_c`. A kod `qsort(memcmp ROM)`
rendezest hasznal, ezert ugyanazzal a ket szondaval egy sima rescan nem csereli fel a szerepeket.
Nincs azonban hardcoded ROM -> szerep mapping: szondacsere vagy eltero probe-keszlet
megvaltoztathatja az indexet. Elesites elott a logolt ROM kodokat fizikailag azonositani kell; a
tartos megoldas explicit ROM mapping.

## Particiok es rollback

### Node

`firmware/node/partitions.csv`:

- `nvs`: `0x9000`, 24 KiB
- `phy_init`: `0xF000`, 4 KiB
- `factory`: `0x10000`, 1 MiB
- `nodebuf`: automatikus offset, **80 KiB (`0x14000`)**, korabban 64 KiB

A particiotabla valtozasa miatt a node-okra **teljes PlatformIO upload kell**, amely a
`partitions.bin`-t is irja. App-only `firmware.bin`/OTA frissites nem eleg. A 64 -> 80 KiB
noveles ugyanazon a kezdo offseten megtartja a regi NVS tartalmat, de a visszagorgetes regi,
64 KiB-os particiotablara nem tekintheto biztonsagosnak: az uj firmware altal a felso 16 KiB-ba
irt NVS lapokat a regi layout nem latja. Rollback elott queue drain/export vagy teljes nodebuf
erase es ujrainicializalas kell.

Az elso sikeres uj-firmware boot a legacy `head/next/count` kulcsokat `state2`-re migralja, majd
torli oket. Ez egy masodik rollback-korlat: a regi node firmware nem ismeri a `state2` blobot,
ezert a regi meta kulcsok visszaallitasa nelkul nem tudja felvenni a pending queue fonalat. A
biztonsagos rollback tehat state2 -> legacy konverziot igenyelne; ilyen tool jelenleg nincs.

### Master

`firmware/master/partitions.csv` valtozatlan:

- `factory`: `0x10000`, `0x180000`
- `backup`: `0x190000`, `0x260000`

Az uj master olvassa a regi V1/d99 slotokat es legacy metat. A rollback viszont egyiranyu
kompatibilitasi korlatba utkozik: a regi firmware nem ismeri a V2 magicet es az uj `state2`
allapotot. V2 slot irasa utan regi master firmware-re visszaallni backup drain vagy teljes
`backup` + kapcsolodo NVS allapot torlese nelkul nem biztonsagos.

## Build env-ek es parancsok

Node, `firmware/node`:

```sh
~/.platformio/penv/bin/pio run \
  -e node1 -e node2 -e node3 \
  -e node1-test -e node2-test -e node3-test
```

Master, `firmware/master`:

```sh
~/.platformio/penv/bin/pio run \
  -e esp32-s3-zero \
  -e esp32-s3-zero-test \
  -e esp32-s3-zero-test-scan \
  -e esp32-s3-zero-cloud \
  -e esp32-s3-zero-cloud-test
```

A 2026-07-10-i forrasallapotot a node es master projektekben kulon is leforditottuk:
**6/6 node** es **5/5 master** environment sikeres, warning nelkul. A node-buffer valtozatlanul
nem erinti a master forrasat, ezert a master eredmenyek a vegso tree-re is ervenyesek.

Egy utolso, ket parhuzamos PlatformIO folyamatot hasznalo osszevont rebuild elindult, de a
felhasznalo kerese miatt felbeszakadt. Addig a `node1`, `node1-test`, `node2`, `node2-test`,
valamint az `esp32-s3-zero`, `esp32-s3-zero-test` es `esp32-s3-zero-test-scan` sikerrel vegzett;
a maradek environmenteket nem hiba miatt, hanem megszakitas miatt nem vartuk meg ebben a futasban.
Az elozetes teljes project-build eredmenyek a tablazatban szerepelnek. `git diff --check` tiszta
volt a lezaro commit elott.

| Env | Firmware | Static RAM | Eredmeny |
| --- | ---: | ---: | --- |
| `node1` | 793680 B | 29860 B | SUCCESS |
| `node1-test` | 793670 B | 29860 B | SUCCESS |
| `node2` | 804204 B | 29948 B | SUCCESS |
| `node2-test` | 804194 B | 29948 B | SUCCESS |
| `node3` | 804206 B | 29948 B | SUCCESS |
| `node3-test` | 804196 B | 29948 B | SUCCESS |
| `esp32-s3-zero` | 763945 B | 37312 B | SUCCESS |
| `esp32-s3-zero-test` | 763933 B | 37312 B | SUCCESS |
| `esp32-s3-zero-test-scan` | 769001 B | 37352 B | SUCCESS |
| `esp32-s3-zero-cloud` | 1011001 B | 41072 B | SUCCESS |
| `esp32-s3-zero-cloud-test` | 1009817 B | 41072 B | SUCCESS |

## Flash sorrend

1. Ellenorizd, hogy a production AP primer 2,4 GHz csatornaja 6; tesztben a master es node
   `bee_test.espnow_channel` erteke azonos legyen.
2. Eloszor a **mastert** flash-eld a celhoz illo env-vel. Eles Hive2 uzemhez ez
   `esp32-s3-zero-cloud`; gyors izolalt teszthez `esp32-s3-zero-cloud-test`.
3. Boot logban ellenorizd: backup init/migracio sikeres, nincs
   `durable backlog disabled`, WiFi/ESP-NOW csatorna jo, SNTP epoch ervenyes.
4. Ezutan node-onkent a megfelelo teljes upload: eloszor `node1-test`, `node2-test`,
   `node3-test`, majd hardveres elfogadas utan `node1`, `node2`, `node3`.
5. Minden node-nal ellenorizd, hogy a feltoltott env az eszkoze: node1 VBAT=0,
   node2/node3 VBAT=1. Ne csereld fel az env-eket.
6. Node flash utan ellenorizd a `nodebuf ready state2` vagy `LEGACY migrated to state2` logot,
   egy `pending committed` mintat, HELLO/DATA ACK-ot, majd a `pending cleared` logot.

Pelda teljes uploadra:

```sh
cd firmware/node
~/.platformio/penv/bin/pio run -e node1-test -t upload --upload-port /dev/cu.<PORT>
```

A master es node production/test env-eket ne keverd: a teszt env 60 masodperces periodust es
tesztcsatornat hasznal, a production node 1200 masodpercet es alapertelmezetten ch6-ot.

## Hardveres elfogadasi lista

Forditasi siker onmagaban nem eleg. Flash utan legalabb ezeket kell vegigfuttatni:

- Mindharom test node legalabb 5 egymast koveto 60 masodperces ciklusban mer, helyben commitol,
  ACK-ot kap es `ACK_SLOT` szerint alszik vissza.
- Cold boot elso minta timestampje 0; ervenyes ACK utan a kovetkezo timer-wake minta epochja
  realis es monoton.
- Cold bootkor radio/HELLO csak a meres es queue commit utan jelenik meg a logban.
- Master kikapcsolas: a node-ok tovabb mernek, pending count no; nincs helyi rekordtorles.
- Master visszakapcsolas: oldest-first drain, ebredesenkent legfeljebb 32; ugyanaz a rekord csak
  egyszer jelenik meg a flash/cloud logikai kulcson.
- Aramszunet-injektalas masteren a slot write es state commit kornyeken: reboot utan orphan
  adoptalas vagy fail-closed log, de ACK-olt rekord nem veszhet el.
- Aramszunet-injektalas node-on enqueue es ACK utani clear kornyeken: reboot utan a rekord vagy
  pending marad, vagy mar torolt; ACK elotti adatvesztes nem lehet.
- WiFi/cloud kieses: backup pending no; helyreallas utan sorrendben kiurul. HTTP/auth hiba
  RETRY, terminalis invalid szenzorrekord SKIP, a mogotte levo jo rekord tovabbhalad.
- Ring full szimulacio: uj node rekord nem kap ACK-ot; olvasatlan rekord nem torlodik.
- Rain: tobb billenes ket master minta kozott, reboot a mentes elott/utan; a delta ne vesszen el
  es ne duplazodjon a tartos rekordkulcsokon.
- Node1 VBAT mindig 0/tiltott; node2 es node3 realis mV erteket ad.
- HX711 meres alatt WiFi meg OFF; sleep elott HX711 power-down. Aramfelvetelt muszerrel is merd.
- DS18B20: mindket ROM kod naplozva, fizikai szerep hozzarendelve; lehuzas/visszadugas utan
  rescan es helyes valid mask.
- Hive2: node rod, master rod, env es temp rekord a meglevo schema szerint irhato; env rekordban
  nincs nem engedelyezett uj mezo.
- Channel mismatch negativ teszt: nincs ACK, pending no, majd helyes csatornan backlog drainel.

## Diagnosztikai logok

Kulcs logok, amelyeket a kovetkezo agent keressen:

- Node: `nodebuf ready state2`, `LEGACY migrated to state2`, `ORPHAN adopted`,
  `FRONT_REPAIR`, `pending committed/cleared`, `ACK contract mismatch`, `COLD_SYNC`,
  `ACK OK/FAIL`, `DRAIN summary`, `CLOCK_SYNC`.
- Master RX: `RX ... kind=NEW/DUP`, `RX_NOT_ACKED`, `ACK`, `SYNC_ACK`.
- Master backup: `recovered unindexed backup slot`, `backup ring full`,
  `backup erase boundary reached unread sector`, `backup index ... failed`.
- Cloud: `CLOUD_SKIP_INVALID`, `cloud PUT failed`, `MSTAT ... backup_pending ... backup_full`.
- DS18B20: discovery count es `DS<n> ROM=...`.

## Nyitott, nem blokkolt tovabbfejlesztesek

- Explicit DS18B20 ROM -> `hive_c`/`scale_c` mapping.
- Kovetkezo wire protokollverzioban 64 bites epoch es a legacy mezonevek tisztazasa.
- A test terv 512 rekordos full-queue dontese: jelenleg fail-closed, az uj minta nem irhato; ezt
  csak termekkocel alapjan szabad drop-oldestre valtoztatni.
- Hosszu hardveres soak test valos WiFi kiesessel, NOR erase hataron es 7 napos node backloggal.

## Lezarasi allapot

A valtozasok kodreview-ja es a node/master kulon buildje elkeszult. A node-buffer uj
power-loss allapotgepenek utolso, fuggetlen review-ja es az utolso osszevont rebuild a felhasznalo
keresere felbeszakadt; ez nem forditasi vagy teszthiba. Hardveres flash, power-cut fault injection
es valos felho/ESP-NOW elfogadasi teszt nem futott. A jelen commit ezert kod- es build-validalt,
de hardveren meg nem elfogadott allapotot jelol.
