#!/usr/bin/env bash
# deslib.sh -- shared helpers for the des_boot test scripts. Source it.
#
# Machine-specific values come from the environment, or from
# tools/des/local.env (gitignored; copy local.env.example):
#   PS3_VFS_ROOT   disc root holding PS3_GAME (and the /dev_hdd1 cache dir;
#                  default work/disc)
#   DES_SCRATCH    where logs and the pid/console files go (default work/tmp)

DES_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$DES_ROOT/tools/des/local.env" ] && . "$DES_ROOT/tools/des/local.env"
DES_SCRATCH="${DES_SCRATCH:-$DES_ROOT/work/tmp}"; mkdir -p "$DES_SCRATCH"
# A play session (play.sh sets DES_KIND=play) stages into its own directory
# with its own pid file and marker, so a test run's des_kill_own never takes
# down the game the user is playing. A play session still clears leftover test
# runs and its own previous session.
if [ "${DES_KIND:-test}" = play ]; then
  DES_PIDFILE="$(cygpath -m "$DES_SCRATCH" 2>/dev/null || echo "$DES_SCRATCH")/des_boot_play.pids"
  DES_BIN="$DES_SCRATCH/play_bin"; DES_OWN_MARK=".des_play"
else
  DES_PIDFILE="$(cygpath -m "$DES_SCRATCH" 2>/dev/null || echo "$DES_SCRATCH")/des_boot.pids"
  DES_BIN="$DES_SCRATCH/bin"; DES_OWN_MARK=".des_staged"
fi
DES_STAGED_MARK=".des_staged"   # beside each test run's staged des_boot.exe

des_env_defaults() {
  export PS3_VFS_ROOT="${PS3_VFS_ROOT:-$DES_ROOT/work/disc}"
  # Known-good boot config (docs/25): the LFQueue + SpursQueue families on
  # real libsre, and no round-notify heuristic.
  export LIBSRE_LLE="${LIBSRE_LLE:-1}"
  export LIBSRE_LLE_ONLY="${LIBSRE_LLE_ONLY:-011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667}"
  export SPURS_NO_ROUND_NOTIFY="${SPURS_NO_ROUND_NOTIFY:-1}"
  # Per-event log lines go through one FILE lock and change the timing being
  # measured (ps3_log.h); runs are quiet unless asked. PS3_VERBOSE=1 restores.
  export PS3_VERBOSE="${PS3_VERBOSE:-0}"
  # The level's busiest frames need up to ~850k vertices, and growing the
  # vertex upload buffer during play stalled one frame for 100-400 ms. Start it
  # at the size the level grows it to (768 MB of upload memory).
  export VP_VB_VERTS="${VP_VB_VERTS:-1572864}"
  # Test runs are silent: AUDIO_MUTE submits zeros to the device at the normal
  # rate, so timing is unchanged. AUDIO_MUTE=0 to hear a run.
  export AUDIO_MUTE="${AUDIO_MUTE:-1}"
  # Test runs must not starve the machine: below-normal priority, on half the
  # logical CPUs (derived, not assumed). PS3_AFFINITY=all lifts the limit.
  export PS3_PRIORITY="${PS3_PRIORITY:-below}"
  if [ -z "${PS3_AFFINITY:-}" ]; then
    local n; n=$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")
    local half=$(( n / 2 )); [ "$half" -lt 1 ] && half=1; [ "$half" -gt 63 ] && half=63
    export PS3_AFFINITY="$(printf '%X' $(( (1 << half) - 1 )))"
  elif [ "$PS3_AFFINITY" = all ]; then unset PS3_AFFINITY; fi
  export PS3_PIDFILE="$DES_PIDFILE"
  # Compiled shaders, kept across runs and staged copies of the exe, beside the
  # saves; precompile_shaders.sh fills it from the game's shader archives.
  export PS3_SHADER_CACHE="${PS3_SHADER_CACHE:-$(cygpath -m "$DES_ROOT/gamedata/shader_cache" 2>/dev/null || echo "$DES_ROOT/gamedata/shader_cache")}"
  # F9 in the game window captures the next frame for rsx_replay; keep those
  # (game-derived) captures in the scratch directory, never the repository.
  export RSX_HOTKEY_DIR="${RSX_HOTKEY_DIR:-$(cygpath -m "$DES_SCRATCH/rsx_hotkey" 2>/dev/null || echo "$DES_SCRATCH/rsx_hotkey")}"
  # F11 starts and stops a profile session (prof_NNN_summary.txt and friends).
  export PROF_DIR="${PROF_DIR:-$(cygpath -m "$DES_SCRATCH/prof" 2>/dev/null || echo "$DES_SCRATCH/prof")}"
}

# des_stage_exe: copy the game binary (DES_EXE, default port/build/des_boot.exe)
# with its .pdb/.map and any DLLs beside it into $DES_BIN ($DES_SCRATCH/bin, or
# play_bin for a play session), and print the copy's path. Runs launch the
# copy, so the build output is never locked and the next fix can be linked
# while a test is still running. The copy keeps
# the name des_boot.exe (des_kill_own and des_running match on it). For
# sym.py against a staged run, pass --map "$DES_BIN/des_boot.map".
des_stage_exe() {
  local src="${DES_EXE:-$DES_ROOT/port/build/des_boot.exe}"
  local dir; dir="$(dirname "$src")"
  local bin="$DES_BIN"; mkdir -p "$bin"
  cp -f "$src" "$bin/des_boot.exe" || return 1
  : > "$bin/$DES_OWN_MARK"   # des_kill_own: this copy is ours
  local f
  for f in "${src%.exe}.pdb" "${src%.exe}.map" "$dir"/*.dll; do
    [ -f "$f" ] || continue
    case "$f" in *.pdb) cp -f "$f" "$bin/des_boot.pdb" ;;
                 *.map) cp -f "$f" "$bin/des_boot.map" ;;
                 *)     cp -f "$f" "$bin/" ;; esac
  done
  echo "$bin/des_boot.exe"
}

# Kill only des_boot processes this tooling started; a copy the user launched
# by hand is left alone. Two sources, so it works from any shell: the pid file
# of the current DES_SCRATCH and kind, and every running des_boot.exe whose exe
# has the test-run staging marker beside it (any DES_SCRATCH, including runs
# started from an earlier shell with a different one). The build output and a
# play session's copy have no test-run marker.
des_kill_own() {
  local pid exe
  if [ -f "$DES_PIDFILE" ]; then
    while read -r pid; do pid="${pid//[!0-9]/}"   # boot_main writes CRLF
      [ -n "$pid" ] || continue
      if tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot'; then
        taskkill //F //PID "$pid" > /dev/null 2>&1
      fi
    done < "$DES_PIDFILE"
    : > "$DES_PIDFILE"
  fi
  while IFS='|' read -r pid exe; do
    pid="${pid//[!0-9]/}"; exe="$(printf %s "$exe" | tr -d '\015')"   # powershell writes CRLF
    [ -n "$pid" ] && [ -n "$exe" ] || continue
    [ -f "$(dirname "$(cygpath -u "$exe")")/$DES_STAGED_MARK" ] || continue
    taskkill //F //PID "$pid" > /dev/null 2>&1
  done < <(powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='des_boot.exe'\" | ForEach-Object { '{0}|{1}' -f \$_.ProcessId, \$_.ExecutablePath }" 2>/dev/null)
}

# Is a des_boot we started still running?
des_running() {
  [ -f "$DES_PIDFILE" ] || return 1
  local pid
  while read -r pid; do pid="${pid//[!0-9]/}"
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
