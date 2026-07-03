/*
 * sens_ds18b20.h — DS18B20 hőmérő (1-Wire, GPIO10, 4.7k felhúzó)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A hőmérséklet a súly drift-korrekciójához + a node-hő diagnosztikához (M1: a C3 belső
 * die-hő ehhez kevés volt). Blocking olvasás: reset -> skip ROM -> convert T -> ~750 ms
 * (12 bit) -> read scratchpad. Hiba (nincs eszköz / CRC) -> NAN.
 *
 * STÁTUSZ: STUB — az interfész végleges; a valós 1-Wire implementáció (bit-bang vagy RMT,
 * eldöntendő a feltöltéskor) itt kerül be. ÚJ modul, nincs kísérleti előzménye.
 */
#ifndef SENS_DS18B20_H
#define SENS_DS18B20_H

/* 1-Wire GPIO init (open-drain + felhúzó feltételezve a HW-en). Egyszer, boot után. */
void ds18b20_init(void);

/* Egy blocking hőmérséklet-olvasás °C-ban. Hiba esetén NAN (a payload temp mezőjébe). */
float ds18b20_read_temp(void);

#endif /* SENS_DS18B20_H */
