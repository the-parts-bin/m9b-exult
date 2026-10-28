#!/usr/bin/env bash
# Copies Ultima VII to the SD card in the board, with
# ../../core/tools/sdput/sdput.py (about 64 KB/s), as U7/:
#   exult.cfg                 (from this directory: paths, 320 x 200, no MIDI)
#   EXULT/exult*.flx          (Exult's own data: get_data.sh)
#   BLACKGATE/STATIC/...      (--bg DIR: the Black Gate's STATIC/, e.g. ULTIMA7)
#   SERPENT/STATIC/...        (--si DIR: Serpent Isle's STATIC/, e.g. SERPENT)
#   EXULT/sqsfxbg.flx, sqsfxsi.flx (--sfx DIR: Exult's Roland MT-32 sound
#                             effects packs, found anywhere in DIR; Exult
#                             plays them instead of no sound effects)
# Either game can be left out (and its exult_*.flx with it). --no-cfg
# leaves the card's exult.cfg as it is (Exult writes its settings there).
# --list only prints what would be copied; --to CARDDIR copies into a
# card's tree.
#
# Usage: put_data.sh --flx DIR [--bg GAMEDIR] [--si GAMEDIR] [--sfx DIR] [--no-cfg]
#                    [--list] [--to CARDDIR]
#   GAMEDIR holds STATIC/ (the installed DOS game, e.g. ULTIMA7)
set -euo pipefail

HERE=$(dirname "$(realpath "$0")")
FLX="" BG="" SI="" SFX="" CFG=1 LIST=0
PUT=(put)
while [ $# -gt 0 ]; do
    case $1 in
    --flx) FLX=$2; shift 2 ;;
    --bg) BG=$2; shift 2 ;;
    --si) SI=$2; shift 2 ;;
    --sfx) SFX=$2; shift 2 ;;
    --no-cfg) CFG=0; shift ;;
    --list) LIST=1; shift ;;
    --to) PUT=(put --to "$2"); shift 2 ;;
    *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
    esac
done
if [ -z "$FLX" ] || { [ -z "$BG" ] && [ -z "$SI" ]; }; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

args=("$FLX/exult.flx" "U7/EXULT/exult.flx")
[ $CFG = 0 ] || args+=("$HERE/exult.cfg" "U7/exult.cfg")
add_game() {   # add_game GAMEDIR CARDDIR FLX SFXPACK
    [ -n "$1" ] || return 0
    local static sfx
    static=$(find "$1" -maxdepth 1 -iname static -type d | head -1)
    [ -n "$static" ] || { echo "no STATIC/ in $1" >&2; exit 1; }
    args+=("$FLX/$3" "U7/EXULT/$3")
    if [ -n "$SFX" ]; then
        sfx=$(find "$SFX" -iname "$4" -type f | head -1)
        [ -n "$sfx" ] || { echo "no $4 in $SFX" >&2; exit 1; }
        args+=("$sfx" "U7/EXULT/$4")
    fi
    for f in "$static"/*; do
        [ -f "$f" ] && args+=("$f" "U7/$2/STATIC/$(basename "$f" | tr '[:lower:]' '[:upper:]')")
    done
}
add_game "$BG" BLACKGATE exult_bg.flx sqsfxbg.flx
add_game "$SI" SERPENT exult_si.flx sqsfxsi.flx

total=0
for ((i = 0; i < ${#args[@]}; i += 2)); do total=$((total + $(stat -c %s "${args[i]}"))); done
echo "$((${#args[@]} / 2)) files, $((total / 1048576)) MB, about $((total / 65536 / 60 + 1)) minutes"
if [ $LIST = 1 ]; then
    printf '%s -> %s\n' "${args[@]}" | head -20
    exit 0
fi
python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
