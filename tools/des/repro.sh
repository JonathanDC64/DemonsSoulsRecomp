#!/usr/bin/env bash
# repro.sh -- run the boot N times + report the distribution of observables
# (docs/18 domain D). The boot is nondeterministic; a single run can't judge a
# fix. This samples N boots into run_1..run_N.txt and calls repro_report.py.
#
# Usage:  repro.sh N [timeout_sec] [VAR=val ...]
#   e.g.  repro.sh 6 20 SPURS_LV2THREADS=1
# Compare a fix:  save two dirs then
#   python tools/des/repro_report.py --baseline base/ --candidate cand/
#
# Whitelist: Bash(tools/des/repro.sh *)
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
N="${1:-5}"; shift || true
TIMEOUT="${1:-20}"; shift || true
# Session-independent scratch: override with DES_SCRATCH, else repo-local work/tmp.
SCRATCH="${DES_SCRATCH:-$PWD/work/tmp}"; mkdir -p "$SCRATCH"
OUTDIR="$SCRATCH/repro_$$"
mkdir -p "$OUTDIR"
[ -f tools/des/local.env ] && . tools/des/local.env
export PS3_VFS_ROOT="${PS3_VFS_ROOT:-$PWD/work/disc}"
export DES_ONLINE="${DES_ONLINE:-0}"   # offline, as deslib.sh's test runs (this one reads the user's settings.ini)
for kv in "$@"; do export "$kv"; done
echo "[repro] $N runs x ${TIMEOUT}s, env: $* -> $OUTDIR"
. "$(dirname "$0")/deslib.sh"
EXE="$(des_stage_exe)" || exit 1
for i in $(seq 1 "$N"); do
  timeout "$TIMEOUT" "$EXE" work/game/EBOOT.ELF > "$OUTDIR/run_$i.txt" 2>&1
  printf '.'
done
echo
python tools/des/repro_report.py "$OUTDIR"/run_*.txt
echo "[repro] logs in $OUTDIR"
