#!/usr/bin/env bash
# exeab.sh <tag> <exeA> <exeB> [rounds [ENV...]]: compare two builds by
# alternating boots, A B then B A and so on (default 6 rounds: 6 boots a side;
# a drift over the batch cancels). Each boot plays from PERF_SAVE (levelcap.sh's
# LC_SAVE; default the pinned regression save, work/tmp/regress/save) uncapped:
# DES_FPS=60, VBLANK_MHZ=PERF_VBLANK (default 960000), RSX_VSYNC=0, normal
# priority on all CPUs, DES_AUTO_CONTINUE=1 through the title and nothing
# pressed after it (a cutscene, if one plays, is pressed through). PERF_SETTLE
# s after the level starts loading (default 40) it takes two 8 s windows, each
# followed by 10 s with nothing armed.
#
# The windows are plain by default (EXEAB_PLAIN=1), as knobab.sh's: the
# console's `stat` gives the flip count at either end, nothing is armed, and
# kabplain.py turns each flip range into presents over frames.log time
# (lc_<boot>/plain_NNN.txt). EXEAB_PLAIN=0 takes profile windows instead, and
# abstat.py then gives two frame rates per window: the profile session's own
# (it times every texture, SRV and pipeline lookup on the FIFO thread and every
# guest syscall, so it reads low and the FIFO thread holds the frame, docs/83)
# and the gap after it (unperturbed); only that mode writes abthreads.txt.
# Extra ENV... go to every boot (both sides); EXEAB_ENV_A and EXEAB_ENV_B
# (VAR=value words) go to one side only, so the same exe twice with
# EXEAB_ENV_B="SOME_KNOB=1" is a knob A/B across boots (knobab.sh compares
# knobs inside a boot).
#
# The vblank has to stay well above the frame rate measured. At 120 Hz with
# builds at ~117 fps, frames under 8.3 ms waited for the vblank, and a change
# worth +1.9% at 240 Hz read +0.9%. 240 Hz is enough at the Palace (~120
# fps), but the pinned save's Tutorial spot runs past 300 fps, hence 960 Hz;
# a boot with a window within 10% of the vblank rate is marked invalid.
# Presents must not wait for the display
# either (RSX_VSYNC=0): with vsync a presenting run is held near the display's
# refresh. Where the system cannot tear, the backend presents with vsync and
# says so, and a presenting boot is then marked invalid.
#
# Checks (perflib.sh): no des_boot.exe may run before the batch, nor before a
# boot (the user's game: the batch stops); the PC must be quiet before each
# boot; the user's save is copied aside once and compared after every boot (a
# change stops the batch); each boot must load PERF_MAP (default m08, where the
# pinned save loads; not checked when PERF_SAVE is set without PERF_MAP), end
# its windows outside a cutscene and not fall back to vsync. RSX_PRESENT is
# pinned once for the batch from the lock state (0 while locked); if the
# session locks under a presenting batch, the batch stops.
#
# Output in $DES_SCRATCH/<tag>/: batch.txt (header), runs.txt (side, boot,
# round, validity, background CPU %), <tag>.out ("<side> <boot> <window
# line>" per window, kabsum.sh's means at the end), abstat.txt (the verdict,
# abstat.py), abthreads.txt (EXEAB_PLAIN=0: per-thread shares, abthreads.py),
# lc_<boot>/ (run.txt, plainwin.txt and plain files or profile files,
# frames.log), <boot>.lc, exeab.log.
# Exit status: 0 the batch ran through, 2 bad arguments, 3 a des_boot.exe was
# running or the save folder was not free, 4 the PC stayed busy or the session
# locked under a presenting batch, 5 the user's save changed.
set -uo pipefail
. "$(dirname "$0")/perflib.sh"
export DES_WINDOW=${DES_WINDOW:-normal}   # (the window is saved with PrintWindow; deslib.sh des_window_launch)
cd "$DES_ROOT" || exit 1
TAG=${1:?tag}; A=${2:?exeA}; B=${3:?exeB}; R=${4:-6}; shift $(( $# < 4 ? $# : 4 ))
D=$DES_SCRATCH/$TAG
[ -e "$D" ] && { echo "[ab] $D exists; pick a new tag"; exit 2; }
for x in "$A" "$B"; do [ -f "$x" ] || { echo "[ab] no exe at $x"; exit 2; }; done
SAVE=${PERF_SAVE:-work/tmp/regress/save}
[ -d "$SAVE" ] || { echo "[ab] no save at $SAVE (pin one: regress.sh pin-save)"; exit 2; }
if [ -n "${PERF_SAVE:-}" ]; then MAP=${PERF_MAP:-}; else MAP=${PERF_MAP:-m08}; fi
VB=${PERF_VBLANK:-960000}; SETTLE=${PERF_SETTLE:-40}
PLAIN=${EXEAB_PLAIN:-1}   # the window files: plain_NNN.txt, or prof_NNN_summary.txt
if [ "$PLAIN" = 1 ]; then WIN=stat; WP="plain_[0-9]*.txt"; else WIN=profile; WP="prof_[0-9]*_summary.txt"; fi
perf_refuse_if_game
mkdir -p "$D"; export PERF_LOG=$D/quiet.txt
exec > >(tee -a "$D/exeab.log") 2>&1
echo "[ab] $TAG: $(date '+%F %T'), $R rounds"
perf_wait_quiet 5 || exit 4
perf_save_snapshot "$D" || exit 3
perf_present; perf_uncapped
export DES_SCRATCH=$D
{
  perf_header
  echo "rounds $R  save $SAVE  map ${MAP:-(not checked)}  settle ${SETTLE}s"
  echo "A $A  md5 $(perf_md5 "$A")"
  echo "B $B  md5 $(perf_md5 "$B")"
  echo "env DES_FPS=60 VBLANK_MHZ=$VB RSX_VSYNC=$RSX_VSYNC PS3_PRIORITY=normal PS3_AFFINITY=all DES_AUTO_CONTINUE=1 $*"
  echo "A only: ${EXEAB_ENV_A:-}  B only: ${EXEAB_ENV_B:-}"
  echo "windows: $([ "$PLAIN" = 1 ] && echo plain || echo profile)"
} > "$D/batch.txt"
cat "$D/batch.txt"
printf '%s\n' "sleep $SETTLE" $WIN "sleep 8" $WIN "sleep 10" $WIN "sleep 8" $WIN "sleep 10" > "$D/cmds.txt"
OUT=$D/$TAG.out; : > "$D/runs.txt"; : > "$OUT"
# A levelcap.sh started here does not outlive this script: ending it runs its
# EXIT trap, which stops its game and puts the save back.
lc=; ec=0
stop_lc() { [ -n "$lc" ] && kill -0 "$lc" 2>/dev/null && { kill "$lc" 2>/dev/null; wait "$lc" 2>/dev/null; }; }
trap stop_lc EXIT
for r in $(seq 1 "$R"); do
  if [ $((r % 2)) = 1 ]; then order="A B"; else order="B A"; fi
  for k in $order; do
    [ "$k" = A ] && X=$A || X=$B
    [ "$k" = A ] && XE=${EXEAB_ENV_A:-} || XE=${EXEAB_ENV_B:-}
    N=${TAG}_${k}$r; RUN=$D/lc_$N
    perf_no_game 20 || { echo "[ab] a des_boot.exe is running; stopping before $N"; ec=3; break 2; }
    perf_present_ok || { echo "[ab] the session locked under a presenting batch; stopping before $N"; ec=4; break 2; }
    perf_wait_quiet 3 || { echo "[ab] stopping before $N"; ec=4; break 2; }
    echo "[ab] $(date +%T) $N (background cpu ${PERF_BG:-?}%)"
    DES_EXE=$X LC_SAVE=$SAVE LC_NOPRESS=1 LC_SETTLE=1 LC_MAX=$((600 + SETTLE)) LC_DUMP=0 \
      bash tools/des/levelcap.sh "$N" "$D/cmds.txt" PS3_PRIORITY=normal PS3_AFFINITY=all \
      PROF_DIR="$(cygpath -m "$RUN")" FRAME_LOG="$(cygpath -m "$RUN")/frames.log" \
      DES_AUTO_CONTINUE=1 DES_FPS=60 VBLANK_MHZ="$VB" RSX_VSYNC="$RSX_VSYNC" "$@" $XE > "$D/$N.lc" 2>&1 &
    lc=$!
    while kill -0 "$lc" 2>/dev/null; do
      [ -f "$RUN/run.txt" ] && perf_skip_cutscene "$RUN" 90 >> "$D/$N.skip" 2>&1
      sleep 1
    done
    wait "$lc"; rc=$?; lc=
    if [ "$rc" = 3 ] || [ "$rc" = 4 ]; then
      echo "[ab] $N: levelcap.sh could not use the save folder (exit $rc, see $D/$N.lc); stopping"; ec=3; break 2
    fi
    if [ "$PLAIN" = 1 ] && [ -d "$RUN" ]; then
      # levelcap.sh printed each `stat`; windows 1 and 2 are flips 1-2 and 3-4.
      awk '$1 == "flips" {f[++n] = $2} END {for (i = 1; i + 1 <= n; i += 2) print (i + 1) / 2, f[i], f[i + 1]}' \
        "$D/$N.lc" > "$RUN/plainwin.txt"
      python "$(cygpath -m "$DES_ROOT/tools/des/kabplain.py")" "$(cygpath -m "$RUN")" > "$RUN/kabplain.txt" 2>&1
    fi
    v=$(perf_run_valid "$RUN" "$MAP" "$VB")
    got=$(ls "$RUN"/$WP 2>/dev/null | wc -l)
    [ "$v" = ok ] && [ "$got" -ne 2 ] && v="windows $got of 2"
    printf '%s\t%s\t%s\t%s\t%s\n' "$k" "$N" "$r" "$v" "${PERF_BG:-?}" >> "$D/runs.txt"
    for f in "$RUN"/$WP; do [ -f "$f" ] && echo "$k $N $(head -1 "$f")" >> "$OUT"; done
    echo "[ab] $N: $v; $(grep -h "^$k $N " "$OUT" | grep -o '[0-9.]* fps average' | tr '\n' ' ')"
    perf_save_check "$D" || { echo "[ab] stopping: the user's save changed"; ec=5; break 2; }
  done
done
python "$(cygpath -m "$DES_ROOT/tools/des/abstat.py")" "$(cygpath -m "$D")" > "$D/abstat.txt" 2>&1
[ "$PLAIN" = 1 ] || python "$(cygpath -m "$DES_ROOT/tools/des/abthreads.py")" "$(cygpath -m "$D")" > "$D/abthreads.txt" 2>&1
bash tools/des/kabsum.sh "$OUT" >> "$OUT"
cat "$D/abstat.txt"
echo "[ab] $TAG finished $(date '+%F %T'); results in $D"
exit $ec
