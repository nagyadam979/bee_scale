# Feladat 01 — ESP32 → Firebase RTDB adatfeltöltő modul

> Delegálható implementációs feladat egy kódoló (implementáló) ágensnek.
> Olvasd be a `MEMORY.md`-t a projekt-kontextushoz, és maradj e feladat scope-jában.
> Készítette (orchestrátor): 2026-05-31.

## Cél
Az ESP32 **5 percenként** feltölt egy súlymérési rekordot a Firebase Realtime Database-be (RTDB), hitelesítve és hibatűrően. Ez a firmware **kommunikációs modulja** — nem érinti a HX711 olvasást (külön feladat) és **nincs benne riasztás**.

## Kontextus
- Okos méhkaptár-mérleg, **1 kaptár, prototípus**.
- Backend: **Firebase Spark (no-cost)** — adat-only. Részletek: `MEMORY.md` 4. szakasz.
- Hálózati áram van, WiFi tervezett → **nincs deep sleep**, a board ébren marad.
- Cloud Functions / Blaze **nem használható** (Sparkon maradunk).

## Bemenet (a felhasználó/projekt biztosítja `secrets.h`-ban)
- WiFi SSID + jelszó
- Firebase **Web API key**
- **RTDB URL**
- Dedikált **eszköz-user** email + jelszó (Firebase Authentication → Email/Password szolgáltató)
- `HIVE_ID` (pl. `hive1`)

## Elvárt kimenet (deliverable)
1. **Arduino-ESP32** firmware modul, lib: **mobizt FirebaseClient** (modern, ID-token auto-refresh). Auth: email/jelszó eszköz-user.
2. **Súlyforrás absztrahálva** `getWeights() → {w_total, w1, w2}` interfész mögött, **mock implementációval** (szintetikus, lassan változó értékek), hogy HX711 nélkül is fordítható/tesztelhető legyen. A valós HX711-integráció külön feladat, ezt az interfészt fogja megvalósítani.
3. **Írási út:** RTDB `push` a `/hives/{HIVE_ID}/readings` ág alá. Rekord séma:
   ```json
   { "ts": 1717160400, "w_total": 42.13, "w1": 21.02, "w2": 21.11 }
   ```
   `ts` = epoch **másodperc** (NTP-sync), `w_*` = kg (float).
4. **Ütemezés:** nem-blokkoló **5 perces** timer (`millis()` alapú, **nem** `delay`), hogy a WiFi/stack éljen.
5. **Hibatűrés:**
   - WiFi auto-reconnect.
   - Token-refresh kezelve (a lib végzi, de a hibaágat kezeld).
   - Írás **retry + exponenciális backoff**.
   - **RAM-only offline buffer** (FIFO, fix cap-pel): rövid kiesés alatt RAM-ban tartja a mintákat, reconnectkor kiüríti. **Áramszünet/reboot esetén a bufferelt minták elveszhetnek — ez elfogadott ebben a fázisban** (NVS-perzisztencia NEM kell).
6. **Secrets külön** `secrets.h`-ban, `.gitignore`-olva. Minden konfig (intervallum, `HIVE_ID`) egy helyen, jól láthatóan.
7. **RTDB Security Rules** mellékelve (külön fájl): az eszköz-user írhat `/hives/{HIVE_ID}` alá, hitelesített web app **csak olvas**. Nem publikus.

## Korlátok
- **Ne** építsd a HX711-olvasást (mock mögött marad).
- **Ne** tegyél bele riasztást / Cloud Functiont / FCM-et.
- Maradj **Sparkon** — semmi Blaze-függő szolgáltatás.
- Titkokat **ne commitolj**.
- A scope-ot ne lépd túl; ha a feladat ellentmondásos vagy a `MEMORY.md`-vel ütközik → **állj meg és jelezz vissza**.

## Kész-kritérium
- Fordul, felcsatlakozik WiFi-re, hitelesít, és **5 percenként ír egy rekordot**, ami a Firebase konzolban megjelenik a megadott sémával.
- WiFi-bontás után **magától visszaáll**, és a bufferelt minták kiürülnek (ellenőrizhető: a kiesés alatti rekordok reconnect után megjelennek).
- A Security Rules **tiltja** az illetéktelen írást/olvasást (gyors negatív teszt: nem hitelesített kliens nem ír/olvas).

## Mellékelt deliverable-ek
- `firmware/` — Arduino-ESP32 forrás (fő sketch + súly-interfész + mock + RTDB writer modul).
- `secrets.h.example` — kitöltendő sablon (a valós `secrets.h` gitignore-olt).
- `firebase/database.rules.json` — RTDB Security Rules.
- Rövid `README` a build/flash + Firebase-oldali előfeltételek (eszköz-user létrehozása, API key/DB URL beszerzése, rules feltöltése) lépéseivel.
