// Egyszerű HX711 olvasó ESP32-S3-ra, nyers érték a Serial monitorra.
// Bekötés: DT -> GPIO4, SCK -> GPIO2
// Könyvtár nélkül, közvetlen bit-bang (128-as erősítés, A csatorna).

#include <Arduino.h>

const int HX711_DT  = 4;   // adat (DOUT)
const int HX711_SCK = 2;   // órajel (PD_SCK)

// Egy 24 bites minta beolvasása a HX711-ből (blokkol, amíg kész nincs).
long hx711_read() {
  // Várunk, amíg a DT alacsonyra megy -> konverzió kész.
  while (digitalRead(HX711_DT) == HIGH) {
    delay(1);
  }

  long value = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(HX711_SCK, HIGH);
    delayMicroseconds(1);
    value = (value << 1) | digitalRead(HX711_DT);
    digitalWrite(HX711_SCK, LOW);
    delayMicroseconds(1);
  }

  // 25. impulzus -> következő olvasás: A csatorna, 128-as erősítés.
  digitalWrite(HX711_SCK, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX711_SCK, LOW);
  delayMicroseconds(1);

  // 24 bites előjeles érték kiterjesztése 32 bitre.
  if (value & 0x800000) {
    value |= 0xFF000000;
  }
  return value;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(HX711_SCK, OUTPUT);
  pinMode(HX711_DT, INPUT);
  digitalWrite(HX711_SCK, LOW);
  Serial.println("HX711 teszt indul (DT=GPIO4, SCK=GPIO2)");
}

void loop() {
  long raw = hx711_read();
  Serial.print("HX711 raw: ");
  Serial.println(raw);
  delay(200);
}
