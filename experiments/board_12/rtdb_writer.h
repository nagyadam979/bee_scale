// rtdb_writer.h — Firebase RTDB feltöltő modul publikus API-ja.
// Elrejti a FirebaseClient lib-et: a fő sketch csak Reading-eket sorba állít,
// a modul intézi az auth-ot, az async push-t, a retry/backoff-ot és a FIFO buffert.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Egy feltöltendő rekord típusa: súly (a board saját rúdja) vagy környezeti
// (AHT20+BMP280). A típus dönti el, melyik RTDB ágra és milyen JSON-nal megy ki.
enum RecType : uint8_t { REC_WEIGHT, REC_ENV };

// Egy feltöltendő rekord. ts = UTC epoch másodperc (mindkét típusnál).
//   REC_WEIGHT → READINGS_PATH,     payload: { ts, w }      (w = kalibrált kg)
//   REC_ENV    → ENV_READINGS_PATH, payload: { ts, t, h, p } (°C, %RH, hPa)
// A két típus EGY közös FIFO-ban utazik, beérkezési sorrendben ürül.
struct Reading {
  uint32_t ts;
  RecType  type;
  float    w;            // REC_WEIGHT
  float    t, h, p;      // REC_ENV: hőmérséklet, páratartalom, nyomás
};

// Egyszeri init: SSL kliens, FirebaseApp inicializálás (email/jelszó auth), DB URL.
void rtdbBegin();

// Minden loop()-ban hívandó: pörgeti az auth/async stacket és a küldés-állapotgépet.
void rtdbLoop();

// true, ha az auth kész és lehet írni (app.ready()).
bool rtdbReady();

// Rekord a FIFO végére. Ha a buffer tele van, a LEGRÉGEBBI esik ki (drop-oldest).
// Visszatérés: true, ha nem esett ki minta; false, ha túlcsordulás miatt droppolt.
bool rtdbEnqueue(const Reading &r);

// Várakozó (még fel nem töltött) minták száma a bufferben.
size_t rtdbPending();
