// rtdb_writer.h — Firebase RTDB feltöltő modul publikus API-ja.
// Elrejti a FirebaseClient lib-et: a fő sketch csak Reading-eket sorba állít,
// a modul intézi az auth-ot, az async push-t, a retry/backoff-ot és a FIFO buffert.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Egy feltöltendő mérési rekord. ts = UTC epoch másodperc, w = kalibrált kg.
// (Boardonként EGYETLEN súlyérték — a board saját rúdjának súlya.)
struct Reading {
  uint32_t ts;
  float    w;
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
