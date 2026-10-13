#!/usr/bin/env bash
# desrun.sh — run des_boot.exe with a timeout, capture output, print a quick summary.
# Side-effecting only in that it runs our own recompiled binary + writes a log to the
# scratchpad. Whitelist as: Bash(tools/des/desrun.sh *)
#
# Usage: desrun.sh <timeout_sec> <outfile> [ENV=val ...]
#   e.g. desrun.sh 60 boot.txt YDKJ_SYNCTRACE=1 GCM_FIFO_DBG=1
# DES_EXE=<path> runs another build (default port/build/des_boot.exe).
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

TIMEOUT="${1:-60}"; OUT="${2:?need outfile}"; shift 2 || true
# Session-independent scratch: override with DES_SCRATCH, else repo-local work/tmp.
SCRATCH="${DES_SCRATCH:-$PWD/work/tmp}"; mkdir -p "$SCRATCH"
case "$OUT" in /*|?:*) ;; *) OUT="$SCRATCH/$OUT" ;; esac

export PS3_VFS_ROOT="D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)"
# Known-good boot config (docs/25): the LFQueue + SpursQueue families on real
# libsre, and no round-notify heuristic. Any ENV=val argument overrides these.
export LIBSRE_LLE=1
export LIBSRE_LLE_ONLY=011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667
export SPURS_NO_ROUND_NOTIFY=1
# Test runs must not starve the machine: below-normal priority on 8 of the
# 16 logical CPUs (override with PS3_PRIORITY= / PS3_AFFINITY=, e.g. FFFF).
export PS3_PRIORITY="${PS3_PRIORITY:-below}"
export PS3_AFFINITY="${PS3_AFFINITY:-FF}"
for kv in "$@"; do export "$kv"; done

# A leftover game from an earlier run would keep a core pinned.
taskkill //F //IM des_boot.exe > /dev/null 2>&1
timeout "$TIMEOUT" "${DES_EXE:-./port/build/des_boot.exe}" work/game/EBOOT.ELF > "$OUT" 2>&1
rc=$?
echo "exit=$rc lines=$(wc -l < "$OUT")  out=$OUT"
echo "=== threads created ==="
grep -aoE 'name="[^"]+"' "$OUT" | sort -u | tr '\n' ' '; echo
echo "=== last non-spam progress lines ==="
grep -avE '\[thrdiag\]|taskset syscall num=3|POLL-spin|no runnable' "$OUT" | tail -6
