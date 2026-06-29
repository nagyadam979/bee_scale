# A főkomponens: az M1b node forrás. NINCS közös header (a node self-contained, nincs rádió).
# Az esp_driver_i2c (i2c_master.h), driver (gpio, temperature_sensor), nvs_flash, esp_timer
# IDF-komponensek a tranzitív függőségeken keresztül elérhetők; explicit REQUIRES nem szükséges.
idf_component_register(
    SRCS "m1b_node.c"
    INCLUDE_DIRS "."
)
