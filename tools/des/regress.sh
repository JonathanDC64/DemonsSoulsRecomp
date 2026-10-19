#!/usr/bin/env bash
# regress.sh [set-name]: regression screenshots of the first level.
#
# Boots with DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 into the pinned save, then walks a fixed
# route from the spawn point with the live PAD_STICK knob (debug console
# setenv) and saves one screenshot per stop:
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
# Timings: REGRESS_SETTLE (s after the level's script archive opens, default
# 75, so the arrival animation is over), REGRESS_WALK / REGRESS_TURN /
# REGRESS_ON (stick-hold seconds, defaults 8 / 1.5 / 5). The game is not
# frame-exact, so the diff allows some drift.
# REGRESS_AB="KNOB=value": an A/B at one spot. After s4_on, set the knob live
# and save s5_ab, then clear it and save s6_back. The walk does not end in
# the same place every run, so this, not two runs, is how to see what one
# knob changes. These two shots are not diffed against the good set.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
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

tools/des/desrun.sh "${REGRESS_MAX:-600}" "$OUT" DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 \
  PAD_FILE="$(cygpath -m "$PAD")" PS3_DEBUG="$(cygpath -m "$CON")" PS3_OSK_TEXT=Aldia HLE_FIRST=1 \
  CELLMARK_DUMP=100000 CELLMARK_DUMP_EVERY=15 CELLMARK_DUMP_CHANGED=1 CELLMARK_DUMP_MINDRAWS=1 \
  CELLMARK_DUMP_DIR="$(cygpath -m "$RUN/shots")" > "$RUN/summary.txt" 2>&1 &
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
sleep "${REGRESS_SETTLE:-75}"

# shot <name>: the newest frame dumped after this call (waits up to 20 s).
shot() {
  local before; before=$(ls "$RUN/shots" | sort | tail -1)
  local i
  for i in $(seq 1 40); do
    local now; now=$(ls "$RUN/shots" | sort | tail -1)
    if [ -n "$now" ] && [ "$now" != "$before" ]; then
      sleep 0.5; cp "$RUN/shots/$now" "$SET/$1.bmp"; say "$1 <- $now"; return 0
    fi
    sleep 0.5
  done
  say "$1: no new frame"; return 1
}
# hold <lx,ly,rx,ry> <seconds>: hold the sticks, then wait for the motion to stop.
hold() {
  des_console "$CON" "setenv PAD_STICK=$1,0,$2" > /dev/null
  sleep "$2"; sleep 2
}

shot s1_spawn
hold 128,0,128,128 "${REGRESS_WALK:-8}";    shot s2_walk
hold 128,128,255,128 "${REGRESS_TURN:-1.5}"; shot s3_turn
hold 128,0,128,128 "${REGRESS_ON:-5}";      shot s4_on
if [ -n "${REGRESS_AB:-}" ]; then
  des_console "$CON" "setenv $REGRESS_AB" > /dev/null; sleep 2; shot s5_ab
  des_console "$CON" "setenv ${REGRESS_AB%%=*}=" > /dev/null; sleep 2; shot s6_back
fi
des_console "$CON" "stat" | sed 's/^/  /'
restore; trap - EXIT
say "set $SET"

if [ -d "$REG/good" ]; then
  python "$DES_ROOT/tools/des/shotdiff.py" "$REG/good" "$SET" --out "$SET/diff"
else
  echo "no good set yet: look at $SET, then: regress.sh accept $NAME"
fi
