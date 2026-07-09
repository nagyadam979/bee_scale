#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ $# -gt 0 ]]; then
  port="$1"
else
  ports=()
  for p in /dev/cu.usbmodem*; do
    [[ -e "$p" ]] || continue
    ports+=("$p")
  done

  if [[ ${#ports[@]} -eq 0 ]]; then
    echo "Nem talaltam /dev/cu.usbmodem* portot. Dugd be a C3-at, majd probald ujra." >&2
    exit 1
  fi

  if [[ ${#ports[@]} -gt 1 ]]; then
    echo "Tobb usbmodem port van bedugva, nem valasztok automatikusan:" >&2
    printf '  %s\n' "${ports[@]}" >&2
    echo "Hasznalat konkret porttal: $0 /dev/cu.usbmodemXXXXX" >&2
    exit 1
  fi

  port="${ports[0]}"
fi

echo "Upload port: ${port}"
exec ~/.platformio/penv/bin/pio run -e esp32-c3-supermini -t upload --upload-port "${port}"
