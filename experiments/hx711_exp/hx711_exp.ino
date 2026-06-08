#include <Arduino.h>
#include <HX711.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <Preferences.h>

#include "secrets.h"

#define HX711_DOUT   2
#define HX711_SCK    1
#define TELNET_PORT  23
#define SAMPLE_MS    500   // 2 Hz mérési output

HX711       scale;
WiFiServer  telnetSrv(TELNET_PORT);
WiFiClient  telnetClient;
Preferences prefs;
String      cmdBuf;

// --- Serial + Telnet dual output -----------------------------------------
static void out(const char* s) {
  Serial.print(s);
  if (telnetClient && telnetClient.connected()) telnetClient.print(s);
}
static void outln(const char* s = "") {
  Serial.println(s);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(s);
    telnetClient.print("\r\n");
  }
}

// --- NVS: kalibráció mentés/betöltés -------------------------------------
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
  Serial.printf("[NVS] offset=%ld  factor=%.2f\n", off, fac);
}

// --- Telnet parancskezelő ------------------------------------------------
static void handleCmd(String& line) {
  line.trim();
  if (line.isEmpty()) return;

  char buf[128];

  if (line == "tare") {
    outln(">> Tara folyamatban (10 minta)...");
    scale.tare(10);
    snprintf(buf, sizeof(buf), ">> Tara kész. offset=%ld", scale.get_offset());
    outln(buf);
    calibSave();
  }
  else if (line.startsWith("cal ")) {
    float kg = line.substring(4).toFloat();
    if (kg <= 0.0f) { outln("!! Használat: cal <kg>  pl: cal 20.5"); return; }
    outln(">> Kalibráció (10 minta, tartsd a súlyt stabilon)...");
    float val = scale.get_value(10);
    if (val == 0.0f) { outln("!! Nulla tared érték — előbb tare?"); return; }
    float fac = val / kg;
    scale.set_scale(fac);
    snprintf(buf, sizeof(buf), ">> factor=%.4f  (ref: %.3f kg)", fac, kg);
    outln(buf);
    calibSave();
  }
  else if (line.startsWith("factor ")) {
    float f = line.substring(7).toFloat();
    if (f == 0.0f) { outln("!! factor nem lehet 0"); return; }
    scale.set_scale(f);
    snprintf(buf, sizeof(buf), ">> factor=%.4f", f);
    outln(buf);
    calibSave();
  }
  else if (line == "raw") {
    long r = scale.read();
    snprintf(buf, sizeof(buf), "raw=%ld  offset=%ld  tared=%ld  factor=%.4f  => %.4f kg",
             r, scale.get_offset(), r - scale.get_offset(),
             scale.get_scale(),
             scale.get_scale() != 0.0f ? (float)(r - scale.get_offset()) / scale.get_scale() : 0.0f);
    outln(buf);
  }
  else if (line == "info") {
    snprintf(buf, sizeof(buf), "IP=%s  RSSI=%ddBm  offset=%ld  factor=%.4f",
             WiFi.localIP().toString().c_str(), WiFi.RSSI(),
             scale.get_offset(), scale.get_scale());
    outln(buf);
  }
  else if (line == "save") {
    calibSave();
  }
  else if (line == "?" || line == "help") {
    outln("Parancsok:");
    outln("  tare            — nullázás (10 minta átlag)");
    outln("  cal <kg>        — kalibráció ismert súllyal");
    outln("  factor <ertek>  — scale factor kézi beállítás");
    outln("  raw             — nyers ADC + offset + kg");
    outln("  info            — IP, RSSI, kalibráció");
    outln("  save            — NVS mentés");
  }
  else {
    out("!! ismeretlen parancs: ");
    outln(line.c_str());
  }
}

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[bee_scale] HX711 + WiFi + OTA + Telnet — kísérleti");

  scale.begin(HX711_DOUT, HX711_SCK);
  calibLoad();
  Serial.println("[HX711] OK");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] csatlakozás: %s", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] FAIL — OTA + Telnet nem lesz elérhető");
    return;
  }
  Serial.printf("[WiFi] OK  IP=%s  RSSI=%ddBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // OTA
  ArduinoOTA.setHostname("bee-scale-exp");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] start..."); });
  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] kész, reboot"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] hiba [%u]\n", e);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] OK");

  // Telnet
  telnetSrv.begin();
  telnetSrv.setNoDelay(true);
  Serial.printf("[Telnet] port %d\n", TELNET_PORT);
  Serial.printf(">> telnet %s\n", WiFi.localIP().toString().c_str());
}

static uint32_t lastSampleMs = 0;

void loop() {
  ArduinoOTA.handle();

  // Telnet: új kliens fogadása
  if (!telnetClient || !telnetClient.connected()) {
    WiFiClient nc = telnetSrv.available();
    if (nc) {
      telnetClient = nc;
      cmdBuf = "";
      telnetClient.println("bee_scale> ? = help");
    }
  }

  // Telnet: parancs olvasás (nem-blokkoló, karakterenként)
  while (telnetClient && telnetClient.available()) {
    char ch = (char)telnetClient.read();
    if (ch == '\n' || ch == '\r') {
      if (cmdBuf.length()) { handleCmd(cmdBuf); cmdBuf = ""; }
    } else {
      cmdBuf += ch;
    }
  }

  // HX711 mintavétel és output
  if (millis() - lastSampleMs < SAMPLE_MS) return;
  lastSampleMs = millis();

  if (!scale.wait_ready_timeout(400)) {
    outln("[HX711] TIMEOUT");
    return;
  }
  long  r  = scale.read();
  float kg = scale.get_scale() != 0.0f
             ? (float)(r - scale.get_offset()) / scale.get_scale()
             : 0.0f;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3f kg\r\n", kg);
  out(buf);
}
