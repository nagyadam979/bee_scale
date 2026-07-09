idf_component_register(
    SRCS "master.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
    REQUIRES esp_wifi esp_http_client esp-tls nvs_flash esp_timer json lwip app_update
)
