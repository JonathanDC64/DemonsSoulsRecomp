#!/usr/bin/env bash
# loads.sh [n] [ENV=val ...]: a session of n (default 5) level loads, Quit Game and continue each time, on
# the game's own route (DES_ROUTE=loads, port/des_route.cpp), from a copy of regress.sh's pinned save. It
# is for changes to what a load starts and stops (threads, the SPURS queues of docs/133), which one boot
# does not repeat. DES_EXE picks the build, as for desrun.sh; ENV goes to the game.
# Out: $DES_SCRATCH/loads_run/: route/route.txt names each load and how long it took, route/summary.json
# counts them ("loads"), route/load<i>.bmp is a shot of each level, run.txt is the game's log.
# Prints the loads, the route's verdict and the log's freeze, crash and lost wake-up lines. Exit status:
# 0 every load anchored and the route ended ok, 1 not, 3 no pinned save or a build too old for one.
#   tools/des/loads.sh             tools/des/loads.sh 10 LIBSRE_OWN=0
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
N=5
case "${1:-}" in ""|*[!0-9]*) ;; *) N="$1"; shift ;; esac
FROM="${REGRESS_DIR:-$DES_SCRATCH/regress}/save"
[ -d "$FROM" ] || { echo "[loads] no pinned save ($FROM): see regress.sh pin-save"; exit 3; }
# The game plays from its own copy (PS3_SAVEDATA_ROOT): a Quit Game saves, and the player's save is never touched.
des_save_root_ok || { echo "[loads] this build does not read PS3_SAVEDATA_ROOT; use a newer one"; exit 3; }
RUN="$DES_SCRATCH/loads_run"
rm -rf "$RUN"; mkdir -p "$RUN"
r=$(des_run_save "$FROM" "$RUN/saves") || { echo "[loads] could not copy $FROM for the run"; exit 3; }
tools/des/desrun.sh "${LOADS_MAX:-$((180 + N * 120))}" "$RUN/run.txt" DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 \
  PS3_SAIL_MOVIES=0 PS3_SAVEDATA_ROOT="$r" DES_ROUTE=loads DES_ROUTE_LOADS="$N" \
  DES_ROUTE_OUT="$(cygpath -m "$RUN/route")" DES_ROUTE_QUIT=1 DES_ROUTE_WAIT="${LOADS_WAIT:-300}" "$@" \
  > "$RUN/desrun.txt" 2>&1
SUM="$RUN/route/summary.json"
res=$(grep -o '"result": "[a-z_]*"' "$SUM" 2>/dev/null | cut -d'"' -f4)
got=$(grep -o '"loads": [0-9]*' "$SUM" 2>/dev/null | awk '{print $2}')
grep -a -E ' (load [0-9]+:|continue: no level|no level after|done:)' "$RUN/route/route.txt" 2>/dev/null
echo "[loads] ${got:-0} of $N loads, route ${res:-did not finish}; freeze lines $(grep -a -c '\[freeze\]' "$RUN/run.txt"), crash lines $(grep -a -c '\[crash\]' "$RUN/run.txt"), lost wake-up lines $(grep -a -c 'spurs-queues\] lost wake-up' "$RUN/run.txt")"
[ "${res:-}" = ok ] && [ "${got:-0}" = "$N" ]
