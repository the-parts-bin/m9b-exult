#!/usr/bin/env bash
# Builds Exult's own data files (fonts, menus, paperdolls, ...: exult.flx,
# exult_bg.flx, exult_si.flx) into DIR (default: ../data/exult): fetches the
# sources (data/ of Exult v1.12.1, which isn't in this tree: 14 MB) and
# packs them with Exult's expack, built for the host from ../exult. The
# headers the build needs (../exult/data/*.h) come from the same step and
# are in the tree already. Needs git and g++.
#
# Usage: get_data.sh [DIR]
set -euo pipefail

HERE=$(dirname "$(realpath "$0")")
EX=$HERE/../exult
DEST=$(realpath -m "${1:-$HERE/../data/exult}")
TAG=v1.12.1
mkdir -p "$DEST"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Building expack"
(cd "$EX" && g++ -O1 -std=gnu++17 -w -DHAVE_CONFIG_H -DEXULT_DATADIR='"EXULT"' -I"$HERE/../port" -I. \
    -Iheaders -Ifiles -Iconf tools/expack.cc \
    $(ls files/*.cc | grep -v -e sdlrwops -e rwregress) files/sha1/sha1.cpp -o "$TMP/expack")

echo "Fetching data/ of Exult $TAG"
git clone -q --depth 1 --branch "$TAG" --filter=blob:none --sparse https://github.com/exult/exult.git "$TMP/exult"
git -C "$TMP/exult" sparse-checkout set data

echo "Packing"
cd "$TMP/exult/data"
for i in fonts/original.in fonts/serif.in shortcutbar.in bg/bg_mr_faces.in bg/bg_paperdol.in \
         bg/introsfx_mt.in bg/introsfx_sb.in flx.in bg/flx.in si/flx.in; do
    [ -f "$i" ] && "$TMP/expack" -i "$i" > /dev/null
done
cp exult.flx exult_bg.flx exult_si.flx "$DEST/"
ls -l "$DEST"/exult*.flx
