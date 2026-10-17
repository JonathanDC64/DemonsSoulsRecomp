#!/usr/bin/env bash
# exeab.sh <tag> <exeA> <exeB> [rounds [ENV...]]: compare two builds by
# alternating runs (A B A B ..., default 2 rounds). Each run boots LC_SAVE
# (levelcap.sh; default the pinned regression save) at DES_FPS=60 with the
# vblank cap lifted (VBLANK_MHZ=120000), settles 40 s, and takes two 8 s
# profile windows. Result: $DES_SCRATCH/<tag>.out, one line per window and
# the A/B means.
#
# One build alone varies by about a frame between windows while the PC is in
# use, so a change has to beat that in every window. RSX_PRESENT is chosen
# once for the batch from the lock state (0 while locked; presenting runs read
# ~2 fps lower than RSX_PRESENT=0). Extra ENV... args go to every run.
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
TAG=$1; A=$2; B=$3; R=${4:-2}; shift $(( $# < 4 ? $# : 4 ))
OUT=$DES_SCRATCH/$TAG.out; : > "$OUT"
CMDS=$DES_SCRATCH/exeab_cmds.txt
printf '%s\n' "sleep 40" profile "sleep 8" profile "sleep 2" profile "sleep 8" profile "sleep 2" > "$CMDS"
export RSX_PRESENT=${RSX_PRESENT:-$(des_session_locked && echo 0 || echo 1)}
echo "RSX_PRESENT=$RSX_PRESENT" >> "$OUT"
for r in $(seq 1 "$R"); do for k in A B; do
  [ $k = A ] && X=$A || X=$B
  N=${TAG}_${k}$r
  DES_EXE=$X LC_NOPRESS=1 LC_SETTLE=1 LC_MAX=600 LC_DUMP=0 \
    bash tools/des/levelcap.sh "$N" "$CMDS" PS3_PRIORITY=normal PS3_AFFINITY=all \
    PROF_DIR="$(cygpath -m "$DES_SCRATCH")/lc_$N" DES_AUTO_CONTINUE=1 DES_FPS=60 VBLANK_MHZ=120000 "$@" \
    > "$DES_SCRATCH/$N.lc" 2>&1
  for f in $(ls "$DES_SCRATCH/lc_$N"/prof_*_summary.txt 2>/dev/null | sort); do echo "$k $(head -1 "$f")" >> "$OUT"; done
done; done
bash tools/des/kabsum.sh "$OUT" >> "$OUT"
echo finished >> "$OUT"
