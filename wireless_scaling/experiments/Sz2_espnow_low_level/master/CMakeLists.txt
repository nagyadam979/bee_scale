# Sz2 MASTER (RX) — ESP-NOW low-level recv · classic ESP-WROOM-32 — ESP-IDF projekt gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. A közös header az ../common-ban van, azt a main/CMakeLists.txt INCLUDE_DIRS húzza be.
#
# Fordítás (PlatformIO):  pio run
# vagy (idf.py):          idf.py set-target esp32 && idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sz2_master_rx)
