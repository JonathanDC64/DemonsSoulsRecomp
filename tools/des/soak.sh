#!/usr/bin/env bash
# soak.sh <tag> [exe [ENV...]]: a ~6 minute run at DES_FPS=120, walking left
# and right with `stat` lines in between, for checking a change to the
# atomics (short runs miss ordering bugs; the Nexus freeze took minutes to
# show). Plays from LC_SAVE like levelcap.sh: point it at a Nexus save, or set
# it empty for the current save. Result: $DES_SCRATCH/<tag>.sum (the flip
# count must keep rising from one stat to the next); the full log is in
# $DES_SCRATCH/lc_<tag>/.
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
TAG=$1; X=${2:-port/build/des_boot.exe}; shift $(( $# < 2 ? $# : 2 ))
CMDS=$DES_SCRATCH/soak_cmds.txt; : > "$CMDS"
for i in 1 2 3 4; do printf '%s\n' stat "sleep 30" stat "sleep 30" stat "stick 200 128 8" "stick 56 128 8" >> "$CMDS"; done
echo stat >> "$CMDS"
export RSX_PRESENT=${RSX_PRESENT:-$(des_session_locked && echo 0 || echo 1)}
DES_EXE=$X LC_NOPRESS=1 LC_SETTLE=20 LC_MAX=700 LC_DUMP=0 \
  bash tools/des/levelcap.sh "$TAG" "$CMDS" PS3_PRIORITY=normal PS3_AFFINITY=all DES_AUTO_CONTINUE=1 DES_FPS=120 "$@" \
  > "$DES_SCRATCH/$TAG.lc" 2>&1
{ grep -a "luabnd.dcx.sdat), pad" "$DES_SCRATCH/lc_$TAG/run.txt" | head -2
  grep -aE "^\s+flips|uptime" "$DES_SCRATCH/$TAG.lc"
  echo finished; } > "$DES_SCRATCH/$TAG.sum"
