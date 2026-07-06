# RSSI-probe főkomponens — nincs wire-header függés (nem ESP-NOW, csak STA + TCP/UDP).
idf_component_register(
    SRCS "rssi_probe.c"
    INCLUDE_DIRS "."
)
