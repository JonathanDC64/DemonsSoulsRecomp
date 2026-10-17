#!/usr/bin/env bash
# kprof.sh <tag> [ENV...]: profile threads in the level. Boots like abboot.sh,
# runs `cpu 4`, then `<PF_VERB> <tid> <PF_SECS>` (default `prof`, 8 s;
# `profrip` gives instruction addresses) for the threads whose `cpu` line
# matches PF_PAT (an ERE, e.g. spu_async_thread or HighGraphics), rows
# PF_FROM..PF_TO of the matches (default 1..3), then one profile window for
# the fps. Result: $DES_SCRATCH/<tag>.out; symbolize with profsym.py.
TAG=$1; shift
: "${PF_PAT:?set PF_PAT to the thread name(s) to profile}"
. "$(dirname "$0")/abboot.sh"
SECS=${PF_SECS:-8}
{
  con "cpu 4" | tee "$DES_SCRATCH/$TAG.cpu"
  PIDS=$(grep -aE "$PF_PAT" "$DES_SCRATCH/$TAG.cpu" | awk -v a="${PF_FROM:-1}" -v b="${PF_TO:-3}" 'NR>=a && NR<=b {print $1}')
  for t in $PIDS; do con "${PF_VERB:-prof} $t $SECS" $((SECS + 30)); done
  con profile >/dev/null; sleep "$SECS"; con profile >/dev/null; sleep 2
  f=$(ls "$RUN"/prof_*_summary.txt | sort | tail -1); head -2 "$f"
} > "$DES_SCRATCH/$TAG.out" 2>&1
ab_finish
echo finished >> "$DES_SCRATCH/$TAG.out"
