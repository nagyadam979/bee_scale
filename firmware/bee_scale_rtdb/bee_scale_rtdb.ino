// bee_scale_rtdb.ino — ESP32 → Firebase RTDB súly-feltöltő (kommunikációs modul).
//
// Feladat: 5 percenként egy súlymérési rekordot (ts, w_total, w1, w2) feltölt az
// RTDB /hives/<HIVE_ID>/readings ágába, hitelesítve és hibatűrően.
//   - Súlyforrás absztrahálva (most mock; HX711 külön feladat).
//   - Nem-blokkoló (millis()) ütemezés, WiFi auto-reconnect, retry/backoff,
//     RAM-only offline FIFO buffer.
//   - NINCS riasztás / Cloud Function / FCM. Firebase Spark-kompatibilis.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "weight_source.h"
#include "rtdb_writer.h"

// --- állapot ----------------------------------------------------------------
uint32_t g_lastSampleMs = 0;
bool     g_firstSampleDone = false;
uint32_t g_lastWifiTryMs = 0;
bool     g_ntpConfigured = false;

// ---------------------------------------------------------------------------
static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);   // stack-szintű auto-reconnect
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] connecting to \"%s\"...\n", WIFI_SSID);
}

// Nem-blokkoló WiFi-felügyelet: ha lecsatlakozott, periodikusan újrapróbál.
static void wifiMaintain() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - g_lastWifiTryMs < WIFI_RECONNECT_INTERVAL_MS) return;
  g_lastWifiTryMs = millis();
  Serial.println("[WiFi] not connected, reconnecting...");
  WiFi.reconnect();
}

// NTP-szinkron: csak csatlakozott WiFi mellett indítjuk (egyszer).
static void ntpMaintain() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_ntpConfigured) return;
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  g_ntpConfigured = true;
  Serial.println("[NTP] time sync requested");
}

static bool timeValid(uint32_t &epochOut) {
  const time_t now = time(nullptr);
  if ((uint32_t)now < VALID_EPOCH_MIN) return false;
  epochOut = (uint32_t)now;
  return true;
}

// 5 percenként (és az első érvényes időnél azonnal) mintát vesz és sorba állít.
static void sampleMaintain() {
  uint32_t epoch;
  if (!timeValid(epoch)) return;  // ts nélkül nem tűzünk rekordot

  const bool due = !g_firstSampleDone ||
                   (millis() - g_lastSampleMs >= SEND_INTERVAL_MS);
  if (!due) return;

  Weights wts;
  if (!getWeights(wts)) {
    Serial.println("[SAMPLE] getWeights() failed, skipping");
    return;
  }

  Reading r;
  r.ts = epoch;
  r.w_total = wts.w_total;
  r.w1 = wts.w1;
  r.w2 = wts.w2;

  rtdbEnqueue(r);
  g_lastSampleMs = millis();
  g_firstSampleDone = true;

  Serial.printf("[SAMPLE] ts=%lu w_total=%.2f w1=%.2f w2=%.2f (pending=%u)\n",
                (unsigned long)r.ts, r.w_total, r.w1, r.w2,
                (unsigned)rtdbPending());
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\n[bee_scale] ESP32 → Firebase RTDB writer");

  weightSourceBegin();
  wifiConnect();

  // Blokkolásmentes: nem várunk itt WiFi-re, a loop felügyeli.
  rtdbBegin();
}

void loop() {
  wifiMaintain();
  ntpMaintain();
  rtdbLoop();        // auth + async stack + küldés-állapotgép
  sampleMaintain();  // 5 perces mintavétel → FIFO
}
