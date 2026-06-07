#include <Arduino.h>
#include <HX711.h>

#ifndef HX711_DATA_PIN_A
#define HX711_DATA_PIN_A 2   // DOUT — bekötött HX711
#endif

#ifndef HX711_DATA_PIN_B
#define HX711_DATA_PIN_B 4   // DOUT — második HX711 (még nincs bekötve)
#endif

#ifndef HX711_SCK_PIN
#define HX711_SCK_PIN 1      // közös SCK (mindkét HX711)
#endif

#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
#define DEBUG_PORT Serial
#else
#define DEBUG_PORT Serial
#endif

namespace {

constexpr uint8_t kDataPinA = HX711_DATA_PIN_A;
constexpr uint8_t kDataPinB = HX711_DATA_PIN_B;
constexpr uint8_t kClockPin = HX711_SCK_PIN;
constexpr uint32_t kReadyTimeoutMs = 1000;
HX711 scaleA;
HX711 scaleB;

}  // namespace

void setup() {
  DEBUG_PORT.begin(115200);
  delay(1500);

  scaleA.begin(kDataPinA, kClockPin);
  scaleB.begin(kDataPinB, kClockPin);
  scaleA.set_gain(128);
  scaleB.set_gain(128);
}

void loop() {
  static bool bannerPrinted = false;

  if (!bannerPrinted) {
    DEBUG_PORT.println();
    DEBUG_PORT.println("ESP32-C3 dual HX711 probe starting");
    DEBUG_PORT.printf("HX711 A: DOUT=GPIO%u, SCK=GPIO%u\n", kDataPinA, kClockPin);
    DEBUG_PORT.printf("HX711 B: DOUT=GPIO%u, SCK=GPIO%u\n", kDataPinB, kClockPin);
    DEBUG_PORT.println("Waiting for both HX711 modules...");
    bannerPrinted = true;
  }

  const bool readyA = scaleA.wait_ready_timeout(kReadyTimeoutMs);
  const bool readyB = scaleB.wait_ready_timeout(kReadyTimeoutMs);

  if (!readyA || !readyB) {
    DEBUG_PORT.printf(
        "HX711 ready timeout. A=%s B=%s\n",
        readyA ? "OK" : "TIMEOUT",
        readyB ? "OK" : "TIMEOUT");
    delay(1000);
    return;
  }

  const long rawA = scaleA.read();
  const long rawB = scaleB.read();
  DEBUG_PORT.printf("HX711 A raw: %ld | HX711 B raw: %ld\n", rawA, rawB);
  delay(250);
}