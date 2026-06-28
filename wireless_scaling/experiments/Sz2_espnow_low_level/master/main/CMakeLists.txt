# A főkomponens: az Sz2 master RX forrás + a KÖZÖS header.
# A `../../common` a main/ komponensből az experiments/Sz2_espnow_low_level/common-ra mutat
# (main -> master -> Sz2_espnow_low_level/common). A CMAKE_CURRENT_LIST_DIR abszolút forma
# robusztus. Az esp_wifi / esp_now / nvs_flash IDF-komponensek tranzitívan elérhetők.
idf_component_register(
    SRCS "sz2_master_rx.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
)
