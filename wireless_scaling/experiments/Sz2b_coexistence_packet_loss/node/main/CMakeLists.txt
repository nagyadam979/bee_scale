# A főkomponens: az Sz2b node forrás + a BEFAGYASZTOTT közös wire-header.
# A ../../../../firmware/common a main/ komponensből a wireless_scaling/firmware/common-ra
# mutat (main -> node -> Sz2b_coexistence_packet_loss -> experiments -> wireless_scaling).
idf_component_register(
    SRCS "sz2b_node.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/common"
)
