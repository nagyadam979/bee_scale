# A node fő komponense: a firmware forrásai + a KÖZÖS header.
# A `../../common` a main/ komponensből a firmware/common-ra mutat (main -> node -> firmware
# -> common). A CMAKE_CURRENT_LIST_DIR abszolút forma robusztus (nem a build cwd-től függ).
# Az esp_wifi / esp_now / nvs_flash / esp_adc / driver IDF-komponensek a tranzitív
# függőségeken keresztül elérhetők; explicit REQUIRES nem szükséges.
idf_component_register(
    SRCS
        "main.c"
        "node_sleep.c"
        "node_buffer.c"
        "espnow_tx.c"
        "sens_hx711.c"
        "sens_vbat.c"
        "sens_ds18b20.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
)
