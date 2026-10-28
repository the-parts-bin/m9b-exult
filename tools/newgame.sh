#!/usr/bin/env bash
# Loads a build (default build-prof/), picks the Black Gate, skips the intro
# and starts a new game from the UART's keys (the same steps tools/host's
# scripts take), waits in Trinsic and takes a screenshot (/tmp/u7.png). For
# tests: the SD card must hold the data (put_data.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-prof}
python3 ../core/tools/run.py "$B/exult.bin" --listen 40 > /dev/null 2>&1
# Enter: the first game (the Black Gate); Esc skips the intro; Up, Enter:
# Start New Game; a name; Down, Down, Space: Journey Onward.
python3 ../core/tools/m9bctl.py keys "<enter><wait:6000><esc><wait:4000><up><wait:500><enter><wait:3000>Avatar<wait:1000><down><wait:500><down><wait:500><space>"
sleep 40
python3 ../core/tools/m9bctl.py shot /tmp/u7.png --scale 2
