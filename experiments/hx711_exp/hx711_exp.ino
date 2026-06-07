#include <Arduino.h>
#include <HX711.h>

// Csak egy HX711 bekötve (A): DOUT=GPIO2, SCK=GPIO1
// B (GPIO4) nincs bekötve — inicializálása kihagyva (set_gain() blokkolna)
#define HX711_DATA_PIN_A 1
#define HX711_SCK_PIN    2

namespace {
  constexpr uint8_t kDataPin  = HX711_DATA_PIN_A;
  constexpr uint8_t kClockPin = HX711_SCK_PIN;
  constexpr uint32_t kReadyTimeoutMs = 1000;
  HX711 scale;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  // Banner rögtön, mielőtt bármilyen blokkolható HX711 hívás
  Serial.printf("\n=== HX711 probe: DOUT=GPIO%u, SCK=GPIO%u ===\n", kDataPin, kClockPin);

  Serial.println("HX711 begin...");
  scale.begin(kDataPin, kClockPin);  // set_gain(128) benne van → blokkolhat, ha nincs válasz
  Serial.println("HX711 begin done");
}

void loop() {
  const bool ready = scale.wait_ready_timeout(kReadyTimeoutMs);
  if (!ready) {
    Serial.println("HX711: TIMEOUT (nem válaszol)");
    delay(500);
    return;
  }
  const long raw = scale.read();
  Serial.printf("raw: %ld\n", raw);
  delay(250);
}
