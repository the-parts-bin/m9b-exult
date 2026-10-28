#!/usr/bin/env bash
# Makes Ultima VII's music into the WAV files the port streams (see
# ../README.md, Sound), 24 kHz 16-bit mono, named as Exult names its
# digital music tracks (05BG.WAV, SI01.WAV, ENDCR01.WAV, ...), from either:
#
#  - Exult's MT-32 music pack (SOURCE holds its .ogg files, also in
#    subdirectories: U7MusicOGG_1of2/ and 2of2/ as unpacked): both games'
#    music as recorded from a Roland MT-32. The recordings' levels span
#    35 dB (-61 to -26 LUFS), so each track is brought to -18 LUFS (its
#    peak at most -1 dBFS), as the other games' music; the six that hold
#    only ambient sounds (wind, birds, dungeons: 04-08BG, 52BG, the pack's
#    readme) to -30, to stay in the background.
#  - The Black Gate's own AdLib music (SOURCE holds STATIC/), rendered: the
#    songs in STATIC/ADLIBMUS.DAT (main), INTROADM.DAT (intro, end credits)
#    and the AdLib ones in ENDSCORE.XMI, through libADLMIDI
#    (../core/tools/midirender: Nuked OPL3, the Fat Man General MIDI bank
#    with the game's own timbres from STATIC/XMIDI.AD over it, Miles'
#    volume model), each at a common loudness (finish.py). No Serpent Isle.
#
# Optionally copied to the SD card (U7/MUSIC/) with
# ../../core/tools/sdput/sdput.py (about 70 KB/s: 52 minutes for the MT-32
# pack's 217 MB, 40 for the AdLib renders' 165 MB).
#
# Usage: make_music.sh SOURCE OUTDIR [--upload | --to CARDDIR]
# Needs python3 and ffmpeg; the AdLib renders also git, cmake and a C/C++
# compiler.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
case "${3-}" in   # --upload: to the board's SD card; --to: into a card's file tree
"" | --upload) PUT=(put) ;;
--to) PUT=(put --to "${4:?--to needs a directory}") ;;
*) echo "unknown option $3" >&2; exit 1 ;;
esac
ST=$1/STATIC
OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
MR=$HERE/../../core/tools/midirender
mkdir -p "$OUT/wav"
upload() {
    [ -n "${3-}" ] || return 0
    args=()
    for f in "$OUT"/wav/*.WAV; do args+=("$f" "U7/MUSIC/$(basename "$f")"); done
    python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
}

if [ -n "$(find "$1" -iname '*.ogg' -print -quit)" ]; then   # the MT-32 pack
    find "$1" -iname '*.ogg' | sort > "$OUT/list.txt"
    while read -r f; do
        n=$(basename "${f%.*}" | tr '[:lower:]' '[:upper:]')
        case $n in 04BG | 05BG | 06BG | 07BG | 08BG | 52BG) t=-30 ;; *) t=-18 ;; esac
        printf 'python3 %q %q %q --target %s\n' "$MR/finish.py" "$f" "$OUT/wav/$n.WAV" $t
    done < "$OUT/list.txt" | xargs -P "$(nproc)" -d '\n' -I{} sh -c '{}' | sed 's|.*/||'
    du -sh "$OUT/wav"
    upload "$@"
    exit 0
fi

mkdir -p "$OUT/main" "$OUT/intro" "$OUT/raw"
"$MR/build.sh" >/dev/null
render() {   # IN NAME [midirender options]
    local in=$1 name=$2
    shift 2
    "$MR/build/midirender" "$in" "$OUT/raw/$name.wav" -b 0 -a "$ST/XMIDI.AD" -v ail -s 600 "$@" 2>/dev/null >/dev/null
    python3 "$MR/finish.py" "$OUT/raw/$name.wav" "$OUT/wav/$name.WAV"
}
python3 "$HERE/flexx.py" "$ST/ADLIBMUS.DAT" "$OUT/main" mid
python3 "$HERE/flexx.py" "$ST/INTROADM.DAT" "$OUT/intro" mid
for f in "$OUT"/main/*.mid; do
    n=$(basename "$f" .mid)
    [ "$(stat -c %s "$f")" -lt 100 ] && continue   # empty songs (49)
    render "$f" "${n}BG"
done
for n in 0 1 2 3; do render "$OUT/intro/0$n.mid" "0${n}BG"; done
render "$OUT/intro/04.mid" ENDCR01
render "$OUT/intro/05.mid" ENDCR02
render "$ST/ENDSCORE.XMI" END01BG -t 3   # the AdLib versions are songs 3 and 4
render "$ST/ENDSCORE.XMI" END02BG -t 4
du -sh "$OUT/wav"
upload "$@"
