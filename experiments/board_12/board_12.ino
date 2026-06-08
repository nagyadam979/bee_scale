#include <Arduino.h>
#include <HX711.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <Preferences.h>

#include "secrets.h"

// Board "12" — MAC …31:50
#define HX711_DOUT    1
#define HX711_SCK     2
#define TELNET_PORT   23
#define STREAM_MS     500
#define TARE_SAMPLES  64    // ~6.4 s @ 10 SPS
#define CAL_SAMPLES   64

HX711       scale;
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
  outln("|    bee_scale  12  menu    |");
  outln("+---------------------------+");
  snprintf(buf, sizeof(buf), "| offset=%-7ld  factor=%-6.1f|",
           scale.get_offset(), scale.get_scale());
  outln(buf);
  outln("+---------------------------+");
  outln("|  [t]  tara (nulláz)       |");
  outln("|  [c]  kalibráció (kg)     |");
  outln("|  [s]  aktuális érték      |");
  outln("|  [r]  raw ADC             |");
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
    case 'i':
      snprintf(buf, sizeof(buf),
               "\r\n  IP=%s  RSSI=%ddBm\r\n  offset=%ld  factor=%.4f",
               WiFi.localIP().toString().c_str(), WiFi.RSSI(),
               scale.get_offset(), scale.get_scale());
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

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[bee_scale] board-12  HX711+WiFi+OTA+Telnet");

  scale.begin(HX711_DOUT, HX711_SCK);
  calibLoad();
  Serial.println("[HX711] OK");

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

  ArduinoOTA.setHostname("bee-scale-12");
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
      outln("bee_scale-12  —  bármely billentyű = menü");
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

  if (tMode != STREAM) return;
  if (millis() - lastStreamMs < STREAM_MS) return;
  lastStreamMs = millis();

  if (!scale.wait_ready_timeout(400)) { outln("[HX711] TIMEOUT"); return; }
  long  r  = scale.read();
  float kg = (float)(r - scale.get_offset()) / scale.get_scale();
  char  buf[32];
  snprintf(buf, sizeof(buf), "%.3f kg\r\n", kg);
  out(buf);
}
