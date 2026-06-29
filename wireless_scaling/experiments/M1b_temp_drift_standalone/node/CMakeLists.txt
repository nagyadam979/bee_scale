# M1b NODE (standalone hő-drift) — ESP32-C3 SuperMini — ESP-IDF gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. NINCS közös header (NINCS rádió/protokoll) — a node self-contained.
#
# Fordítás (PlatformIO):  pio run
# vagy (idf.py):          idf.py set-target esp32c3 && idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(m1b_node)
