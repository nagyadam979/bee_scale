# Sz6 MASTER — ESP32-S3-Zero — ESP-IDF projekt gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. A közös wire-header a firmware/common/protocol.h, a main/CMakeLists.txt húzza be.
#
# Fordítás (PlatformIO):  pio run
# vagy (idf.py):          idf.py set-target esp32s3 && idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sz6_master)
