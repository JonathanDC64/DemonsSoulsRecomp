#!/usr/bin/env bash
# regress.sh [set-name]: regression screenshots of the first level.
#
# Boots with DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 into the pinned save, lets the
# opening cutscene play out, then walks a fixed route from the spawn point with
# the live PAD_STICK knob (debug console setenv) and saves one screenshot per stop:
#   s1_spawn   at the spawn point, after the level settles
#   s2_walk    after walking forward
#   s3_turn    after turning the camera right
#   s4_on      after walking on
# then compares the set with the last accepted one (shotdiff.py) and prints
# the per-shot summary. Exit status: 0 all ok, 1 a shot regressed or is
# missing, 2 the run never reached the level, 3 the save folder could not be
# used: another run holds it (waited DES_SAVE_LOCK_WAIT s), or the player's
# save could not be copied aside, so nothing was swapped; 4 a run that has
# gone left the save folder swapped out, and this one did not undo it: the
# [save] lines say what was found and what to do (DES_SAVE_TAKEOVER,
# des_save_lock in deslib.sh).
#
#   regress.sh                 new set named by date+time, diffed against "good"
#   regress.sh accept <name>   make <name> the new "good" set
# Like a run, pin-save and accept wait for a run that holds the save folder.
#
# Sets live in REGRESS_DIR (default $DES_SCRATCH/regress). Screenshots are
# game-derived: point REGRESS_DIR outside the repository and never commit them.
# The run plays from a pinned save ($REG/save, `regress.sh pin-save [dir]`);
# the player's save is copied aside before the run and put back after it, so
# neither their progress nor an autosave moves the spawn point.
#
# The route runs on the game's clock, not the wall clock (regress_route.py).
# The run writes a frame log (DES_FRAMELOG, $RUN/frames.txt), and game time is
# the sum of the frame steps in it: 1/30 s a frame at the original 30 fps, the
# measured frame time under DES_FPS. So each stop is the same game moment at
# 30, 120 or 240 fps and on a slow run, where the holds and waits just take
# longer on the wall clock. A stick hold is a PAD_STICK window the runtime ends
# by itself, its length set from the game/real rate and corrected while it
# runs; a shot is the next frame after the console's CELLMARK_DUMP_NOW=1.
# Timings, in game seconds (30 frames each at the original rate):
# REGRESS_SETTLE (after the opening cutscene ends, default 35.9; with no
# cutscene, after the player appears), REGRESS_WALK / REGRESS_TURN /
# REGRESS_ON (stick holds, defaults 8 / 1.5 / 5). With the defaults s1_spawn
# and the first walk fall where the clock-driven version of this script put
# them at 30 fps; each later stop comes 0.3-0.6 s sooner after its hold, so
# s4_on is about 1.4 s sooner (see regress_route.py). Sets it accepted still
# compare. $RUN/route.txt has the timeline: the anchor, each hold's game-time
# start and length, each shot's game time.
# REGRESS_AB="KNOB=value": an A/B at one spot. After s4_on, set the knob live
# and save s5_ab, then clear it and save s6_back. The walk does not end in
# the same place every run, so this, not two runs, is how to see what one
# knob changes. These two shots are not diffed against the good set.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$DES_ROOT" || exit 1
REG="${REGRESS_DIR:-$DES_SCRATCH/regress}"; mkdir -p "$REG"

SAVE="gamedata/dev_hdd0/home/00000001/savedata"
# take_save: hold the save folder (des_save_lock) until this script exits.
take_save() {
  des_save_lock "$SAVE"; case $? in
    0) ;;
    1) echo "[regress] $SAVE is not free; not starting"; exit 3 ;;
    *) echo "[regress] not starting: see the [save] lines above"; exit 4 ;;
  esac
  trap 'des_save_unlock "$SAVE"' EXIT
}
# The route starts wherever the save puts the character, so the run plays
# from a pinned copy ($REG/save) and the player's own save is put back after.
# `regress.sh pin-save [dir]` pins the current save (or dir); re-accept a set
# after pinning a different one.
if [ "${1:-}" = pin-save ]; then
  src="${2:-$SAVE}"; [ -d "$src" ] || { echo "no save at $src"; exit 1; }
  take_save
  rm -rf "$REG/save"; cp -r "$src" "$REG/save"; echo "pinned $src"; exit 0
fi

if [ "${1:-}" = accept ]; then
  [ -d "$REG/${2:?set name}" ] || { echo "no set $REG/$2"; exit 1; }
  take_save
  rm -rf "$REG/good"; cp -r "$REG/$2" "$REG/good"; echo "good = $2"; exit 0
fi

NAME="${1:-$(date +%Y%m%d_%H%M%S)}"; SET="$REG/$NAME"
take_save
RUN="$DES_SCRATCH/regress_run"; rm -rf "$RUN" "$SET"; mkdir -p "$RUN/shots" "$SET"
OUT="$RUN/run.txt"; PAD="$RUN/pad"; CON="$RUN/con"; : > "$PAD"; : > "$OUT"

if [ -d "$SAVE" ]; then
  des_backup_save "$SAVE" "$RUN/save" || { echo "[regress] could not back up $SAVE; save not swapped"; exit 3; }
  des_save_note "$SAVE" "$RUN/save" || { echo "[regress] could not note the backup in $SAVE.lock; save not swapped"; exit 3; }
fi
restore() {   # as in levelcap.sh
  trap '' TERM INT HUP
  des_kill_own
  if [ ! -d "$RUN/save" ] || des_restore_save "$RUN/save" "$SAVE"; then des_save_unlock "$SAVE"; fi
  trap - TERM INT HUP
}
trap restore EXIT
if [ -d "$REG/save" ]; then
  des_save_swap "$REG/save" "$SAVE" || { echo "[regress] could not put $REG/save in place"; exit 2; }
else echo "[regress] no pinned save ($REG/save): using the current one; see pin-save"; fi

# No periodic frame dump: each shot asks for the next frame (CELLMARK_DUMP_NOW),
# and MINDRAWS keeps an empty present from being the one saved.
tools/des/desrun.sh "${REGRESS_MAX:-600}" "$OUT" DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 \
  PAD_FILE="$(cygpath -m "$PAD")" PS3_DEBUG="$(cygpath -m "$CON")" PS3_OSK_TEXT=Aldia HLE_FIRST=1 \
  DES_FRAMELOG="$(cygpath -m "$RUN/frames.txt")" \
  CELLMARK_DUMP_MINDRAWS=1 CELLMARK_DUMP_DIR="$(cygpath -m "$RUN/shots")" > "$RUN/summary.txt" 2>&1 &
t0=$SECONDS
say() { echo "[regress] t=$((SECONDS - t0))s $*"; }

# DES_AUTO_CONTINUE presses through the menus in-process until the level's
# script archive opens.
until grep -aq luabnd "$OUT"; do
  des_running || [ $((SECONDS - t0)) -lt 20 ] || { say "run ended before the level"; exit 2; }
  [ $((SECONDS - t0)) -lt 400 ] || { say "no level load"; exit 2; }
  sleep 0.5
done
say "level loading"

python "$(cygpath -m "$HERE/regress_route.py")" --frames "$(cygpath -m "$RUN/frames.txt")" \
  --console "$(cygpath -m "$CON")" --shots "$(cygpath -m "$RUN/shots")" --set "$(cygpath -m "$SET")" \
  --log "$(cygpath -m "$RUN/route.txt")" --settle "${REGRESS_SETTLE:-35.9}" --walk "${REGRESS_WALK:-8}" \
  --turn "${REGRESS_TURN:-1.5}" --on "${REGRESS_ON:-5}" --ab "${REGRESS_AB:-}" --t-offset $((SECONDS - t0))
rc=$?
[ $rc = 2 ] && exit 2
des_console "$CON" "stat" | sed 's/^/  /'
restore; trap - EXIT
say "set $SET"

if [ -d "$REG/good" ]; then
  python "$DES_ROOT/tools/des/shotdiff.py" "$REG/good" "$SET" --out "$SET/diff"
else
  echo "no good set yet: look at $SET, then: regress.sh accept $NAME"
fi
