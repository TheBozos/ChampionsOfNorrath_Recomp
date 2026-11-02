#!/usr/bin/env bash
# Extracts the files of your own Champions of Norrath (NTSC-U, SLUS-20565) disc image into game/, both layers of the
# dual-layer DVD, with PS2Recomp/tools/ps2_disc.py, and links the image there as disc.iso: the game reads its level
# folders by sector number.
# Usage: scripts/extract_iso.sh <image.iso>
source "$(dirname "$0")/common.sh"
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <image.iso>"; exit 1; }
mkdir -p "$GAME_DIR"
python3 "$ROOT/PS2Recomp/tools/ps2_disc.py" extract "$1" "$GAME_DIR"
[ -f "$ELF" ] || { echo "no SLUS_205.65 in $1: not the NTSC-U disc?"; exit 1; }
ln -sfn "$(realpath "$1")" "$GAME_DIR/disc.iso"
expected=89fc400cf533d3eacca4264c8c315ad30d9fd735
actual=$(sha1sum "$ELF" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || echo "warning: SLUS_205.65 SHA-1 is $actual, the port is built against $expected"
echo "Disc files in $GAME_DIR"
