#!/usr/bin/env bash
# desrun.sh — run des_boot.exe with a timeout, capture output, print a quick summary.
# Side-effecting only in that it runs our own recompiled binary + writes a log to the
# scratchpad. Whitelist as: Bash(tools/des/desrun.sh *)
#
# Usage: desrun.sh <timeout_sec> <outfile> [ENV=val ...]
#   e.g. desrun.sh 60 boot.txt YDKJ_SYNCTRACE=1 GCM_FIFO_DBG=1
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

TIMEOUT="${1:-60}"; OUT="${2:?need outfile}"; shift 2 || true
SCRATCH="/c/Users/jonat/AppData/Local/Temp/claude/c--dev-projects-demons-souls-pc-port/e916f056-9f0d-434a-81ed-1ea83209d4b0/scratchpad"
case "$OUT" in /*|?:*) ;; *) OUT="$SCRATCH/$OUT" ;; esac

export PS3_VFS_ROOT="D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)"
for kv in "$@"; do export "$kv"; done

timeout "$TIMEOUT" ./port/build/des_boot.exe work/game/EBOOT.ELF > "$OUT" 2>&1
rc=$?
echo "exit=$rc lines=$(wc -l < "$OUT")  out=$OUT"
echo "=== threads created ==="
grep -aoE 'name="[^"]+"' "$OUT" | sort -u | tr '\n' ' '; echo
echo "=== last non-spam progress lines ==="
grep -avE '\[thrdiag\]|taskset syscall num=3|POLL-spin|no runnable' "$OUT" | tail -6
