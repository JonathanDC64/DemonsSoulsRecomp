#!/usr/bin/env bash
# levelcap.sh <name> <cmdfile> [ENV...]: boot to the level like regress.sh,
# settle, then run debug-console commands from <cmdfile>, one per line.
# "sleep N" and "stick <lx> <ly> <secs>" (hold the left stick there, 0-255
# with 128 centred, for that long, then wait 2 s) are handled here; every
# other line goes to the console. Output goes to
# $DES_SCRATCH/lc_<name>: run.txt (des_boot's log), summary.txt, con(.out).
#
#   LC_SAVE     save to play from; default the pinned regression save
#               (work/tmp/regress/save, see `regress.sh pin-save`). Set it
#               empty to play from the current save. The current save is
#               copied aside first and put back when the script exits, also
#               on a plain kill, Ctrl+C or a closed terminal. One run at a
#               time: a second levelcap.sh or regress.sh waits until the
#               first has put the save back (des_save_lock in deslib.sh).
#   LC_SETTLE   seconds to wait after the level starts loading (default 75)
#   LC_MAX      run cap in seconds (default 600)
#   LC_NOPRESS  set to skip pressing through the title and load screens
#               (DES_AUTO_CONTINUE=1 does that job)
#   LC_DUMP     CELLMARK screenshot count (default 100000; 0 for none)
#   LC_EVERY    frames between screenshots (default 30)
#   DES_EXE     exe to run (desrun.sh; default port/build/des_boot.exe)
#
# Exit status: 0 done; 2 the run stopped before the level, or could not start
# (no save at LC_SAVE); 3 the save folder could not be used: another run
# holds it (waited DES_SAVE_LOCK_WAIT s), or the player's save could not be
# copied aside, so nothing was swapped; 4 a run that has gone left the save
# folder swapped out, and this one did not undo it: the [save] lines say what
# was found and what to do (DES_SAVE_TAKEOVER, des_save_lock in deslib.sh).
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
NAME=$1; CMDS=$2; shift 2
SAVE="gamedata/dev_hdd0/home/00000001/savedata"
des_save_lock "$SAVE"; case $? in
  0) ;;
  1) echo "[lc] $SAVE is not free; not starting"; exit 3 ;;
  *) echo "[lc] not starting: see the [save] lines above"; exit 4 ;;
esac
trap 'des_save_unlock "$SAVE"' EXIT
RUN="$DES_SCRATCH/lc_$NAME"; rm -rf "$RUN"; mkdir -p "$RUN/shots"; SHOTS="$RUN/shots"
OUT="$RUN/run.txt"; PAD="$RUN/pad"; CON="$RUN/con"; : > "$PAD"; : > "$OUT"
LC_SAVE="${LC_SAVE-work/tmp/regress/save}"
if [ -d "$SAVE" ]; then
  des_backup_save "$SAVE" "$RUN/save" || { echo "[lc] could not back up $SAVE; save not swapped"; exit 3; }
  des_save_note "$SAVE" "$RUN/save" || { echo "[lc] could not note the backup in $SAVE.lock; save not swapped"; exit 3; }
fi
# bash runs the EXIT trap when a plain kill, Ctrl+C or a closed terminal ends
# it too. Further signals are ignored until the save is back: des_put_save
# empties the folder, then copies, and a second Ctrl+C or kill between the
# two left it empty.
# If the save cannot be put back, the lock stays and the next run puts it back.
restore() {
  trap '' TERM INT HUP
  des_kill_own
  if [ ! -d "$RUN/save" ] || des_restore_save "$RUN/save" "$SAVE"; then des_save_unlock "$SAVE"; fi
  trap - TERM INT HUP
}
trap restore EXIT
if [ -n "$LC_SAVE" ]; then
  [ -d "$LC_SAVE" ] || { echo "[lc] no save at $LC_SAVE"; exit 2; }
  des_save_swap "$LC_SAVE" "$SAVE" || { echo "[lc] could not put $LC_SAVE in place"; exit 2; }
fi
tools/des/desrun.sh "${LC_MAX:-600}" "$OUT" DES_SKIP_INTRO=1 \
  PAD_FILE="$(cygpath -m "$PAD")" PS3_DEBUG="$(cygpath -m "$CON")" PS3_OSK_TEXT=Aldia HLE_FIRST=1 CELLMARK_DUMP=${LC_DUMP:-100000} CELLMARK_DUMP_EVERY=${LC_EVERY:-30} CELLMARK_DUMP_CHANGED=1 CELLMARK_DUMP_MINDRAWS=1 CELLMARK_DUMP_DIR="$(cygpath -m "$SHOTS")" "$@" > "$RUN/summary.txt" 2>&1 &
t0=$SECONDS; say() { echo "[lc] t=$((SECONDS - t0))s $*"; }
k=0
until grep -aq luabnd "$OUT"; do
  des_running || [ $((SECONDS - t0)) -lt 20 ] || { say "run ended before the level"; exit 2; }
  [ $((SECONDS - t0)) -lt 400 ] || { say "no level load"; exit 2; }
  if [ -z "${LC_NOPRESS:-}" ] && grep -aq 'nid=0x8B72CDA1 cellPadGetData' "$OUT" && [ ! -s "$PAD" ]; then
    k=$((k + 1)); if [ $((k % 3)) = 0 ]; then echo 0x0008 > "$PAD"; else echo 0x4000 > "$PAD"; fi
  fi
  sleep 2
done
say "level loading"; sleep "${LC_SETTLE:-75}"
while IFS= read -r c; do
  c="${c%$'\r'}"; [ -z "$c" ] && continue
  case "$c" in
    sleep\ *) sleep "${c#sleep }";;
    stick\ *) set -- ${c#stick }; des_console "$CON" "setenv PAD_STICK=$1,$2,128,128,0,$3" >/dev/null; sleep "$3"; sleep 2;;
    *) say "$c"; des_console "$CON" "$c";;
  esac
done < "$CMDS"
say done
