/*
 * node_sleep.h — deep-sleep ciklus + RTC-megőrzött állapot (cycle / boot_id)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A C3-on a deep sleep = teljes reboot (app_main újraindul), de az RTC_DATA_ATTR memória
 * túléli a timer-wakeöt. Ez a modul kezeli az abszolút ébredés-számlálót (cycle -> seq) és a
 * bootonkénti boot_id-t (reset-detektálás a masteren), valamint a fogyasztásbarát alvásba lépést
 * (log flush + USB-Serial-JTAG pad lekapcsolás + timer-wakeup). [M1 node-minta, éles-mérve.]
 */
#ifndef NODE_SLEEP_H
#define NODE_SLEEP_H

#include <stdint.h>
#include <stdbool.h>

/* Az aktuális ébredés állapota (a node_sleep_begin_cycle tölti). */
typedef struct {
    uint32_t cycle;      /* abszolút ébredés-számláló (0 hideg indulásnál, ++ minden wakeön) -> seq */
    uint32_t boot_id;    /* bootonkénti (hideg-indulásonkénti) azonosító -> reset-detektálás */
    bool     cold_boot;  /* true, ha ez hideg indulás (nem timer-wakeup) */
} node_wake_state_t;

/*
 * A ciklus elején hívandó: eldönti, timer-wakeupból jövünk-e (folytatjuk a számlálót) vagy
 * hideg indulás (cycle=0, új boot_id). Kitölti az *st-t.
 */
void node_sleep_begin_cycle(node_wake_state_t *st);

/*
 * Fogyasztásbarát deep sleepbe lép `seconds` másodpercre: log flush -> USB-JTAG pad off ->
 * timer-wakeup -> esp_deep_sleep_start(). NEM tér vissza (a wakeup app_main-t indít).
 */
void node_enter_deep_sleep(uint32_t seconds);

/*
 * Minimál deep sleep belépés izolációs méréshez: nem állít periféria GPIO-kat,
 * csak flush -> USB-Serial-JTAG pad off -> timer wake -> deep sleep.
 */
void node_enter_deep_sleep_minimal(uint32_t seconds);

#endif /* NODE_SLEEP_H */
