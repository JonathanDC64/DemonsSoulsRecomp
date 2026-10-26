#!/usr/bin/env bash
# knobab.sh <tag> <KNOB=val[,KNOB2=val2...] | knobs-file> [ENV...]: compare
# live knobs inside boots, so the boot-to-boot spread drops out. Each boot
# (KAB_BOOTS, default 1) boots like abboot.sh from PERF_SAVE (default the
# pinned regression save) on DES_EXE (default port/build/des_boot.exe),
# uncapped like exeab.sh (DES_FPS=60, VBLANK_MHZ=PERF_VBLANK default 960000,
# RSX_VSYNC=0), waits for the level and KAB_SETTLE s (default 40; a cutscene
# is pressed through), then for each knob line takes KAB_WIN-second
# windows (default 6) in KAB_ORDER (default ABBAAB, pairs of AB or BA, so a
# drift inside the boot cancels): A with the line's knobs cleared
# (`setenv KNOB=`), B with them set.
# The windows are plain by default (KAB_PLAIN=1): two console `stat` calls
# give the flip count at either end, the boot runs with FRAME_LOG, and after
# the boot kabplain.py turns each flip range into presents over frames.log time
# (lc_<boot>/plain_NNN.txt). Nothing is armed in the window. KAB_PLAIN=0 takes
# profile windows instead (prof_NNN_*.txt, with the per-thread tables), but the
# profile session times every texture, SRV and pipeline lookup on the FIFO
# thread, so there the FIFO thread holds the frame and a FIFO-thread change
# reads bigger than in plain play (docs/83 2.1: +6.5% there, +2.3% plain).
# A knobs file has one comparison per line ("KNOB=value", or several as
# "KNOB=value,OTHER=value"; blank lines and # comments skipped). Extra ENV...
# go to every boot.
#
# Window check: after each window, in the 2 s gap before the next (outside the
# timed window), the game window is saved once with PrintWindow (wincap.ps1)
# into shots/. If the level is black (mean brightness under PERF_BLACK,
# default 2 of 255), the boot stops and so does the batch: a live knob switch
# once left a stale vertex shader, and the black frames read as a gain. If the
# first capture fails, the batch stops too (rerun with PERF_WINCHECK=0 to go
# without the check). Off while RSX_PRESENT=0 (nothing is presented) or with
# PERF_WINCHECK=0.
#
# Same quiet, save and validity checks as exeab.sh. Output in
# $DES_SCRATCH/<tag>/: batch.txt, knobs.txt, manifest.txt (window number, side,
# knob line; the same in every boot), runs.txt, wincheck.txt, shots/,
# <tag>.out ("<side> <boot> <window line>" per window, kabsum.sh's
# means at the end), knobstat.txt (the verdict per knob line, knobstat.py),
# lc_<boot>/, <boot>.log, knobab.log. Exit status as exeab.sh's (0 ran
# through, 2 bad arguments, 3 a des_boot.exe was running or the save folder
# was not free, 4 the PC stayed busy or the session locked, 5 the user's save
# changed), plus 6 the window check stopped it.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -n "${KAB_CHILD:-}" ]; then
  # One boot of the batch below. KAB_CHILD is the batch directory; the
  # arguments are <boot> <knobs.txt> [ENV...].
  TAG=$1; KF=$2; shift 2; BD=$KAB_CHILD
  mapfile -t KNOBS < "$KF"
  . "$HERE/perflib.sh"
  . "$HERE/abboot.sh"
  W=${KAB_WIN:-6}; ORDER=${KAB_ORDER:-ABBAAB}; pid=$(perf_game_pid); i=0; stopc=0
  check=${PERF_WINCHECK:-1}; [ "${RSX_PRESENT:-1}" = 0 ] && check=0; plain=${KAB_PLAIN:-1}
  for kv in "${KNOBS[@]}"; do
    for s in $(echo "$ORDER" | sed 's/./& /g'); do
      kill -0 "$LC" 2>/dev/null || break 2   # the run ended (its cap, a crash): the parent counts the windows
      for one in ${kv//,/ }; do
        if [ "$s" = A ]; then con "setenv ${one%%=*}=" > /dev/null; else con "setenv $one" > /dev/null; fi
      done
      if [ "$plain" = 1 ]; then
        sleep 1; s1=$(con stat | awk '$1 == "flips" {print $2}'); sleep "$W"
        echo "$((i + 1)) $s1 $(con stat | awk '$1 == "flips" {print $2}')" >> "$RUN/plainwin.txt"
      else
        sleep 1; con profile > /dev/null; sleep "$W"; con profile > /dev/null
      fi
      i=$((i + 1)); sleep 2 & gap=$!
      if [ "$check" = 1 ]; then
        shot=$(perf_wincap "$pid" "$BD/shots/${TAG}_w$(printf %03d "$i").png")
        echo "$TAG window $i $s $kv: ${shot:-no capture}" >> "$BD/wincheck.txt"
        if perf_black "$shot"; then
          echo "black after window $i ($s, $kv)" > "$RUN/stopped"; stopc=5; wait "$gap"; break 2
        fi
        # A check that cannot capture would let a black level through unseen.
        if [ "$i" = 1 ] && [ "${shot%% *}" != mean ]; then
          echo "no capture after window 1 (${shot:-no output}); PERF_WINCHECK=0 runs without the check" > "$RUN/stopped"
          stopc=6; wait "$gap"; break 2
        fi
      fi
      wait "$gap"   # the next window starts 2 s after this one ended, or once the capture is done
    done
    for one in ${kv//,/ }; do con "setenv ${one%%=*}=" > /dev/null; done
  done
  ab_finish
  [ "$plain" = 1 ] && python "$(cygpath -m "$HERE/kabplain.py")" "$(cygpath -m "$RUN")" > "$RUN/kabplain.txt" 2>&1
  exit $stopc
fi

. "$HERE/perflib.sh"
cd "$DES_ROOT" || exit 1
TAG=${1:?tag}; KV=${2:?KNOB=value or a knobs file}; shift 2
D=$DES_SCRATCH/$TAG
[ -e "$D" ] && { echo "[ab] $D exists; pick a new tag"; exit 2; }
NB=${KAB_BOOTS:-1}; W=${KAB_WIN:-6}; ORDER=${KAB_ORDER:-ABBAAB}; SETTLE=${KAB_SETTLE:-40}
PLAIN=${KAB_PLAIN:-1}   # the window files: plain_NNN.txt, or prof_NNN_summary.txt
if [ "$PLAIN" = 1 ]; then WF=plain; WP="plain_[0-9]*.txt"; else WF=prof; WP="prof_[0-9]*_summary.txt"; fi
# knobstat.py pairs windows 1-2, 3-4, ...: each pair has to be AB or BA.
echo "$ORDER" | grep -Eqx '((AB)|(BA))+' || { echo "[ab] KAB_ORDER is pairs of AB or BA (like ABBAAB)"; exit 2; }
X=${DES_EXE:-port/build/des_boot.exe}
[ -f "$X" ] || { echo "[ab] no exe at $X"; exit 2; }
SAVE=${PERF_SAVE:-work/tmp/regress/save}
[ -d "$SAVE" ] || { echo "[ab] no save at $SAVE (pin one: regress.sh pin-save)"; exit 2; }
if [ -n "${PERF_SAVE:-}" ]; then MAP=${PERF_MAP:-}; else MAP=${PERF_MAP:-m08}; fi
VB=${PERF_VBLANK:-960000}
if [ -f "$KV" ]; then KNOBS=$(tr -d '\r' < "$KV" | grep -v '^[[:space:]]*#' | awk 'NF'); else KNOBS=$KV; fi
[ -n "$KNOBS" ] || { echo "[ab] no knob lines in $KV"; exit 2; }
if printf '%s\n' "$KNOBS" | grep -Evq '^[A-Za-z_][A-Za-z0-9_]*=[^, ]*(,[A-Za-z_][A-Za-z0-9_]*=[^, ]*)*$'; then
  echo "[ab] a knob line reads KNOB=value or KNOB=value,OTHER=value (no spaces)"; exit 2
fi
perf_refuse_if_game
mkdir -p "$D/shots"; export PERF_LOG=$D/quiet.txt
exec > >(tee -a "$D/knobab.log") 2>&1
printf '%s\n' "$KNOBS" > "$D/knobs.txt"
: > "$D/manifest.txt"; nw=0
while IFS= read -r kv; do
  for s in $(echo "$ORDER" | sed 's/./& /g'); do nw=$((nw + 1)); echo "$nw $s $kv" >> "$D/manifest.txt"; done
done < "$D/knobs.txt"
# The game's run cap (levelcap.sh's LC_MAX, abboot.sh's default 900 s) and
# abboot.sh's idle after the level (AB_IDLE) have to cover the boot and every
# window.
LCM=$(( 400 + SETTLE + nw * (W + 8) )); [ "$LCM" -lt 900 ] && LCM=900
export LC_MAX=$LCM AB_IDLE=$LCM
echo "[ab] $TAG: $(date '+%F %T'), $NB boot(s), $nw windows of ${W}s a boot, order $ORDER"
perf_wait_quiet 5 || exit 4
perf_save_snapshot "$D" || exit 3
perf_present; perf_uncapped
{
  perf_header wincheck
  echo "boots $NB  order $ORDER  window ${W}s ($WF)  settle ${SETTLE}s  save $SAVE  map ${MAP:-(not checked)}"
  echo "exe $X  md5 $(perf_md5 "$X")"
  echo "env DES_FPS=60 VBLANK_MHZ=$VB RSX_VSYNC=$RSX_VSYNC PS3_PRIORITY=normal PS3_AFFINITY=all DES_AUTO_CONTINUE=1 $*"
  sed 's/^/knob /' "$D/knobs.txt"
} > "$D/batch.txt"
cat "$D/batch.txt"
OUT=$D/$TAG.out; : > "$D/runs.txt"; : > "$OUT"; : > "$D/wincheck.txt"
# A boot started here does not outlive this script: ending it runs the
# boot's EXIT trap (abboot.sh), which ends its levelcap.sh, which stops the
# game and puts the save back.
kpid=; ec=0
stop_kid() { [ -n "$kpid" ] && kill -0 "$kpid" 2>/dev/null && { kill "$kpid" 2>/dev/null; wait "$kpid" 2>/dev/null; }; }
trap stop_kid EXIT
for b in $(seq 1 "$NB"); do
  N=${TAG}_k$b; RUN=$D/lc_$N
  FL=(); [ "$PLAIN" = 1 ] && FL=("FRAME_LOG=$(cygpath -m "$RUN")/frames.log")
  perf_no_game 20 || { echo "[ab] a des_boot.exe is running; stopping before $N"; ec=3; break; }
  perf_present_ok || { echo "[ab] the session locked under a presenting batch; stopping before $N"; ec=4; break; }
  perf_wait_quiet 3 || { echo "[ab] stopping before $N"; ec=4; break; }
  echo "[ab] $(date +%T) $N (background cpu ${PERF_BG:-?}%)"
  KAB_CHILD=$D DES_SCRATCH=$D DES_EXE=$X LC_SAVE=$SAVE KAB_SETTLE=$SETTLE \
    bash "$HERE/knobab.sh" "$N" "$D/knobs.txt" PS3_PRIORITY=normal PS3_AFFINITY=all \
    ${FL[@]+"${FL[@]}"} \
    DES_FPS=60 VBLANK_MHZ="$VB" RSX_VSYNC="$RSX_VSYNC" "$@" > "$D/$N.log" 2>&1 &
  kpid=$!
  wait "$kpid"; rc=$?; kpid=
  v=$(perf_run_valid "$RUN" "$MAP" "$VB")
  [ "$rc" = 5 ] && v="level went black: $(cat "$RUN/stopped" 2>/dev/null)"
  [ "$rc" = 6 ] && v="window check failed: $(cat "$RUN/stopped" 2>/dev/null)"
  got=$(ls "$RUN"/$WP 2>/dev/null | wc -l)
  [ "$v" = ok ] && [ "$got" -ne "$nw" ] && v="windows $got of $nw"
  printf '%s\t%s\t%s\t%s\t%s\n' K "$N" "$b" "$v" "${PERF_BG:-?}" >> "$D/runs.txt"
  i=0
  for f in "$RUN"/$WP; do
    [ -f "$f" ] || continue
    i=$((i + 1)); echo "$(awk -v i=$i '$1 == i {print $2}' "$D/manifest.txt") $N $(head -1 "$f")" >> "$OUT"
  done
  echo "[ab] $N: $v"
  perf_save_check "$D" || { echo "[ab] stopping: the user's save changed"; ec=5; break; }
  if [ "$rc" = 5 ] || [ "$rc" = 6 ]; then echo "[ab] stopping: $v (wincheck.txt, shots/)"; ec=6; break; fi
done
python "$(cygpath -m "$DES_ROOT/tools/des/knobstat.py")" "$(cygpath -m "$D")" > "$D/knobstat.txt" 2>&1
bash tools/des/kabsum.sh "$OUT" >> "$OUT"
cat "$D/knobstat.txt"
echo "[ab] $TAG finished $(date '+%F %T'); results in $D"
exit $ec
