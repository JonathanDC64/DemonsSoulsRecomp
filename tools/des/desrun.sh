#!/usr/bin/env bash
# desrun.sh — run des_boot.exe with a timeout, capture output, print a quick summary.
# Side-effecting only in that it runs our own recompiled binary + writes a log to the
# scratchpad. Whitelist as: Bash(tools/des/desrun.sh *)
#
# Usage: desrun.sh <timeout_sec> <outfile> [ENV=val ...]
#   e.g. desrun.sh 60 boot.txt YDKJ_SYNCTRACE=1 GCM_FIFO_DBG=1
# DES_EXE=<path> runs another build (default port/build/des_boot.exe). The run
# launches a copy in $DES_SCRATCH/bin (des_stage_exe), so rebuilding never
# fails on a locked exe while it runs.
# COLD_CACHE=1 wipes the game's HDD cache first (the default keeps it).
# Defaults (VFS root, quiet logs, priority, affinity) are in deslib.sh.
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1

TIMEOUT="${1:-60}"; OUT="${2:?need outfile}"; shift 2 || true
case "$OUT" in /*|?:*) ;; *) OUT="$DES_SCRATCH/$OUT" ;; esac

des_env_defaults
for kv in "$@"; do export "$kv"; done

# A leftover game from an earlier run of ours would keep a core busy.
des_kill_own
if [ "${COLD_CACHE:-0}" = 1 ]; then rm -rf "$(des_cache_dir)"; fi
EXE="$(des_stage_exe)" || { echo "desrun: could not stage the exe"; exit 1; }
START=$SECONDS
timeout "$TIMEOUT" "$EXE" work/game/EBOOT.ELF > "$OUT" 2>&1
rc=$?
des_wait_own $((TIMEOUT - (SECONDS - START)))   # the game that setup's Play started, if any
des_kill_own
echo "exit=$rc lines=$(wc -l < "$OUT")  out=$OUT"
echo "=== threads created ==="
grep -aoE 'name="[^"]+"' "$OUT" | sort -u | tr '\n' ' '; echo
echo "=== last non-spam progress lines ==="
grep -avE '\[thrdiag\]|taskset syscall num=3|POLL-spin|no runnable' "$OUT" | tail -6
