#!/bin/zsh
# flash_catch.sh — alvó (deep sleep-elő) C3 node felflashelése az awake-ablak elkapásával.
#
# Működés: 1) előfordítás; 2) catch_bootloader.py — a port felbukkanásakor ~20 ms-en belül
# beadja a download-mód DTR/RTS szekvenciát, a chip a ROM bootloaderben ragad (nem alszik);
# 3) sima upload a stabil bootloaderre.
#
# Miért nem sima upload-retry: a pio/esptool hidegindítása ~2-5 s, a node awake-ablaka
# ~1-1,5 s — a verseny rendre elveszett (kimérve 2026-07-05).
#
# Használat:  ./flash_catch.sh <projekt_dir> <env> <port> [catch_timeout_s]
# FIGYELEM: a portot fogó serial_logger.py-t ELŐBB le kell állítani (exkluzív nyitás).

set -u
PROJ="$1"; ENV="$2"; PORT="$3"; CATCH_TIMEOUT="${4:-120}"
PIO=~/.platformio/penv/bin/pio
PY=~/.platformio/penv/bin/python
TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$PROJ" || exit 2

# Előfordítás, hogy az elkapás után már csak az upload fusson.
"$PIO" run -e "$ENV" >/dev/null 2>&1 || { echo "BUILD FAIL"; exit 3; }

for attempt in 1 2 3; do
    echo "[$attempt/3] bootloader-elkapas: $PORT ..."
    if ! "$PY" "$TOOLS_DIR/catch_bootloader.py" "$PORT" "$CATCH_TIMEOUT"; then
        continue
    fi
    if "$PIO" run -e "$ENV" -t upload --upload-port "$PORT" >/tmp/flash_catch_$$.log 2>&1; then
        echo "UPLOAD OK ($ENV -> $PORT)"
        rm -f /tmp/flash_catch_$$.log
        exit 0
    fi
    echo "upload hiba, ujraprobalas:"
    tail -5 /tmp/flash_catch_$$.log
done
echo "UPLOAD FAIL: 3 elkapasi kiserlet sem sikerult"
rm -f /tmp/flash_catch_$$.log
exit 1
