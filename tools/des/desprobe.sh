#!/usr/bin/env bash
# desprobe.sh <outfile> <max_sec> [ENV=val ...] — one event-driven test run.
#
# Boots des_boot in the background and watches its log instead of sleeping on
# a schedule:
#   * presses a button (PRESS_MASK, default CROSS 0x4000) every PRESS_EVERY s
#     (default 2) from the first pad poll until PRESS_UNTIL appears in the log
#     (default: the trophy registration that follows the auto-save notice);
#     PRESS_UNTIL= (empty) disables pressing;
#   * when WHEN=<regex> appears, runs each line of CMDS=<file> on the debug
#     console ("sleep N" lines pause), then stops the run unless KEEP=1;
#   * returns as soon as the game exits -- pair with EXIT_ON=<tag>,
#     EXIT_ON_HLE=<nid> or EXIT_AFTER_MS=<n> so the game ends itself once the
#     answer is in the log. <max_sec> is only the backstop.
#   * SHOTS=<n> checks every n-th present and keeps it only when the image
#     changed (CELLMARK_DUMP_CHANGED; 300 files max)
#     in $DES_SCRATCH/shots.
# Prints the run's [exit-on] / [gcm-wedge] headline lines at the end.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1
OUT="${1:?need outfile}"; MAX="${2:?need max seconds}"; shift 2
case "$OUT" in /*|?:*) ;; *) OUT="$DES_SCRATCH/$OUT" ;; esac

PAD="$DES_SCRATCH/pad.$$"; : > "$PAD"
CON="$DES_SCRATCH/dbgcon.$$"; rm -f "$CON" "$CON.out"
extra=(PAD_FILE="$(cygpath -m "$PAD")" PS3_DEBUG="$(cygpath -m "$CON")")
if [ -n "${SHOTS:-}" ]; then
  S="$DES_SCRATCH/shots"; rm -rf "$S"; mkdir -p "$S"
  extra+=(CELLMARK_DUMP=300 CELLMARK_DUMP_EVERY="$SHOTS" CELLMARK_DUMP_CHANGED=1 CELLMARK_DUMP_MINDRAWS=1 CELLMARK_DUMP_DIR="$(cygpath -m "$S")")
fi
: > "$OUT"
tools/des/desrun.sh "$MAX" "$OUT" "${extra[@]}" "$@" > "$OUT.summary" 2>&1 &
RUN=$!

PRESS_UNTIL="${PRESS_UNTIL-RegisterContext\(ctx=}"
PRESS_EVERY="${PRESS_EVERY:-2}"; PRESS_MASK="${PRESS_MASK:-0x4000}"
pressing=0; [ -n "$PRESS_UNTIL" ] && pressing=1
fired=0; last_press=0; t0=$SECONDS
while kill -0 "$RUN" 2>/dev/null; do
  sleep 0.5
  if [ $pressing = 1 ]; then
    if grep -aqE "$PRESS_UNTIL" "$OUT"; then
      pressing=0; echo "[probe] t=$((SECONDS - t0))s '$PRESS_UNTIL' seen, stopped pressing"
    elif grep -aq 'GetData polling begins' "$OUT" && [ $((SECONDS - last_press)) -ge "$PRESS_EVERY" ]; then
      [ -s "$PAD" ] || echo "$PRESS_MASK" > "$PAD"; last_press=$SECONDS
    fi
  fi
  if [ $fired = 0 ] && [ -n "${WHEN:-}" ] && grep -aqE "$WHEN" "$OUT"; then
    fired=1; echo "[probe] t=$((SECONDS - t0))s '$WHEN' seen"
    if [ -n "${CMDS:-}" ]; then
      while IFS= read -r c || [ -n "$c" ]; do
        [ -z "$c" ] && continue
        case "$c" in sleep\ *) sleep "${c#sleep }"; continue ;; esac
        des_console "$CON" "$c"
      done < "$CMDS"
    fi
    [ "${KEEP:-0}" = 1 ] || des_kill_own
  fi
done
wait "$RUN" 2>/dev/null
echo "[probe] run ended after $((SECONDS - t0))s; log $OUT ($(wc -l < "$OUT") lines)"
grep -a '^\[exit-on\]\|^\[gcm-wedge\] label\|label255 moved again' "$OUT" | head -12
rm -f "$PAD" "$CON" "$CON.out"
