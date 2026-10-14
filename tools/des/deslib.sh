#!/usr/bin/env bash
# deslib.sh -- shared helpers for the des_boot test scripts. Source it.
#
# Machine-specific values come from the environment; the fallbacks below are
# only defaults for the original dev box, never requirements:
#   PS3_VFS_ROOT   disc root holding PS3_GAME (and the /dev_hdd1 cache dir)
#   DES_SCRATCH    where logs and the pid/console files go (default work/tmp)

DES_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DES_SCRATCH="${DES_SCRATCH:-$DES_ROOT/work/tmp}"; mkdir -p "$DES_SCRATCH"
DES_PIDFILE="$(cygpath -m "$DES_SCRATCH" 2>/dev/null || echo "$DES_SCRATCH")/des_boot.pids"

des_env_defaults() {
  des_vfs_fallback="D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)"   # dev-box default only
  export PS3_VFS_ROOT="${PS3_VFS_ROOT:-$des_vfs_fallback}"
  # Known-good boot config (docs/25): the LFQueue + SpursQueue families on
  # real libsre, and no round-notify heuristic.
  export LIBSRE_LLE="${LIBSRE_LLE:-1}"
  export LIBSRE_LLE_ONLY="${LIBSRE_LLE_ONLY:-011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667}"
  export SPURS_NO_ROUND_NOTIFY="${SPURS_NO_ROUND_NOTIFY:-1}"
  # Per-event log lines go through one FILE lock and change the timing being
  # measured (ps3_log.h); runs are quiet unless asked. PS3_VERBOSE=1 restores.
  export PS3_VERBOSE="${PS3_VERBOSE:-0}"
  # Test runs must not starve the machine: below-normal priority, on half the
  # logical CPUs (derived, not assumed). PS3_AFFINITY=all lifts the limit.
  export PS3_PRIORITY="${PS3_PRIORITY:-below}"
  if [ -z "${PS3_AFFINITY:-}" ]; then
    local n; n=$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")
    local half=$(( n / 2 )); [ "$half" -lt 1 ] && half=1; [ "$half" -gt 63 ] && half=63
    export PS3_AFFINITY="$(printf '%X' $(( (1 << half) - 1 )))"
  elif [ "$PS3_AFFINITY" = all ]; then unset PS3_AFFINITY; fi
  export PS3_PIDFILE="$DES_PIDFILE"
}

# Kill only des_boot processes this tooling started (their ids are in the pid
# file); a copy the user launched by hand is left alone.
des_kill_own() {
  [ -f "$DES_PIDFILE" ] || return 0
  local pid
  while read -r pid; do
    [ -n "$pid" ] || continue
    if tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot'; then
      taskkill //F //PID "$pid" > /dev/null 2>&1
    fi
  done < "$DES_PIDFILE"
  : > "$DES_PIDFILE"
}

# Is a des_boot we started still running?
des_running() {
  [ -f "$DES_PIDFILE" ] || return 1
  local pid
  while read -r pid; do
    [ -n "$pid" ] && tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot' && return 0
  done < "$DES_PIDFILE"
  return 1
}

# des_console <cmdfile-base> <command>: send one debug-console command and
# print the reply once it has been written (no fixed sleep; 15 s cap).
des_console() {
  local d="$1" c="$2" i
  rm -f "$d.out"; echo "$c" > "$d"
  for i in $(seq 1 150); do
    grep -aq '^\[dbg\] done' "$d.out" 2>/dev/null && break
    sleep 0.1
  done
  echo "== $c"; grep -av '^\[dbg\]' "$d.out" 2>/dev/null | grep -v '^$'
}

des_cache_dir() { echo "${DES_CACHE_DIR:-$PS3_VFS_ROOT/cache}"; }
