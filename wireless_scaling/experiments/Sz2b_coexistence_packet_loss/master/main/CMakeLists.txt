# A főkomponens: az Sz2b master forrás + a BEFAGYASZTOTT közös wire-header.
# A ../../../../firmware/common a main/ komponensből a wireless_scaling/firmware/common-ra
# mutat (main -> master -> Sz2b_coexistence_packet_loss -> experiments -> wireless_scaling).
idf_component_register(
    SRCS "sz2b_master.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/common"
)
