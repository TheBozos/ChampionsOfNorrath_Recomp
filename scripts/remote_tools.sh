#!/usr/bin/env bash
# Builds ps2_recomp and ps2_analyzer on a remote x86-64 Linux box over ssh and copies them back into build/tools, where
# scripts/analyze.sh and scripts/recomp.sh find them.
#   REMOTE_HOST   ssh host (default stallman)
#   REMOTE_DIR    work directory on the host, relative to its home (default con-remote)
#   REMOTE_TOOLS  mise tools the build runs with (default: latest cmake and ninja)
source "$(dirname "$0")/common.sh"
HOST="${REMOTE_HOST:-stallman}"
RDIR="${REMOTE_DIR:-con-remote}"
TOOLS="${REMOTE_TOOLS:-cmake@latest ninja@latest}"

echo "== PS2Recomp to $HOST:$RDIR/tools/src"
ssh "$HOST" mkdir -p "$RDIR/tools/src"
rsync -aL --delete --exclude=.git --exclude='/build*/' "$ROOT/PS2Recomp/" "$HOST:$RDIR/tools/src/"

echo "== build on $HOST"
args=$(printf '%q ' "$RDIR" "$TOOLS")
ssh "$HOST" "bash -s -- $args" <<'REMOTE'
set -euo pipefail
work=$HOME/$1/tools
read -ra tools <<< "$2"
MISE_JOBS=1 mise install "${tools[@]}"
cmake=(mise exec "${tools[@]}" -- cmake)
"${cmake[@]}" -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
"${cmake[@]}" --build "$work/build" --target ps2_recomp ps2_analyzer -j "$(nproc)"
REMOTE

echo "== tools to $TOOLS_BUILD"
mkdir -p "$TOOLS_BUILD/ps2xRecomp" "$TOOLS_BUILD/ps2xAnalyzer"
rsync -tz "$HOST:$RDIR/tools/build/ps2xRecomp/ps2_recomp" "$TOOLS_BUILD/ps2xRecomp/"
rsync -tz "$HOST:$RDIR/tools/build/ps2xAnalyzer/ps2_analyzer" "$TOOLS_BUILD/ps2xAnalyzer/"
