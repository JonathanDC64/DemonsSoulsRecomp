#!/usr/bin/env bash
# dbghang.sh <max_sec> <outfile> <cmdfile> [ENV=val ...]
# Boots des_boot with the debug console and polls `stat` every 8 s. Once the
# flip count stops moving (a hang), runs each line of <cmdfile> as a console
# command, prints the replies, then kills the game. Gives up after <max_sec>.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
MAX="$1"; OUT="$2"; CMDS="$3"; shift 3
D="$PWD/work/tmp/dbgcon.$$"; rm -f "$D" "$D.out"
rm -rf "D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)/cache"
tools/des/desrun.sh $((MAX + 60)) "$OUT" PS3_DEBUG="$D" "$@" > /dev/null 2>&1 &
sleep 30
last=-1; t=30; hung=0
while [ $t -lt "$MAX" ]; do
  rm -f "$D.out"; echo stat > "$D"; sleep 2
  f=$(grep -a "flips" "$D.out" 2>/dev/null | awk '{print $2}')
  echo "t=$t flips=$f"
  if [ -n "$f" ] && [ "$f" = "$last" ] && [ "$f" != 0 ]; then hung=1; break; fi
  last=$f; sleep 6; t=$((t + 8))
done
if [ $hung = 1 ]; then
  rm -f "$D.out"
  while IFS= read -r c || [ -n "$c" ]; do
    [ -z "$c" ] && continue
    case "$c" in sleep\ *) sleep "${c#sleep }"; continue ;; esac
    echo "$c" > "$D"; sleep 2
  done < "$CMDS"
  sleep 1; cat "$D.out" 2>/dev/null
else echo "no hang within $MAX s"; fi
taskkill //F //IM des_boot.exe > /dev/null 2>&1
rm -f "$D" "$D.out"
