# A kalibrációs firmware fő komponense (MASTER). Minden forrás ÖNÁLLÓ (a prod olvasók ide másolva).
# REQUIRES: driver (gpio + usb_serial_jtag), esp_timer (pacing), esp_partition (nyers log-partíció),
#           vfs (stdin non-blocking a USB-Serial-JTAG konzolon).
idf_component_register(
    SRCS
        "cal_main.c"
        "sens_hx711.c"
        "sens_ds18b20.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_partition vfs
)
