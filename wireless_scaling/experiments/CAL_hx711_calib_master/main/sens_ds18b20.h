/*
 * sens_ds18b20.h — DS18B20 1-Wire MULTI-szonda olvasó (MASTER-variáns).
 * bee_scale / wireless_scaling · 2. fázis · experiments/CAL_hx711_calib_master
 *
 * A masteren 2 DS18B20 van EGY buszon (hive_c + scale_c), ROM-cím szerint. Ez az olvasó a prod
 * master.c ROM-search + match-ROM logikáját emeli át: bootkor megkeresi az összes szondát
 * (max DS18B20_MAX_SENSORS), a ROM-okat RENDEZETT (stabil) sorrendben tárolja, és egy körben
 * kiolvassa mindegyik hőjét. A "melyik a scale" döntést a felhő-oldali fit hozza a bootkor
 * kiírt ROM-lista alapján — a kalibrációs firmware NEM végez szerep-mappinget, mindkettőt logol.
 */
#ifndef SENS_DS18B20_H
#define SENS_DS18B20_H

#include <stdbool.h>
#include <stdint.h>
#include "cal_config.h"

/* 1-Wire GPIO init + ROM-search a buszon. Egyszer, boot után. Feltölti a talált szondák
 * ROM-jait (rendezett sorrendben) és a darabszámot. */
void ds18b20_init(void);

/* Hány szondát talált a boot-kori ROM-search (0..DS18B20_MAX_SENSORS). */
int ds18b20_get_count(void);

/* Az idx-edik szonda 8 bájtos ROM-ja (a "# DS ROLES" header kiírásához), vagy NULL ha idx
 * a talált darabszámon kívül esik. A sorrend a rendezett ROM-search sorrendje (stabil). */
const uint8_t *ds18b20_get_rom(int idx);

/* Egy konverzió + az összes talált szonda kiolvasása egy körben. A temps[] tömbbe (max elem)
 * kerül az első min(count, max) szonda °C-ja, a rendezett ROM-sorrend szerint; a hiányzó/hibás
 * (nincs jelen / CRC / range / 85 sentinel / all-0/all-FF) helyekre NAN. Visszatérés: true ha
 * MINDEN talált szonda hibátlanul olvasódott. */
bool ds18b20_read_all(float *temps, int max);

#endif /* SENS_DS18B20_H */
