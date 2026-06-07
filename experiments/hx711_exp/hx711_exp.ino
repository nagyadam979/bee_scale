#include <Arduino.h>
#include <HX711.h>

#define HX711_DATA_PIN_A 2
#define HX711_SCK_PIN    1

namespace {
  constexpr uint8_t kDataPin  = HX711_DATA_PIN_A;
  constexpr uint8_t kClockPin = HX711_SCK_PIN;
  constexpr uint32_t kReadyTimeoutMs = 1000;
  HX711 scale;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== HX711 probe: DOUT=GPIO%u, SCK=GPIO%u ===\n", kDataPin, kClockPin);
  scale.begin(kDataPin, kClockPin);
  Serial.println("HX711 ready");
}

void loop() {
  if (!scale.wait_ready_timeout(kReadyTimeoutMs)) {
    Serial.println("TIMEOUT");
    delay(500);
    return;
  }
  Serial.printf("raw: %ld\n", scale.read());
  delay(250);
}
