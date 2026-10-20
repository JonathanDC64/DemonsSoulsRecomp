#!/usr/bin/env bash
# abboot.sh: sourced by knobab.sh and kprof.sh. Boots LC_SAVE (levelcap.sh;
# default the pinned regression save) in the background with an idle command
# file (AB_IDLE seconds, default 1500; the run cap is LC_MAX, default 900),
# waits for the level and then KAB_SETTLE seconds (default 40; when the
# caller sourced perflib.sh first, a cutscene that plays meanwhile is pressed
# through), and defines
#   con <command> [cap-secs]   send a console command and print its reply
#   ab_finish                  end the idle wait, so levelcap stops the run,
#                              puts the save back and exits
# Expects TAG set, and the extra ENV... args in "$@".
# The levelcap.sh started here does not outlive the script that sourced this:
# when that exits for any reason (the give-ups below, a kill), it ends that
# levelcap.sh, and only it, and waits for it, so the save is back first.
. "$(dirname "${BASH_SOURCE[0]}")/deslib.sh"
cd "$DES_ROOT" || exit 1
RUN=$DES_SCRATCH/lc_$TAG; LCLOG=$DES_SCRATCH/$TAG.lc
IDLE=$DES_SCRATCH/abboot_idle.txt; echo "sleep ${AB_IDLE:-1500}" > "$IDLE"
LC_NOPRESS=1 LC_SETTLE=1 LC_MAX=${LC_MAX:-900} LC_DUMP=0 \
  bash tools/des/levelcap.sh "$TAG" "$IDLE" PS3_PRIORITY=normal PS3_AFFINITY=all \
  PROF_DIR="$(cygpath -m "$RUN")" DES_AUTO_CONTINUE=1 "$@" > "$LCLOG" 2>&1 &
LC=$!
# Still our job (not yet waited for), so the pid cannot have been reused.
# levelcap.sh's EXIT trap stops the game and puts the save back; while it
# still waits for the save folder, it just exits.
ab_stop() {
  jobs -pr | grep -qx "$LC" || return 0
  kill "$LC" 2>/dev/null; wait "$LC" 2>/dev/null
}
trap ab_stop EXIT
t0=$SECONDS
until grep -aq 'level loading' "$LCLOG" 2>/dev/null; do
  sleep 1; kill -0 $LC 2>/dev/null || { echo "[ab] run ended before the level"; exit 1; }
  [ $((SECONDS - t0)) -lt 300 ] || { echo "[ab] no level load"; exit 1; }
done
if declare -F perf_skip_cutscene > /dev/null; then
  # The caller sourced perflib.sh (knobab.sh): press through a cutscene that
  # plays while the level settles, so no window lands in its 30 fps lock.
  t1=$SECONDS
  while [ $((SECONDS - t1)) -lt "${KAB_SETTLE:-40}" ]; do
    perf_skip_cutscene "$RUN" 60
    kill -0 $LC 2>/dev/null || { echo "[ab] run ended while settling"; exit 1; }
    sleep 1
  done
else
  sleep "${KAB_SETTLE:-40}"
fi
con() { des_console "$RUN/con" "$1" "${2:-15}"; }
ab_finish() {
  ps -ef | awk -v p=$LC '$3==p && /sleep/ {print $2}' | xargs -r kill
  wait $LC 2>/dev/null
}
