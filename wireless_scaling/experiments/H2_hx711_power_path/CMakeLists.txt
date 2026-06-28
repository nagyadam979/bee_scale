# H2 — HX711 power-path (excitation-lekapcsolás) — ESP-IDF projekt gyökér
#
# Cél: minimális firmware a HX711-M modul nyugalmi áramának és az excitation
# lekapcsolásának (0 V a cellán) méréséhez. A HX711 modul + load cell BE van kötve.
# Target: ESP32-C3 SuperMini. Rádió NINCS (csak deep sleep + HX711 power-down).
#
# Fordítás:
#   idf.py set-target esp32c3
#   idf.py build
#   idf.py -p <PORT> flash monitor   # a boot-logot kiírja, majd deep sleepbe megy

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(h2_hx711_power_path)
