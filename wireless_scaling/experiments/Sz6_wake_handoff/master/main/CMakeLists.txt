# A főkomponens: az Sz6 master forrás + a BEFAGYASZTOTT közös wire-header.
# A ../../../../firmware/common a main/ komponensből a wireless_scaling/firmware/common-ra
# mutat (main -> master -> Sz6_wake_handoff -> experiments -> wireless_scaling).
idf_component_register(
    SRCS "sz6_master.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/common"
)
