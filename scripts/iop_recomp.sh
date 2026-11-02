#!/usr/bin/env bash
# Recompiles CoN's IRX modules to native C++ in generated/iop/ with PS2Recomp's tools/iop_recomp.py, run with
# PS2X_IOP_NATIVE=1 (the default, see runtime/con_overrides.cpp). The recompiler works from each module as the IOP
# loaded it, relocated at its base, so this first boots the game with PS2X_IOP_DUMP_MODULES into build/iop_modules/.
# Like the rest of generated/, the output is derived from the game and never committed. Rebuild the runner after it.
# A game run: take the bench lock around it.
#   scripts/iop_recomp.sh [seconds of boot, default 60]
source "$(dirname "$0")/common.sh"
cd "$ROOT"
SECONDS_TO_BOOT=${1:-60}
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
DUMPS="$ROOT/build/iop_modules"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
rm -rf "$DUMPS" "$GENERATED/iop" && mkdir -p "$DUMPS" "$GENERATED/iop" build/iop_user
PS2X_USER_DIR="$ROOT/build/iop_user" PS2X_NO_DIALOGS=1 PS2X_WINDOW_NO_FOCUS=1 PS2X_IOP_NATIVE=0 PS2X_NATIVE_AUDIO=0 \
    PS2X_IOP_HARDWARE=1 PS2X_IOP_DUMP_MODULES="$DUMPS" timeout "$SECONDS_TO_BOOT" "$BIN" "$ELF" > build/iop_dump.log 2>&1 || true
for json in "$DUMPS"/*.json; do
    module=$(basename "$json" .json)
    python3 PS2Recomp/tools/iop_recomp.py "$json" --out "$GENERATED/iop" | grep -E '^[^ ]|functions,'
    python3 PS2Recomp/tools/iop_recomp.py "$json" --list > "build/iop_${module}_list.txt"
done
echo "$(ls "$GENERATED/iop" | wc -l) recompiled IOP modules in $GENERATED/iop"
