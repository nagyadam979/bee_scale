# bee_scale HX711 kalibrációs firmware (MASTER-variáns) — ESP32-S3-Zero — ESP-IDF gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. Önálló eszköz: a prod firmware/master olvasói a main/-be MÁSOLVA/ÁTEMELVE vannak.
#
# Fordítás (PlatformIO):  pio run -e cal_master
# vagy (idf.py):          idf.py set-target esp32s3 && idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(cal_hx711_master)
