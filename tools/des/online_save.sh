#!/usr/bin/env bash
# online_save.sh [out-dir]: makes the online test save (docs/69): the pinned regression save plus the Stone of
# Ephemeral Eyes (x10), the five eye stones (white, blue, black, red, the kick item) and the Friend's and Foe's rings.
#
# Copies the source save into a scratch save root (PS3_SAVEDATA_ROOT), boots a developer build on it with
# DES_GIVE=online, waits for the items and for the save the give asks the game for, then copies the result to
# [out-dir] (default work/tmp/online/save; one already there is renamed to <out-dir>.bak-<time>). The source is only
# read; gamedata's save is never used.
#
#   DES_EXE=<developer build> tools/des/online_save.sh
#
# ONLINE_SAVE_SRC   the save to start from (default work/tmp/regress/save, the pinned regression save)
# ONLINE_SAVE_GIVE  the DES_GIVE list (default "online"; see port/des_dev.cpp)
# Needs ps3recomp's PS3_SAVEDATA_ROOT and a build with DES_DEV=ON. Exit status: 0 saved, 2 the run failed, 3 nothing
# was added, 4 the game wrote no save.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
SRC="${ONLINE_SAVE_SRC:-$DES_ROOT/work/tmp/regress/save}"
DST="${1:-$DES_ROOT/work/tmp/online/save}"
[ -d "$SRC" ] || { echo "online_save: no source save at $SRC"; exit 2; }
RUN="$DES_SCRATCH/online_save"; ROOT="$RUN/saves"; OUT="$RUN/run.txt"
rm -rf "$RUN"; mkdir -p "$ROOT"; : > "$OUT"
cp -a "$SRC/." "$ROOT/" || exit 2
sum() { find "$ROOT" -name '*USER.DAT' -exec md5sum {} + | sort | md5sum; }
before=$(sum)
trap des_kill_own EXIT
tools/des/desrun.sh "${ONLINE_SAVE_MAX:-420}" "$OUT" DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 PS3_SAIL_MOVIES=0 \
  PS3_NO_USER_INPUT=1 DES_ONLINE=0 PS3_SAVEDATA_ROOT="$(cygpath -m "$ROOT")" DES_GIVE="${ONLINE_SAVE_GIVE:-online}" \
  > "$RUN/summary.txt" 2>&1 &
t0=$SECONDS
say() { echo "[online_save] t=$((SECONDS - t0))s $*"; }
# DES_GIVE waits in the game until the character's stats are loaded and no cutscene runs, then adds the items and
# asks for a save (SaveRequest).
until grep -aq "DES_GIVE: .* save requested\|DES_GIVE .*refused" "$OUT"; do
  des_running || [ $((SECONDS - t0)) -lt 20 ] || { say "the run ended before DES_GIVE"; exit 2; }
  [ $((SECONDS - t0)) -lt 360 ] || { say "no DES_GIVE line"; exit 2; }
  sleep 1
done
sleep 1; grep -a "\[dev\] DES_GIVE" "$OUT"
grep -aq "DES_GIVE: .* save requested" "$OUT" || { say "nothing was added"; exit 3; }
tq=$SECONDS
until [ "$(sum)" != "$before" ]; do
  des_running || break
  [ $((SECONDS - tq)) -lt 60 ] || break
  sleep 1
done
sleep 3
[ "$(sum)" != "$before" ] || { say "the game wrote no save"; exit 4; }
des_kill_own
if [ -e "$DST" ]; then mv "$DST" "$DST.bak-$(date +%Y%m%d-%H%M%S)" || exit 2; fi   # an older one is kept
mkdir -p "$DST" && cp -a "$ROOT/." "$DST/" || exit 2
say "saved to $DST"
