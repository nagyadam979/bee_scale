# A főkomponens: az Sz6 node forrás + a BEFAGYASZTOTT közös wire-header.
# A ../../../../firmware/common a main/ komponensből a wireless_scaling/firmware/common-ra
# mutat (main -> node -> Sz6_wake_handoff -> experiments -> wireless_scaling). A
# CMAKE_CURRENT_LIST_DIR abszolút forma robusztus (nem a build cwd-től függ).
idf_component_register(
    SRCS "sz6_node.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/common"
)
