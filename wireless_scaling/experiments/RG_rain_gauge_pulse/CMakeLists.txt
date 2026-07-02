# RG — Esőmérő reed-impulzus kísérlet (WH-SP-RG tipping bucket) — ESP-IDF projekt gyökér
#
# Cél: a billenővödör-esőmérő reed-impulzusainak kiolvasása/számlálása a node-on.
# Target: ESP32-C3 SuperMini. Két mérési mód a forrásban (RG_MODE): padi kalibráló és
# deep-sleep wake-on-tip.
#
# Fordítás:
#   idf.py set-target esp32c3
#   idf.py build
#   idf.py -p <PORT> flash monitor

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rg_rain_gauge_pulse)
