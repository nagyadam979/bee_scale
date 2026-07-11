# bee_scale HX711 kalibrációs firmware (NODE-variáns) — ESP32-C3 SuperMini — ESP-IDF gyökér
#
# Tiszta ESP-IDF layout (gyökér CMakeLists.txt + main/ komponens). PlatformIO-val ÉS idf.py-vel
# is fordul. Önálló eszköz: a prod firmware/node olvasói a main/-be MÁSOLVA vannak.
#
# Fordítás (PlatformIO):  pio run -e cal_node
# vagy (idf.py):          idf.py set-target esp32c3 && idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(cal_hx711_node)
