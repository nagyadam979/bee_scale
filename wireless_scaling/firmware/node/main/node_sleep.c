/*
 * node_sleep.c — lásd node_sleep.h. [Kiemelve az M1 node_beacon app_main-jéből, éles-mérve.]
 */
#include <stdio.h>
#include "node_sleep.h"
#include "esp_sleep.h"
#include "esp_random.h"
#include "esp_rom_sys.h"          /* esp_rom_delay_us() */
#include "esp_err.h"
#include "soc/soc_caps.h"         /* SOC_USB_SERIAL_JTAG_SUPPORTED */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "esp_private/sleep_console.h" /* sleep_console_usj_pad_backup_and_disable() */
#endif

/* RTC-memória érvényesség-jele: csak ha ezt találjuk wakeup után, folytatjuk a számlálót. */
#define NODE_RTC_MAGIC   0xBEE5CA1Eu

/* RTC-megőrzött állapot (timer-wakeöt túléli; hideg indulásnál szemét -> a magic dönt). */
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_cycle;
RTC_DATA_ATTR static uint32_t s_rtc_boot_id;

void node_sleep_begin_cycle(node_wake_state_t *st)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER && s_rtc_magic == NODE_RTC_MAGIC) {
        s_rtc_cycle++;
        st->cold_boot = false;
    } else {
        s_rtc_cycle   = 0;
        s_rtc_boot_id = esp_random();
        s_rtc_magic   = NODE_RTC_MAGIC;
        st->cold_boot = true;
    }
    st->cycle   = s_rtc_cycle;
    st->boot_id = s_rtc_boot_id;
}

void node_enter_deep_sleep(uint32_t seconds)
{
    /* A log kiürítése + rövid delay, MIELŐTT a USB-JTAG pad lekapcsol (különben az utolsó
     * sorok elvesznek). */
    fflush(stdout);
    esp_rom_delay_us(20000);   /* ~20 ms a soros kiírás befejezésére */

    /*
     * USB-Serial-JTAG pad LEKAPCSOLÁSA deep sleephez (KRITIKUS a C3 fogyasztására): a USJ pad
     * deep sleepben is bekapcsolva marad (~500-800 µA), az IDF ezt csak light sleepnél kapcsolja
     * le automatikusan. (Reset-ből ébredünk deep sleep után, ezért pad-restore nem kell.)
     */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL));
    esp_deep_sleep_start();   /* nincs visszatérés */
}
