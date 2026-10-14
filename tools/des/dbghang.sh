#!/usr/bin/env bash
# dbghang.sh <max_sec> <outfile> <cmdfile> [ENV=val ...]
# Boots des_boot with the debug console and polls `stat` every 8 s. Once the
# flip count stops moving (a hang), runs each line of <cmdfile> as a console
# command, prints the replies, then kills the game. Gives up after <max_sec>.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
MAX="$1"; OUT="$2"; CMDS="$3"; shift 3
D="$DES_SCRATCH/dbgcon.$$"; rm -f "$D" "$D.out"
tools/des/desrun.sh $((MAX + 60)) "$OUT" PS3_DEBUG="$D" "$@" > /dev/null 2>&1 &
sleep 30
last=-1; t=30; hung=0
while [ $t -lt "$MAX" ]; do
  des_console "$D" stat > "$D.stat"
  f=$(grep -a "flips" "$D.stat" 2>/dev/null | awk '{print $2}')
  echo "t=$t flips=$f"
  if [ -n "$f" ] && [ "$f" = "$last" ] && [ "$f" != 0 ]; then hung=1; break; fi
  last=$f; sleep 6; t=$((t + 8))
done
if [ $hung = 1 ]; then
  rm -f "$D.out"
  while IFS= read -r c || [ -n "$c" ]; do
    [ -z "$c" ] && continue
    case "$c" in sleep\ *) sleep "${c#sleep }"; continue ;; esac
    des_console "$D" "$c"
  done < "$CMDS"
else echo "no hang within $MAX s"; fi
des_kill_own
rm -f "$D" "$D.out"
