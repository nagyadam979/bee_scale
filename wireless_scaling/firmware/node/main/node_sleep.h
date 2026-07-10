/*
 * node_sleep.h — deep-sleep ciklus + RTC-megőrzött állapot
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A C3-on a deep sleep = teljes reboot (app_main újraindul), de az RTC_DATA_ATTR memória
 * túléli a timer-wakeöt. Ez a modul kezeli az ébredés-számlálót, a boot_id-t, a mastertől
 * szinkronizált epoch-becslést és a fogyasztásbarát alvásba lépést.
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
 * Az aktuális idő becsült Unix epoch másodperce. A becslés a legutóbbi master ACK epochjából,
 * az aktív időből és timer-wake esetén az előzőleg kért deep-sleep időből készül. Ha a node ezen
 * a bootláncon még nem kapott érvényes master időt, 0.
 */
uint32_t node_sleep_estimated_epoch_s(void);

/* Master ACK-ból érkező aktuális Unix epoch rögzítése. Hamis, ha az érték nem érvényes epoch. */
bool node_sleep_sync_epoch(uint32_t epoch_s);

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
