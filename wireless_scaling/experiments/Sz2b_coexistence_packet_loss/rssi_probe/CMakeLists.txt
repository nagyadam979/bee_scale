# Sz2b RSSI-PROBE — antenna-diagnosztikai firmware (S3-Zero / C3 SuperMini közös forrás)
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens), PlatformIO-val fordítva.
# Fordítás: pio run -e s3zero  /  pio run -e c3super

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rssi_probe)
