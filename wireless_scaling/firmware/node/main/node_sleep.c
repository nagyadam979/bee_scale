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
#include "driver/gpio.h"
#include "node_config.h"
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

static void prepare_peripheral_gpios_for_sleep(void)
{
    /*
     * ESP32-C3 deep sleepben a nem holdolt GPIO-k alapból high-Z állapotba kerülnek.
     * Ezért nem piszkáljuk tömegesen az összes lábat: csak a külső fogyasztót vezérlő
     * kimeneteket tesszük determinisztikus állapotba.
     */
#if VBAT_MEASURE_ENABLED
    gpio_config_t vbat_cfg = {
        .pin_bit_mask = (1ULL << VBAT_SW_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&vbat_cfg));
#if VBAT_KEEP_DIVIDER_ON_IN_SLEEP
    ESP_ERROR_CHECK(gpio_set_level(VBAT_SW_GPIO, 1));   /* NPN ON -> GPIO0 = VBAT/2, nincs ADC túlfesz */
#else
    ESP_ERROR_CHECK(gpio_set_level(VBAT_SW_GPIO, 0));   /* NPN OFF -> osztó le, de GPIO0 VBAT-ra úszhat */
#endif
    gpio_hold_en(VBAT_SW_GPIO);
#endif

    /* A LED lábat nem holdoljuk: boardfüggő polaritás/kötés miatt a tartott szint mA-t vihet. */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << USER_LED_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));

#if DS18B20_HOLD_HIGH_IN_SLEEP
    gpio_config_t onewire_cfg = {
        .pin_bit_mask = (1ULL << DS18B20_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&onewire_cfg));
    ESP_ERROR_CHECK(gpio_set_level(DS18B20_GPIO, 1));
    gpio_hold_en(DS18B20_GPIO);
#elif DS18B20_ENABLED
    /* 1-Wire busz release: a külső 4.7k felhúzó tartja high-on, az ESP nem hajtja. */
    gpio_config_t onewire_cfg = {
        .pin_bit_mask = (1ULL << DS18B20_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&onewire_cfg));
#endif

    gpio_deep_sleep_hold_en();
}

void node_enter_deep_sleep(uint32_t seconds)
{
    prepare_peripheral_gpios_for_sleep();

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

void node_enter_deep_sleep_minimal(uint32_t seconds)
{
    fflush(stdout);
    esp_rom_delay_us(20000);

#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL));
    esp_deep_sleep_start();
}
