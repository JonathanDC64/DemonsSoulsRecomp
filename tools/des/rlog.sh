#!/usr/bin/env bash
# rlog.sh — READ-ONLY query helper for RPCS3 / des_boot logs.
# Safe by construction: only reads a log file (grep/sed/awk), never writes/deletes.
# Whitelist as: Bash(tools/des/rlog.sh *)
#
# Usage:
#   rlog.sh profile [LOG]                  size, channels, dialog-reached check
#   rlog.sh thread  <name> [LOG]           one thread's activity (trimmed)
#   rlog.sh sema    <hexid> [LOG]          semaphore lifecycle (create/wait/post/destroy)
#   rlog.sh threads [LOG]                  list all PPU threads created (in order)
#   rlog.sh window  <t1> <t2> <pat> [LOG]  lines matching <pat> in time window t1..t2
#   rlog.sh grep    <pattern> [LOG]        read-only passthrough grep (trimmed)
#   rlog.sh count   <pattern> [LOG]        count matches, grouped
#
# LOG defaults to $RLOG or the standard RPCS3 log path.
set -euo pipefail

[ -f "$(dirname "$0")/local.env" ] && . "$(dirname "$0")/local.env"
DEFAULT_LOG="${RPCS3_DIR:-.}/log/RPCS3.log"   # RPCS3_DIR: the RPCS3 install (local.env)
trim() { sed -E 's/\{PPU[^}]*\[([0-9a-fx:]+)\]\} /@\1 /; s/·[A-Z] //'; }

mode="${1:-profile}"; shift || true

case "$mode" in
  profile)
    LOG="${1:-${RLOG:-$DEFAULT_LOG}}"
    ls -la --time-style=+%m-%d_%H:%M "$LOG"
    echo "=== channels ==="
    grep -aoE '\} [A-Za-z_0-9]+:' "$LOG" | sed 's/}//' | sort | uniq -c | sort -rn | head -15
    echo "lines: $(wc -l < "$LOG")"
    echo "=== dialog reached? (cellSaveDataAutoLoad2 CALL, not import) ==="
    grep -aE 'cellSaveData(AutoLoad2|List)' "$LOG" | grep -avE 'import| -> ' | head -4 || echo "(none)"
    ;;
  thread)
    name="$1"; LOG="${2:-${RLOG:-$DEFAULT_LOG}}"
    grep -aE "Thread \($name\)" "$LOG" | grep -avE 'RSX: Add|not found|ROP reads' | \
      sed -E "s/^.{0,2}([0-9:.]+).*Thread \($name\)[^}]*\} /\1 /"
    ;;
  sema)
    id="$1"; LOG="${2:-${RLOG:-$DEFAULT_LOG}}"
    grep -aE "sem_id=$id|=\*$id|semaphore.*$id" "$LOG" | trim
    ;;
  threads)
    LOG="${1:-${RLOG:-$DEFAULT_LOG}}"
    grep -aoE 'Thread "[^"]+" created \(id=0x[0-9a-f]+' "$LOG" | \
      sed -E 's/.*Thread "([^"]+)" created \(id=(0x[0-9a-f]+)/\2 \1/'
    ;;
  window)
    t1="$1"; t2="$2"; pat="$3"; LOG="${4:-${RLOG:-$DEFAULT_LOG}}"
    awk -v a="$t1" -v b="$t2" '{ if (match($0,/[0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9]+/)) { ts=substr($0,RSTART,RLENGTH); if (ts>=a && ts<=b) print } }' "$LOG" | \
      grep -aE "$pat" | trim
    ;;
  grep)
    pat="$1"; LOG="${2:-${RLOG:-$DEFAULT_LOG}}"
    grep -aE "$pat" "$LOG" | trim
    ;;
  count)
    pat="$1"; LOG="${2:-${RLOG:-$DEFAULT_LOG}}"
    grep -aoE "$pat" "$LOG" | sort | uniq -c | sort -rn
    ;;
  *)
    echo "unknown mode: $mode (see header for usage)"; exit 2 ;;
esac
