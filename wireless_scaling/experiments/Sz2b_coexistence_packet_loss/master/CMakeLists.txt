# Sz2b MASTER — ESP32-S3-Zero — ESP-IDF projekt gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. A közös wire-header a firmware/common/protocol.h, a main/CMakeLists.txt húzza be.
#
# Fordítás (PlatformIO):  pio run -e p3-sta-traffic   (fázisonkénti env-ek, lásd platformio.ini)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sz2b_master)
