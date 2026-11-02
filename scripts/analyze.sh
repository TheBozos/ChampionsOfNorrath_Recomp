#!/usr/bin/env bash
# Regenerates recomp/con.toml from the ELF with ps2_analyzer.
# WARNING: this overwrites hand edits in the TOML; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" "${ELF#$ROOT/}" "${CONFIG#$ROOT/}"
sed -i "s|^output = .*|output = \"${GENERATED#$ROOT/}/\"|" "$CONFIG"
