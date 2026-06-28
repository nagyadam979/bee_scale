# A főkomponens: az M1 master logger forrás + a KÖZÖS header.
# A `../../common` a main/ komponensből az experiments/M1_drift_guard_window/common-ra mutat
# (main -> master -> M1_drift_guard_window/common). A CMAKE_CURRENT_LIST_DIR abszolút forma
# robusztus. Az esp_wifi / esp_now / nvs_flash IDF-komponensek tranzitívan elérhetők.
idf_component_register(
    SRCS "m1_master_logger.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
)
