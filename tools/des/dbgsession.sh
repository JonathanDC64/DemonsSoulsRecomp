#!/usr/bin/env bash
# dbgsession.sh <wait_sec> <outfile> <cmdfile> [ENV=val ...]
# Boots des_boot with the debug console, waits <wait_sec>, runs each line of
# <cmdfile> as a console command (a line "sleep N" pauses N seconds), prints
# the replies, then kills the game (a hung boot pins the CPU).
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
WAIT="$1"; OUT="$2"; CMDS="$3"; shift 3
D="$PWD/work/tmp/dbgcon.$$"; rm -f "$D" "$D.out"
rm -rf "D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)/cache"
tools/des/desrun.sh $((WAIT + 120)) "$OUT" PS3_DEBUG="$D" "$@" > /dev/null 2>&1 &
sleep "$WAIT"
while IFS= read -r c || [ -n "$c" ]; do
  [ -z "$c" ] && continue
  case "$c" in sleep\ *) sleep "${c#sleep }"; continue ;; esac
  echo "$c" > "$D"; sleep 2
done < "$CMDS"
sleep 1; cat "$D.out" 2>/dev/null
taskkill //F //IM des_boot.exe > /dev/null 2>&1
rm -f "$D" "$D.out"
