#include <Arduino.h>
#include <HX711.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <Preferences.h>
#include <time.h>

#include "secrets.h"
#include "config.h"
#include "rtdb_writer.h"

// Board "21" — MAC …89:10
#define HX711_DOUT    2
#define HX711_SCK     1
// DS18B20 OneWire kaptár-hőmérő (2026-06-08, board_21-re kötve, GPIO3). Külső
// felhúzó (4.7 kΩ a DQ↔VCC közé) szükséges, ha a modulon nincs rajta.
#define ONEWIRE_PIN   3
#define TELNET_PORT   23
#define STREAM_MS     500
#define TARE_SAMPLES  64    // ~6.4 s @ 10 SPS
#define CAL_SAMPLES   64

HX711       scale;
OneWire           oneWire(ONEWIRE_PIN);
DallasTemperature ds18b20(&oneWire);   // DS18B20 kaptár-hőmérő
bool              dsOk = false;        // van-e a buszon DS18B20
WiFiServer  telnetSrv(TELNET_PORT);
WiFiClient  telnetClient;
Preferences prefs;

enum Mode { STREAM, MENU, CAL_INPUT };
Mode   tMode = STREAM;
String inputBuf;

// --- dual output ----------------------------------------------------------
static void out(const char* s) {
  Serial.print(s);
  if (telnetClient && telnetClient.connected()) telnetClient.print(s);
}
static void outln(const char* s = "") {
  Serial.println(s);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(s); telnetClient.print("\r\n");
  }
}

// --- NVS ------------------------------------------------------------------
static void calibSave() {
  prefs.begin("hx711", false);
  prefs.putLong ("offset", scale.get_offset());
  prefs.putFloat("factor", scale.get_scale());
  prefs.end();
  outln(">> Mentve NVS-be.");
}
static void calibLoad() {
  prefs.begin("hx711", true);
  long  off = prefs.getLong ("offset", 0);
  float fac = prefs.getFloat("factor", 1.0f);
  prefs.end();
  scale.set_offset(off);
  scale.set_scale(fac > 0.0f ? fac : 1.0f);
  Serial.printf("[NVS] offset=%ld  factor=%.4f\n", off, fac);
}

// --- menü -----------------------------------------------------------------
static void showMenu() {
  char buf[48];
  outln("\r\n+---------------------------+");
  outln("|    bee_scale  21  menu    |");
  outln("+---------------------------+");
  snprintf(buf, sizeof(buf), "| offset=%-7ld  factor=%-6.1f|",
           scale.get_offset(), scale.get_scale());
  outln(buf);
  outln("+---------------------------+");
  outln("|  [t]  tara (nulláz)       |");
  outln("|  [c]  kalibráció (kg)     |");
  outln("|  [s]  aktuális érték      |");
  outln("|  [r]  raw ADC             |");
  outln("|  [d]  hőm (DS18B20)       |");
  outln("|  [w]  1-wire diag         |");
  outln("|  [i]  info / IP           |");
  outln("|  [q]  vissza streamingbe  |");
  outln("+---------------------------+");
  out("> ");
}

static void doTare() {
  char buf[64];
  snprintf(buf, sizeof(buf), "\r\nTara (%d minta, ~%.0f s)...",
           TARE_SAMPLES, TARE_SAMPLES / 10.0f);
  outln(buf);
  scale.tare(TARE_SAMPLES);
  snprintf(buf, sizeof(buf), "Kész — offset=%ld", scale.get_offset());
  outln(buf);
  calibSave();
}

static void doCal(float kg) {
  char buf[80];
  snprintf(buf, sizeof(buf), "Kalibráció (%d minta, ~%.0f s)...",
           CAL_SAMPLES, CAL_SAMPLES / 10.0f);
  outln(buf);
  float val = scale.get_value(CAL_SAMPLES);
  if (val == 0.0f) { outln("!! Nulla tared érték — előbb tara!"); return; }
  float fac = val / kg;
  scale.set_scale(fac);
  snprintf(buf, sizeof(buf), "Kész — factor=%.4f  (%.3f kg)", fac, kg);
  outln(buf);
  calibSave();
}

static void handleMenuKey(char ch) {
  char buf[128];
  switch (ch) {
    case 't': doTare(); showMenu(); break;
    case 'c':
      outln("\r\nReferencia súly kg-ban (pl. 20.5), Enter:");
      out("> ");
      inputBuf = "";
      tMode = CAL_INPUT;
      break;
    case 's':
      if (scale.wait_ready_timeout(500)) {
        long  r  = scale.read();
        float kg = (float)(r - scale.get_offset()) / scale.get_scale();
        snprintf(buf, sizeof(buf), "\r\n  → %.3f kg  (raw=%ld)", kg, r);
        outln(buf);
      } else outln("\r\n  → HX711 TIMEOUT");
      showMenu();
      break;
    case 'r':
      if (scale.wait_ready_timeout(500)) {
        long r = scale.read();
        snprintf(buf, sizeof(buf),
                 "\r\n  raw=%ld  offset=%ld  tared=%ld  factor=%.4f  => %.3f kg",
                 r, scale.get_offset(), r - scale.get_offset(),
                 scale.get_scale(),
                 (float)(r - scale.get_offset()) / scale.get_scale());
        outln(buf);
      } else outln("\r\n  → HX711 TIMEOUT");
      showMenu();
      break;
    case 'd': {
      Reading rt;
      uint32_t ep = (uint32_t)time(nullptr);
      if (readTemp(ep, rt)) {
        snprintf(buf, sizeof(buf), "\r\n  → %.2f °C", rt.t);
      } else {
        snprintf(buf, sizeof(buf),
                 "\r\n  hőm olvasás SIKERTELEN (DS18B20=%s)", dsOk ? "ok" : "fail");
      }
      outln(buf);
      showMenu();
      break;
    }
    case 'w': {
      // 1-Wire busz-diagnosztika. Reboot nélkül újra-detektál: ezért a
      // huzalozás/felhúzó javítása után azonnal visszajelez. Frissíti a dsOk-t is.
      snprintf(buf, sizeof(buf), "\r\n1-Wire diag (GPIO%d)...", ONEWIRE_PIN);
      outln(buf);
      // 0) Idle-vonalszint: megkülönbözteti a hibamódokat. Belső felhúzóval HIGH =
      //    a vonal szabad (külső 4.7k hiányozhat, de nincs rövidzár); LOW = valami
      //    GND-re húzza (DQ↔GND csere / tápolatlan szenzor). Felhúzó nélkül a sima
      //    INPUT lebeg (értéke esetleges).
      pinMode(ONEWIRE_PIN, INPUT);
      delayMicroseconds(50);
      const int idleFloat = digitalRead(ONEWIRE_PIN);
      pinMode(ONEWIRE_PIN, INPUT_PULLUP);
      delayMicroseconds(50);
      const int idlePullup = digitalRead(ONEWIRE_PIN);
      snprintf(buf, sizeof(buf), "  idle: INPUT=%d  INPUT_PULLUP=%d  (%s)",
               idleFloat, idlePullup,
               idlePullup ? "vonal szabad (kulso felhuzo?)"
                          : "GND-re huzva (bekotes/rovidzar)");
      outln(buf);
      pinMode(ONEWIRE_PIN, INPUT);
      // 1) Elektromos jelenlét: a reset presence-pulzust vár. NO → a vonal lebeg
      //    (hiányzó 4.7k felhúzó / szakadás), nem is OneWire-protokoll kérdés.
      const bool presence = oneWire.reset();
      snprintf(buf, sizeof(buf), "  reset/presence: %s",
               presence ? "YES (van eszkoz a buszon)"
                        : "NO  (vonal lebeg -> felhuzo/bekotes hianyzik?)");
      outln(buf);
      // 2) ROM-search: minden eszköz 64-bites ROM-ja, family-kód + CRC.
      oneWire.reset_search();
      uint8_t addr[8];
      int n = 0;
      while (oneWire.search(addr)) {
        char rom[20]; int o = 0;
        for (int i = 0; i < 8; i++) o += snprintf(rom + o, sizeof(rom) - o, "%02X", addr[i]);
        const bool crcok = (OneWire::crc8(addr, 7) == addr[7]);
        const char *fam = addr[0] == 0x28 ? "DS18B20" :
                          addr[0] == 0x10 ? "DS18S20" :
                          addr[0] == 0x22 ? "DS1822"  : "?";
        snprintf(buf, sizeof(buf), "  #%d ROM=%s fam=0x%02X(%s) CRC=%s",
                 n, rom, addr[0], fam, crcok ? "ok" : "BAD");
        outln(buf);
        n++;
      }
      snprintf(buf, sizeof(buf), "  search: %d eszkoz", n);
      outln(buf);
      // 3) DallasTemperature re-init → dsOk frissítés + parazita-táp + olvasás.
      ds18b20.begin();
      dsOk = ds18b20.getDeviceCount() > 0;
      if (dsOk) {
        ds18b20.setResolution(12);
        const bool para = ds18b20.isParasitePowerMode();
        ds18b20.requestTemperatures();
        const float c = ds18b20.getTempCByIndex(0);
        snprintf(buf, sizeof(buf), "  dsOk=YES parazita-tap=%s  t=%.2f C",
                 para ? "yes" : "no", c);
      } else {
        snprintf(buf, sizeof(buf), "  dsOk=NO (DallasTemperature 0 eszkozt lat)");
      }
      outln(buf);
      showMenu();
      break;
    }
    case 'i':
      snprintf(buf, sizeof(buf),
               "\r\n  IP=%s  RSSI=%ddBm\r\n  offset=%ld  factor=%.4f\r\n  rtdb pending=%u",
               WiFi.localIP().toString().c_str(), WiFi.RSSI(),
               scale.get_offset(), scale.get_scale(),
               (unsigned)rtdbPending());
      outln(buf);
      showMenu();
      break;
    case 'q':
      outln("\r\nStreaming... (bármely billentyű = menü)");
      tMode = STREAM;
      break;
    default: showMenu(); break;
  }
}

static void handleCalInput(const String& line) {
  float kg = line.toFloat();
  if (kg > 0.0f) {
    doCal(kg);
    tMode = MENU;
    showMenu();
  } else {
    outln("Érvénytelen — próbáld újra (pl. 20.5):");
    out("> ");
    inputBuf = "";
  }
}

// --- RTDB mintavétel ------------------------------------------------------
// 5 percenként (és az első érvényes NTP-időnél azonnal) átlagolt kalibrált
// súlymintát vesz, és a FIFO-ba teszi. NEM blokkol (millis()-alapú), és a
// telnet módtól FÜGGETLENÜL fut (a loop a streaming early-return ELŐTT hívja).
static uint32_t g_lastSampleMs    = 0;
static bool     g_firstSampleDone = false;

static bool timeValid(uint32_t &epochOut) {
  const time_t now = time(nullptr);
  if ((uint32_t)now < VALID_EPOCH_MIN) return false;  // óra még nincs szinkronban
  epochOut = (uint32_t)now;
  return true;
}

// DS18B20 → temp Reading. Blokkoló konverzió (~750 ms @ 12-bit). false, ha nincs
// szenzor a buszon vagy a mérés érvénytelen (lecsatlakozott szonda → -127 °C).
static bool readTemp(uint32_t epoch, Reading &r) {
  if (!dsOk) return false;
  ds18b20.requestTemperatures();
  const float c = ds18b20.getTempCByIndex(0);
  if (c <= DEVICE_DISCONNECTED_C) return false;  // -127: nincs válasz / szakadás
  r.ts   = epoch;
  r.type = REC_TEMP;
  r.t    = c;                                    // °C
  return true;
}

static void sampleMaintain() {
  uint32_t epoch;
  if (!timeValid(epoch)) return;   // ts nélkül nem tűzünk rekordot

  const bool due = !g_firstSampleDone ||
                   (millis() - g_lastSampleMs >= SEND_INTERVAL_MS);
  if (!due) return;

  if (!scale.is_ready()) return;   // ha az ADC nem kész, később újrapróbáljuk

  Reading rw;
  rw.type = REC_WEIGHT;
  rw.ts   = epoch;
  rw.w    = scale.get_units(WEIGHT_AVG_SAMPLES);  // átlagolt kg: (avg-offset)/scale
  rtdbEnqueue(rw);

  // Hőmérséklet-minta ugyanabban az ütemben (külön RTDB ágra). Ha a szenzor nem
  // elérhető, a súly attól még megy — a hőm egyszerűen kimarad ebből a ciklusból.
  Reading rt;
  if (readTemp(epoch, rt)) {
    rtdbEnqueue(rt);
    Serial.printf("[SAMPLE] ts=%lu w=%.2f t=%.2f (pending=%u)\n",
                  (unsigned long)epoch, rw.w, rt.t, (unsigned)rtdbPending());
  } else {
    Serial.printf("[SAMPLE] ts=%lu w=%.2f temp=SKIP (pending=%u)\n",
                  (unsigned long)epoch, rw.w, (unsigned)rtdbPending());
  }

  g_lastSampleMs    = millis();
  g_firstSampleDone = true;
}

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[bee_scale] board-21  HX711+WiFi+OTA+Telnet");

  scale.begin(HX711_DOUT, HX711_SCK);
  calibLoad();
  Serial.println("[HX711] OK");

  // DS18B20 OneWire kaptár-hőmérő (GPIO4). 12-bit felbontás (~0.0625 °C, ~750 ms
  // konverzió — blokkoló kérés, de csak 5 percenként, a HX711-átlaggal egy ütemben).
  ds18b20.begin();
  const int dsCount = ds18b20.getDeviceCount();
  dsOk = dsCount > 0;
  if (dsOk) ds18b20.setResolution(12);
  Serial.printf("[DS18B20] %s (%d eszkoz a buszon)\n", dsOk ? "OK" : "FAIL", dsCount);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] %s", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] FAIL");
    return;
  }
  Serial.printf("[WiFi] OK  IP=%s  RSSI=%ddBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // NTP-szinkron (UTC, offset 0) + Firebase RTDB init — WiFi UTÁN.
  configTime(0, 0, "pool.ntp.org");
  Serial.println("[NTP] time sync requested");
  rtdbBegin();

  ArduinoOTA.setHostname("bee-scale-21");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]()   { Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]()     { Serial.println("[OTA] kész"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] hiba %u\n", e); });
  ArduinoOTA.begin();

  telnetSrv.begin();
  telnetSrv.setNoDelay(true);
  Serial.printf("[Telnet] →  nc %s %d\n", WiFi.localIP().toString().c_str(), TELNET_PORT);
}

static uint32_t lastStreamMs = 0;

void loop() {
  ArduinoOTA.handle();

  if (!telnetClient || !telnetClient.connected()) {
    WiFiClient nc = telnetSrv.available();
    if (nc) {
      telnetClient = nc;
      tMode = STREAM;
      inputBuf = "";
      outln("bee_scale-21  —  bármely billentyű = menü");
    }
  }

  while (telnetClient && telnetClient.available()) {
    char ch = (char)telnetClient.read();
    if (tMode == STREAM) {
      if (ch == '\r' || ch == '\n') continue;
      tMode = MENU;
      showMenu();
    } else if (tMode == MENU) {
      handleMenuKey(ch);
    } else if (tMode == CAL_INPUT) {
      if (ch == '\n' || ch == '\r') {
        if (inputBuf.length()) handleCalInput(inputBuf);
      } else if (ch == 127 || ch == 8) {
        if (inputBuf.length()) { inputBuf.remove(inputBuf.length() - 1); out("\b \b"); }
      } else {
        inputBuf += ch; char e[2] = {ch, 0}; out(e);
      }
    }
  }

  // RTDB feltöltés + mintavétel: MINDEN loopban, a telnet módtól FÜGGETLENÜL
  // (a streaming early-return ELŐTT) — különben menü módban leállna a feltöltés.
  rtdbLoop();
  sampleMaintain();

  if (tMode != STREAM) return;
  if (millis() - lastStreamMs < STREAM_MS) return;
  lastStreamMs = millis();

  if (!scale.is_ready()) return;
  long  r  = scale.read();
  float kg = (float)(r - scale.get_offset()) / scale.get_scale();
  char  buf[32];
  snprintf(buf, sizeof(buf), "%.3f kg\r\n", kg);
  out(buf);
}
