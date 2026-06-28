# A főkomponens: az M1 node beacon forrás + a KÖZÖS header.
# A `../../common` a main/ komponensből az experiments/M1_drift_guard_window/common-ra mutat
# (main -> node -> M1_drift_guard_window/common). A CMAKE_CURRENT_LIST_DIR abszolút forma
# robusztus (nem a build cwd-től függ). Az esp_wifi / esp_now / nvs_flash / driver (temp-szenzor)
# IDF-komponensek a tranzitív függőségeken keresztül elérhetők; explicit REQUIRES nem szükséges.
idf_component_register(
    SRCS "m1_node_beacon.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
)
