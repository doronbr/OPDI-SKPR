#!/usr/bin/env bash
set -euo pipefail

PORT="${IDF_PORT:-${1:-/dev/ttyUSB0}}"
IDF_ROOT_HINT="${IDF_ROOT_HINT:-$HOME/esp}"

ensure_idf() {
  if command -v idf.py >/dev/null 2>&1; then return; fi
  # Probe likely installs (customize as needed)
  for cand in \
    "$IDF_PATH" \
    "$IDF_ROOT_HINT/esp-idf" \
    "$IDF_ROOT_HINT/v5.5/esp-idf" \
    "/opt/esp/idf/v5.5/esp-idf"; do
    [ -n "${cand}" ] || continue
    if [ -d "$cand" ] && [ -f "$cand/export.sh" ]; then
      export IDF_PATH="$cand"
      # shellcheck disable=SC1090
      . "$cand/export.sh" >/dev/null
      break
    fi
  done
  command -v idf.py >/dev/null 2>&1 || { echo "ERROR: Unable to locate esp-idf (set IDF_PATH)" >&2; exit 1; }
}

ensure_idf

echo "Using port $PORT"

if ! idf.py --version 2>/dev/null | grep -q esp32p4; then
  echo "Selecting target esp32p4"
  idf.py set-target esp32p4
fi

if [ ! -f build/CMakeCache.txt ]; then
  echo "Initial configure"
  idf.py reconfigure
fi

echo "Building"
idf.py build | tee build/build.log

echo "Flashing"
idf.py -p "$PORT" flash

if [ "${NO_MONITOR:-0}" != "1" ]; then
  echo "Monitoring (Ctrl+] then q to exit)"
  idf.py -p "$PORT" monitor
fi
