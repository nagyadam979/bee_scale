# GPIO9 DS18B20 boot-stabilitas teszt

## Cel

A vegleges node-on a DS18B20 1-Wire homero a GPIO9-re kerult, kb. 5 cm kabellel, es mar nem
atkotheto. A GPIO9 az ESP32-C3-on strapping / BOOT lab, ezert azt kell igazolni, hogy a
szenzor + felhuzo + rovid kabel resetkor es tapra adaskor nem huzza le a labat, es a node
mindig normal firmware-be bootol.

Ez nem homerseklet-pontossagi teszt. Ez boot-megbizhatosagi teszt.

## Elvart bekotes

| Funkcio | Ertek |
|---|---|
| DS18B20 DQ | GPIO9 |
| DQ felhuzo | 4.7 kOhm 3.3 V-ra, fizikailag a node panelhez kozel |
| Kabelhossz | kb. 5 cm |
| Szenzor tap | 3.3 V, nem parasite power |
| GND | kozos node GND |

## Pass / fail

Pass:
- 50/50 tapra adas normal app boot, nincs bootloader/download mod.
- 50/50 reset normal app boot, nincs bootloader/download mod.
- Deep sleepbol 50/50 timer wake normal app boot.
- GPIO9 resetkor nem esik tartosan LOW-ra.
- A DS18B20 bekotve is ugyanugy bootol, mint lehuzott szenzorral.

Fail:
- akar 1 alkalommal download/bootloader modba ragad.
- akar 1 alkalommal nincs app log / nincs ESP-NOW ciklus / csak ROM bootloader latszik.
- GPIO9 tapra adaskor vagy resetkor lassan, bizonytalanul vagy tartosan LOW kozelben marad.

## Meresi setup

1. A vegleges node legyen teljesen osszerakva: HX711, cella, VBAT oszto, DS18B20 GPIO9-en.
2. A DS18B20 kabel es mechanikai elvezetes legyen azonos a vegleges allapottal.
3. A node-on fusson olyan firmware, ami bootkor egyertelmu sort logol, majd deep sleepbe megy.
4. Ha van soros log: rogzitsd a boot sorokat.
5. Ha nincs stabil soros log: a master oldali vett `seq` / ACK / payload log is elfogadhato
   boot-bizonyiteknak.
6. Ha van szkóp vagy logikai analizator: merd GPIO9-et tapra adas es reset kozben.

## Teszt A: tapra adasi boot

1. Akku / tap teljesen le.
2. Varj legalabb 5 s-ot.
3. Add vissza a tapot.
4. Ellenorizd, hogy normal app indul.
5. Jegyezd fel: `PASS` vagy `FAIL`, es ha fail, a tunetet.
6. Ismeteld 50-szer.

Elfogadas: 50/50 PASS.

## Teszt B: reset gomb / EN reset

1. Node normal appban vagy deep sleepben.
2. Nyomj resetet / EN resetet.
3. Ellenorizd, hogy normal app indul.
4. Ismeteld 50-szer.

Elfogadas: 50/50 PASS.

## Teszt C: deep sleep wake

1. Allits rovid, pl. 30 s-os wake periodust tesztprofilban.
2. Hagyd futni legalabb 50 cikluson at.
3. A master logban ellenorizd, hogy minden ciklus utan van normal payload / ACK.
4. Figyeld, hogy nincs olyan ciklus, ahol a node bootloaderben marad es eltunik.

Elfogadas: 50/50 wake PASS.

## Teszt D: GPIO9 villamos ellenorzes

Szkoppal / logikai analizatorral:

1. Probe: GPIO9-DQ a node GND-hez kepest.
2. Tapra adas pillanataban figyeld az elso 100 ms-ot.
3. Reset pillanataban figyeld az elso 100 ms-ot.
4. A vonal alapallapota legyen gyorsan HIGH, kb. 3.3 V.

Elfogadas:
- resetkor/tapra adaskor nincs tartos LOW;
- a vonal nem lebeg;
- a 4.7 kOhm felhuzo egyertelmuen HIGH-on tartja a labat.

## Kontrollok

1. DS18B20 lehuzva, csak 4.7 kOhm felhuzo marad: 10 tapra adas PASS.
2. DS18B20 bekotve: 50 tapra adas PASS.
3. Ha van lehetoseg, hideg/meleg gyors kontroll:
   - szobahomerseklet;
   - szenzor/cella lehutve vagy felmelegitve, de parakicsapodas nelkul.

## Dontes

Ha minden fenti teszt PASS, akkor a GPIO9-es DS18B20 a rovid, kb. 5 cm-es kabellel elfogadhato
a vegleges node-on. A dokumentacioban ettol fuggetlenul jelolni kell:

- `DS18B20_GPIO = GPIO9`;
- GPIO9 strapping lab, ezert a 4.7 kOhm felhuzo kotelezo;
- hosszabb kabelre vagy kulso csatlakozos homero-agba ez a kiosztas nem viheto at automatikusan.

Ha barmelyik teszt FAIL, a firmware-rel ezt nem erdemes "kijavitani": a gond reset elotti
strapping-szint. Ilyenkor csak hardveres javitas segithet, peldaul erosebb felhuzo vagy a
vonal resetkori leválasztasa, de ezt kulon kell merlegelni.

## 2026-07-09 fogyasztas-debug megjegyzes

A GPIO9 / DS18B20 ag a fogyasztas-debug soran kulon gyanus lett, mert ha a pad alvasban LOW-ra
marad hajtva, a 4.7 kOhm felhuzo kb. 700 uA-t tud elfuteni. Ezert az aktualis node firmware-ben
a homero olvasasa tiltott, de deep sleep elott a GPIO9 explicit HIGH + hold allapotba kerul.

Aktualis kapcsolok:

```c
#define DS18B20_ENABLED 0
#define DS18B20_HOLD_HIGH_IN_SLEEP 1
```

Ha a homero-ag visszakerul aktiv hasznalatba, eloszor azt kell igazolni, hogy:

- meres utan a busz release/HIGH allapotba kerul;
- deep sleepben GPIO9 kb. 3.3 V;
- a deep sleep aram nem ugrik meg a 4.7 kOhm felhuzo miatt.
