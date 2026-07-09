/*
 * sens_hx711.h — HX711 mérőerősítő: power-up/down + nyers/kalibrált súly
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A HX711-M bit-bang olvasása (PD_SCK=GPIO5, DOUT=GPIO6). Power-down (PD_SCK HIGH>60us) alatt
 * a load cell excitation lekapcsol -> ~0 áram (H2, éles-mérve). A BENCH-ben a power_up/down
 * külön hívható (fázis-izolált mérés), a FIELD egy read_weight-et hív.
 */
#ifndef SENS_HX711_H
#define SENS_HX711_H

#include <stdbool.h>
#include <stdint.h>

/* GPIO-k beállítása (PD_SCK output, DOUT input). Egyszer, boot után. */
void hx711_init(void);

/* Power-up: PD_SCK LOW -> a chip aktív, excitation BE. Olvasás előtt hívandó. */
void hx711_power_up(void);

/* Power-down: PD_SCK HIGH (>60us) -> excitation lekapcsol, ~0 áram. Mérés után hívandó. */
void hx711_power_down(void);

/* Egy nyers 24-bit 2's complement olvasás (A csatorna, gain 128), 32-bitre kiterjesztve. */
int32_t hx711_read_raw(void);

/* Nyers olvasás státusszal. false = DOUT timeout / nincs modul / rossz bekötés. */
bool hx711_read_raw_checked(int32_t *raw_out);

/* HX711_SAMPLES átlagolt mérés. Egyelőre nyers átlag float-ként (kalibráció később). */
float hx711_read_weight(void);

/* HX711_SAMPLES átlagolt mérés státusszal. false esetén *weight_out = NAN. */
bool hx711_read_weight_checked(float *weight_out);

#endif /* SENS_HX711_H */
