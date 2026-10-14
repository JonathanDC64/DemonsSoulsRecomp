#!/usr/bin/env bash
# dbgsession.sh <wait_sec> <outfile> <cmdfile> [ENV=val ...]
# Boots des_boot with the debug console, waits <wait_sec>, runs each line of
# <cmdfile> as a console command (a line "sleep N" pauses N seconds), prints
# the replies, then kills the game (a hung boot pins the CPU).
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
WAIT="$1"; OUT="$2"; CMDS="$3"; shift 3
D="$DES_SCRATCH/dbgcon.$$"; rm -f "$D" "$D.out"
tools/des/desrun.sh $((WAIT + 120)) "$OUT" PS3_DEBUG="$D" "$@" > /dev/null 2>&1 &
sleep "$WAIT"
while IFS= read -r c || [ -n "$c" ]; do
  [ -z "$c" ] && continue
  case "$c" in sleep\ *) sleep "${c#sleep }"; continue ;; esac
  des_console "$D" "$c"
done < "$CMDS"
des_kill_own
rm -f "$D" "$D.out"
