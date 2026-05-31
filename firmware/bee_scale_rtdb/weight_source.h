// weight_source.h — súlyforrás absztrakció.
// A valós HX711-integráció KÜLÖN feladat; ezt az interfészt fogja megvalósítani.
// Most a weight_source_mock.cpp adja a szintetikus implementációt.
#pragma once

#include <stdint.h>

struct Weights {
  float w_total;  // kg — összsúly (w1 + w2)
  float w1;       // kg — 1. mérőrúd (HX711 #1)
  float w2;       // kg — 2. mérőrúd (HX711 #2)
};

// Egyszeri init (HX711 esetén: pin setup, tara, kalibráció betöltése).
void weightSourceBegin();

// Egy mérés kiolvasása. true, ha érvényes érték áll elő.
bool getWeights(Weights &out);
