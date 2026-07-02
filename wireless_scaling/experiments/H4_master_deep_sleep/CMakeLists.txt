# H4 — Master board deep-sleep áram (ESP32-S3-Zero) — ESP-IDF projekt gyökér
#
# Cél: minimális firmware a master nyers deep-sleep áramának méréséhez.
# Target: ESP32-S3 (Waveshare S3-Zero). Rádió NINCS (csak deep sleep + WS2812 data-low).
#
# Fordítás (idf.py):
#   idf.py set-target esp32s3
#   idf.py build
#   idf.py -p <PORT> flash monitor   # boot-log, majd tartós deep sleep
#
# Fordítás/flash PlatformIO-val: lásd platformio.ini.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(h4_master_deep_sleep)
