#!/usr/bin/env bash
# knobab.sh <tag> <KNOB=val> [ENV...]: compare a live knob within one run.
# Boots like abboot.sh, then takes KAB_WIN-second (default 6) profile windows
# in KAB_ORDER (default ABBAAB, so a drift over the run cancels), where A has
# KNOB cleared and B has `setenv KNOB=val`. Result: $DES_SCRATCH/<tag>.out.
TAG=$1; KV=$2; shift 2
. "$(dirname "$0")/abboot.sh"
W=${KAB_WIN:-6}; ORDER=${KAB_ORDER:-ABBAAB}; OUT=$DES_SCRATCH/$TAG.out
for k in $(echo "$ORDER" | sed 's/./& /g'); do
  if [ "$k" = A ]; then con "setenv ${KV%%=*}=" >/dev/null; else con "setenv $KV" >/dev/null; fi
  sleep 1; con profile >/dev/null; sleep "$W"; con profile >/dev/null; sleep 2
done
con "setenv ${KV%%=*}=" >/dev/null
: > "$OUT"; i=0
for f in $(ls "$RUN"/prof_*_summary.txt | sort); do
  echo "${ORDER:$i:1} $(head -1 "$f")" >> "$OUT"; i=$((i + 1))
done
bash tools/des/kabsum.sh "$OUT" >> "$OUT"
ab_finish
echo finished >> "$OUT"
