// rtdb_writer.cpp — Firebase RTDB feltöltő: auth + async push + retry/backoff + FIFO.
//
// A FirebaseClient lib (mobizt) build-kapcsolóit a header include ELŐTT kell
// definiálni; ezért a libet KIZÁRÓLAG ez a fordítási egység húzza be, így a
// FirebaseClient típusok nem szivárognak ki a modulból.

#define ENABLE_USER_AUTH   // email/jelszó auth + ID-token auto-refresh
#define ENABLE_DATABASE    // Realtime Database

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

#include "rtdb_writer.h"
#include "config.h"
#include "secrets.h"

// ---------------------------------------------------------------------------
// Firebase objektumok (globálisak — a lib így várja)
// ---------------------------------------------------------------------------
namespace {

WiFiClientSecure ssl_client;
AsyncClientClass aClient(ssl_client);

UserAuth  user_auth(FIREBASE_API_KEY, DEVICE_USER_EMAIL, DEVICE_USER_PASSWORD, USER_AUTH_EXPIRE_SEC);
FirebaseApp app;
RealtimeDatabase Database;

// Az aktuálisan repülő push eredménye + a feltöltési útvonal.
const String kReadingsPath = String("/hives/") + HIVE_ID + "/readings";

// --- FIFO ring buffer (RAM-only) -------------------------------------------
Reading  g_buf[BUFFER_CAPACITY];
size_t   g_head = 0;   // legrégebbi elem indexe
size_t   g_count = 0;  // tárolt elemek száma
uint32_t g_dropped = 0;

// --- küldés-állapotgép ------------------------------------------------------
// Szinkron (await) push-t használunk: determinisztikus siker/hiba, nincs
// callback-verseny és nincs hamis timeout. (Az async callback + saját timeout
// kombó a C3 + flaky WiFi mellett hamis timeoutot és DUPLIKÁLT írást okozott —
// lásd a commit-üzenetet.) A push blokkol a kérés idejére, de aktívan szervizeli
// a hálózatot; az 5 perces ütemezés továbbra is millis()-alapú, nem delay().
enum SendState { S_IDLE, S_BACKOFF };
SendState g_state = S_IDLE;

uint32_t g_backoffMs     = RETRY_BACKOFF_BASE_MS;
uint32_t g_nextAttemptMs = 0;

// ---------------------------------------------------------------------------
// FIFO segédek
// ---------------------------------------------------------------------------
bool bufEmpty() { return g_count == 0; }
bool bufFull()  { return g_count == BUFFER_CAPACITY; }

const Reading &bufFront() { return g_buf[g_head]; }

void bufPush(const Reading &r) {
  const size_t tail = (g_head + g_count) % BUFFER_CAPACITY;
  g_buf[tail] = r;
  g_count++;
}

void bufPopFront() {
  g_head = (g_head + 1) % BUFFER_CAPACITY;
  g_count--;
}

// ---------------------------------------------------------------------------
// Auth/app debug-callback (initializeApp adja át; a libet az app.loop() pörgeti).
// CSAK naplóz — a push eredményét NEM ez kezeli (az await-push szinkron tér vissza).
// ---------------------------------------------------------------------------
void rtdbAuthCb(AsyncResult &aResult) {
  if (aResult.isEvent()) {
    Firebase.printf("[RTDB] event: %s, msg: %s, code: %d\n",
                    aResult.uid().c_str(), aResult.eventLog().message().c_str(),
                    aResult.eventLog().code());
  }
  if (aResult.isDebug()) {
    Firebase.printf("[RTDB] debug: %s\n", aResult.debug().c_str());
  }
  if (aResult.isError()) {
    Firebase.printf("[RTDB] error: %s, msg: %s, code: %d\n",
                    aResult.uid().c_str(), aResult.error().message().c_str(),
                    aResult.error().code());
  }
}

// A FIFO élén lévő rekord SZINKRON (await) push-a. Blokkol a kérés idejére, majd
// determinisztikusan tér vissza: true = sikeres írás (van pushId), false = hiba.
bool tryPushFront() {
  const Reading &r = bufFront();

  object_t json, o_ts, o_wt, o_w1, o_w2;
  JsonWriter w;
  w.create(o_ts, "ts",      r.ts);                  // uint32 epoch sec (idézőjel nélkül)
  w.create(o_wt, "w_total", number_t(r.w_total, 2));
  w.create(o_w1, "w1",      number_t(r.w1, 2));
  w.create(o_w2, "w2",      number_t(r.w2, 2));
  w.join(json, 4, o_ts, o_wt, o_w1, o_w2);

  const String name = Database.push<object_t>(aClient, kReadingsPath, json);

  if (aClient.lastError().code() == 0 && name.length() > 0) {
    Firebase.printf("[RTDB] pushed ok, key: %s (ts=%lu)\n",
                    name.c_str(), (unsigned long)r.ts);
    return true;
  }
  Firebase.printf("[RTDB] push failed: msg: %s, code: %d\n",
                  aClient.lastError().message().c_str(), aClient.lastError().code());
  return false;
}

void scheduleBackoff() {
  g_nextAttemptMs = millis() + g_backoffMs;
  Firebase.printf("[RTDB] retry in %lu ms (pending=%u)\n",
                  (unsigned long)g_backoffMs, (unsigned)g_count);
  g_backoffMs = g_backoffMs * 2;
  if (g_backoffMs > RETRY_BACKOFF_MAX_MS) g_backoffMs = RETRY_BACKOFF_MAX_MS;
}

void resetBackoff() { g_backoffMs = RETRY_BACKOFF_BASE_MS; }

} // namespace

// ---------------------------------------------------------------------------
// Publikus API
// ---------------------------------------------------------------------------
void rtdbBegin() {
  ssl_client.setInsecure();  // Spark/prototípus: nem validáljuk a szervertanúsítványt

  Firebase.printf("[RTDB] FirebaseClient v%s\n", FIREBASE_CLIENT_VERSION);
  Serial.println("[RTDB] initializing app (email/password auth)...");

  initializeApp(aClient, app, getAuth(user_auth), rtdbAuthCb, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
}

void rtdbLoop() {
  // Auth + async taskok pörgetése.
  app.loop();

  const bool ready = app.ready() && WiFi.status() == WL_CONNECTED;

  switch (g_state) {
    case S_IDLE:
      if (ready && !bufEmpty()) {
        if (tryPushFront()) {     // szinkron push; csak megerősített sikerre popolunk
          bufPopFront();
          resetBackoff();         // marad S_IDLE → a következő mintát is kiüríti
        } else {
          scheduleBackoff();
          g_state = S_BACKOFF;
        }
      }
      break;

    case S_BACKOFF:
      if ((int32_t)(millis() - g_nextAttemptMs) >= 0) {
        g_state = S_IDLE;  // a lejárt backoff után újrapróbáljuk a FIFO élét
      }
      break;
  }
}

bool rtdbReady() {
  return app.ready();
}

bool rtdbEnqueue(const Reading &r) {
  if (bufFull()) {
    bufPopFront();   // drop-oldest
    g_dropped++;
    Firebase.printf("[RTDB] buffer full, dropped oldest (total dropped=%lu)\n",
                    (unsigned long)g_dropped);
    bufPush(r);
    return false;
  }
  bufPush(r);
  return true;
}

size_t rtdbPending() {
  return g_count;
}
