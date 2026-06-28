# A főkomponens: az Sz2 node TX forrás + a KÖZÖS header.
# A `../../common` a main/ komponensből az experiments/Sz2_espnow_low_level/common-ra mutat
# (main -> node -> Sz2_espnow_low_level/common). A CMAKE_CURRENT_LIST_DIR abszolút forma
# robusztus (nem a build cwd-től függ). Az esp_wifi / esp_now / nvs_flash IDF-komponensek a
# tranzitív függőségeken keresztül elérhetők; explicit REQUIRES nem szükséges.
idf_component_register(
    SRCS "sz2_node_tx.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
)
