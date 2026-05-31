// weight_source_mock.cpp — szintetikus súlyforrás HX711 nélküli teszteléshez.
// Lassan változó, kg-ban realisztikus értékeket ad (napi hordás/párolgás imitálás
// + kis zaj). Cseréld le a valódi HX711-olvasásra (külön feladat).

#include <Arduino.h>
#include <math.h>
#include "weight_source.h"

namespace {
// Kaptáranként ~40 kg körüli össztömeg, rudanként ~fele, kis aszimmetriával.
constexpr float kBaseW1 = 21.0f;   // kg
constexpr float kBaseW2 = 21.0f;   // kg
constexpr float kSwingKg = 1.5f;   // lassú lengés amplitúdója
constexpr float kPeriodS = 3600.0f; // 1 órás lengés-periódus (mock)
}

void weightSourceBegin() {
  // Mocknál nincs teendő. (HX711: pinMode, scale.begin, set_scale, tare.)
}

bool getWeights(Weights &out) {
  const float t = millis() / 1000.0f;
  const float phase = 2.0f * (float)M_PI * t / kPeriodS;

  // Determinisztikus lassú lengés + enyhe, rúdankénti eltérés.
  const float noise1 = 0.02f * sinf(t * 0.37f);
  const float noise2 = 0.02f * cosf(t * 0.29f);

  out.w1 = kBaseW1 + kSwingKg * sinf(phase) + noise1;
  out.w2 = kBaseW2 + kSwingKg * sinf(phase + 0.3f) + noise2;
  out.w_total = out.w1 + out.w2;
  return true;
}
