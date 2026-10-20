#!/usr/bin/env bash
# regfps.sh <tag> [fps ...]: regress.sh once per frame rate (default 30 240)
# on DES_EXE (default port/build/des_boot.exe): the regression walk from the
# pinned save, diffed against the accepted set, at the game's own 30 fps and
# above it. 30 is the game's mode (no DES_FPS); any other rate runs with
# DES_FPS=<fps> and RSX_VSYNC=0 (unless set), so 240 runs at 240 and not at
# the display's refresh.
#
# The pinned save and the accepted set come from REGRESS_SRC (default
# work/tmp/regress, where `regress.sh pin-save` and `accept` keep them) and are
# copied into $DES_SCRATCH/<tag>/regress, so the batch's sets never land in the
# user's regress directory. Each rate runs with its own DES_SCRATCH
# ($DES_SCRATCH/<tag>/<tag>_f<fps>), so no run's copy of the user's save is
# removed by the next. Like exeab.sh it refuses to start while any des_boot.exe
# runs, copies the user's save aside once and compares it after every run (a
# change stops the batch), and pins RSX_PRESENT from the lock state.
#
# Output: $DES_SCRATCH/<tag>/regfps.log (regress.sh's output per rate, prefixed
# [<tag>_f<fps>]), batch.txt, each rate's set and diff under regress/, and its
# run log under <tag>_f<fps>/regress_run/. Exit status: the worst regress.sh
# exit (0 all shots ok, 1 a shot regressed or is missing, 2 no level, 3/4 the
# save folder), 3 if a des_boot.exe was running, 5 if the user's save changed.
set -uo pipefail
. "$(dirname "$0")/perflib.sh"
cd "$DES_ROOT" || exit 1
TAG=${1:?tag}; shift
FPS=${*:-30 240}
SRC=${REGRESS_SRC:-work/tmp/regress}
D=$DES_SCRATCH/$TAG
EXE=${DES_EXE:-port/build/des_boot.exe}
[ -e "$D" ] && { echo "[regfps] $D exists; pick a new tag"; exit 2; }
[ -f "$EXE" ] || { echo "[regfps] no exe at $EXE"; exit 2; }
[ -d "$SRC/save" ] || { echo "[regfps] no pinned save at $SRC/save (regress.sh pin-save)"; exit 2; }
for f in $FPS; do
  case "$f" in ''|*[!0-9]*) echo "[regfps] a frame rate is a whole number: $f"; exit 2 ;; esac
done
[ "$(printf '%s\n' $FPS | sort | uniq -d)" = "" ] || { echo "[regfps] each frame rate once"; exit 2; }
perf_refuse_if_game
mkdir -p "$D/regress"
exec > >(tee -a "$D/regfps.log") 2>&1
cp -r "$SRC/save" "$D/regress/" || { echo "[regfps] could not copy the pinned save"; exit 2; }
if [ -d "$SRC/good" ]; then cp -r "$SRC/good" "$D/regress/" || { echo "[regfps] could not copy the accepted set"; exit 2; }
else echo "[regfps] no accepted set at $SRC/good: the shots are saved, not diffed"; fi
perf_save_snapshot "$D" || exit 3
perf_present
{
  perf_header
  echo "exe $EXE  md5 $(perf_md5 "$EXE")  fps $FPS  pinned save $SRC/save"
  echo "30 runs the game's mode; other rates run with DES_FPS=<fps> RSX_VSYNC=${RSX_VSYNC:-0}"
} > "$D/batch.txt"
cat "$D/batch.txt"
worst=0
for f in $FPS; do
  name=${TAG}_f$f
  perf_no_game 20 || { echo "[regfps] a des_boot.exe is running; stopping before $name"; [ "$worst" -lt 3 ] && worst=3; break; }
  if [ "$f" = 30 ]; then set --; how="the game's 30 fps mode"
  else set -- DES_FPS="$f" RSX_VSYNC="${RSX_VSYNC:-0}"; how="$*"; fi
  echo "[regfps] $(date +%T) $name: $how"
  env DES_SCRATCH="$D/$name" REGRESS_DIR="$D/regress" DES_EXE="$EXE" "$@" \
    bash tools/des/regress.sh "$name" 2>&1 | sed -u "s/^/[$name] /"
  rc=${PIPESTATUS[0]}
  echo "[regfps] $name: regress.sh exit $rc"; [ "$rc" -gt "$worst" ] && worst=$rc
  perf_save_check "$D" || { echo "[regfps] stopping: the user's save changed"; [ "$worst" -lt 5 ] && worst=5; break; }
  [ "$rc" = 3 ] || [ "$rc" = 4 ] && { echo "[regfps] stopping: the save folder (see the [save] lines)"; break; }
done
echo "[regfps] $TAG finished $(date '+%F %T'), worst exit $worst"
exit $worst
