/*
 * sens_ds18b20.h — DS18B20 hőmérő (1-Wire, node_config.h szerinti GPIO, 4.7k felhúzó)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A hőmérséklet a súly drift-korrekciójához + a node-hő diagnosztikához (M1: a C3 belső
 * die-hő ehhez kevés volt). A FIELD ciklus a konverziót korán indítja, majd HX711/VBAT után
 * olvassa ki. Hiba (nincs eszköz / CRC) -> NAN.
 */
#ifndef SENS_DS18B20_H
#define SENS_DS18B20_H

#include <stdbool.h>

/* 1-Wire GPIO init (open-drain + felhúzó feltételezve a HW-en). Egyszer, boot után. */
void ds18b20_init(void);

/* Konverzió indítása. true esetén DS18B20_CONVERSION_MS után olvasható ki az eredmény. */
bool ds18b20_start_conversion(void);

/* Korábban elindított konverzió scratchpad-kiolvasása °C-ban. Hiba esetén NAN. */
float ds18b20_read_temp_after_conversion(void);

/* Egy blocking hőmérséklet-olvasás °C-ban. Hiba esetén NAN (a payload temp mezőjébe). */
float ds18b20_read_temp(void);

/* Az utolsó olvasás diagnosztikai minősége. A numerikus 85.0 / CRC- vagy
 * range-hibás értéket a driver visszaadja, ezt a getter jelzi hibásnak. */
bool ds18b20_last_read_had_error(void);

#endif /* SENS_DS18B20_H */
