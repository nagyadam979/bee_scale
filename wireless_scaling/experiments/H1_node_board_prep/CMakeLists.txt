# H1 — Node board előkészítés (deep-sleep áram) — ESP-IDF projekt gyökér
#
# Cél: minimális firmware a node nyers deep-sleep áramának méréséhez.
# Target: ESP32-C3 SuperMini. Rádió NINCS (csak deep sleep + HX711 power-down).
#
# Fordítás:
#   idf.py set-target esp32c3
#   idf.py build
#   idf.py -p <PORT> flash monitor   # a boot-logot kiírja, majd deep sleepbe megy

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(h1_node_board_prep)
