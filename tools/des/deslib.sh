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

# True while the Windows session is at the lock screen (LogonUI is running).
des_session_locked() {
  tasklist //FI "IMAGENAME eq LogonUI.exe" //NH 2>/dev/null | grep -qi logonui
}

des_env_defaults() {
  export PS3_VFS_ROOT="${PS3_VFS_ROOT:-$DES_ROOT/work/disc}"
  # Known-good boot config (docs/25): the LFQueue + SpursQueue families on
  # real libsre, and no round-notify heuristic.
  export LIBSRE_LLE="${LIBSRE_LLE:-1}"
  export LIBSRE_LLE_ONLY="${LIBSRE_LLE_ONLY:-011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667}"
  export SPURS_NO_ROUND_NOTIFY="${SPURS_NO_ROUND_NOTIFY:-1}"
  # Behind the lock screen DWM holds the back buffers and every frame waits on
  # the GPU (docs/28, unattended runs). A test run checks the lock at launch
  # and renders without presenting there; set RSX_PRESENT to pin it instead.
  if [ "${DES_KIND:-test}" != play ] && [ -z "${RSX_PRESENT:-}" ]; then
    export RSX_PRESENT=$(des_session_locked && echo 0 || echo 1)
  fi
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

# des_console <cmdfile-base> <command> [cap-secs]: send one debug-console
# command and print the reply once it has been written (no fixed sleep; the
# cap defaults to 15 s, longer for commands like `prof <tid> <secs>`).
des_console() {
  local d="$1" c="$2" i
  rm -f "$d.out"; echo "$c" > "$d"
  for i in $(seq 1 $(( ${3:-15} * 10 ))); do
    grep -aq '^\[dbg\] done' "$d.out" 2>/dev/null && break
    sleep 0.1
  done
  echo "== $c"; grep -av '^\[dbg\]' "$d.out" 2>/dev/null | grep -v '^$'
}

des_cache_dir() { echo "${DES_CACHE_DIR:-$PS3_VFS_ROOT/cache}"; }

# des_put_save <from> <to>: make directory <to> hold a copy of <from>. It
# replaces what <to> holds, not <to> itself: while something has <to> open
# (a shell sitting in it, say), `rm -rf <to>` empties it but cannot remove
# it, and `cp -r <from> <to>` then puts the copy one level down, where the
# game does not look. If <from> is missing, <to> is left alone.
des_put_save() {
  [ -d "$1" ] || return 1
  mkdir -p "$2" || return 1
  find "$2" -mindepth 1 -delete 2>/dev/null
  cp -r "$1/." "$2/"
}

# des_backup_save <save> <backup>: copy the save aside and check the copy.
# Fails if the copy differs, so a script stops before it swaps the save.
des_backup_save() {
  des_put_save "$1" "$2" && diff -rq "$1" "$2" > /dev/null
}

# des_restore_save <backup> <save>: put the backup back and check it.
des_restore_save() {
  des_put_save "$1" "$2" && diff -rq "$1" "$2" > /dev/null && return 0
  echo "[save] could not put $1 back into $2; the backup is still there" >&2
  return 1
}

# The save folder belongs to one test run at a time. levelcap.sh and
# regress.sh copy the player's save aside, put another save in its place and
# put the player's back at the end. A second run that started before the
# first had put the save back copied the swapped-in save aside as the
# player's, and put that back at its own end.
#
# des_save_lock <save>: wait until no other run holds <save>, then hold it
# (the directory <save>.lock, with the holder's pid). Waits up to
# DES_SAVE_LOCK_WAIT seconds, a whole number (default 900, longer than a
# capped run; 0 tries once). The lock of a run that has gone (its pid has
# exited, or it never wrote one) is taken over by one waiter, the first to
# make claim.<pid> in it; the others wait for that one. If that run had
# swapped the save and not put it back (it was killed outright, or the copy
# back failed), its backup goes back first; if that fails too, the lock stays
# and this fails.
# des_save_note <save> <backup>: record in the lock that <backup> holds the
# player's save, before the swap.
# des_save_unlock <save>: let the folder go, once the save is back.
des_save_lock() {
  local lock="$1.lock" wait="${DES_SAVE_LOCK_WAIT:-900}" owner tries i nopid=0 said=
  case "$wait" in ''|*[!0-9]*)
    echo "[save] DES_SAVE_LOCK_WAIT=$wait: it takes a whole number of seconds" >&2; return 1 ;;
  esac
  mkdir -p "$(dirname "$1")" || return 1
  tries=$(( 10#$wait * 2 )); [ "$tries" -ge 1 ] || tries=1
  for ((i = 1; i <= tries; i++)); do
    if mkdir "$lock" 2>/dev/null; then echo $$ > "$lock/pid"; return 0; fi
    owner=$(cat "$lock/pid" 2>/dev/null); owner="${owner//[!0-9]/}"
    if [ -n "$owner" ]; then nopid=0; else nopid=$((nopid + 1)); fi
    # Gone: its pid has exited, or it never wrote one (5 s without).
    if { [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; } || [ $nopid -gt 10 ]; then
      if mkdir "$lock/claim.${owner:-none}" 2>/dev/null; then
        if [ "$(cat "$lock/pid" 2>/dev/null | tr -cd 0-9)" = "$owner" ]; then
          echo $$ > "$lock/pid"
          des_save_takeover "$1" "${owner:-?}"; return
        fi
        rmdir "$lock/claim.${owner:-none}" 2>/dev/null   # it changed hands meanwhile
      fi
    elif [ -z "$said" ]; then
      said=1; echo "[save] waiting for run ${owner:-?} to finish with $1" >&2
    fi
    if [ $i -lt $tries ]; then sleep 0.5; fi
  done
  echo "[save] gave up waiting for $1 ($lock)" >&2
  return 1
}

# des_save_takeover <save> <owner>: finish the lock of run <owner>, which has
# gone, for des_save_lock, once this shell has claimed it and written its pid
# in it. Returns 0 once the lock is this shell's.
des_save_takeover() {
  local save="$1" lock="$1.lock" b
  b=$(cat "$lock/backup" 2>/dev/null)
  if [ -n "$b" ]; then
    echo "[save] run $2 ended with $save swapped out; putting back $b" >&2
    des_restore_save "$b" "$save" || { echo "[save] leaving $lock in place" >&2; return 1; }
  fi
  rm -f "$lock/backup"; rm -rf "$lock"/claim.*
  return 0
}
des_save_note() { realpath "$2" > "$1.lock/backup"; }
des_save_unlock() {
  [ "$(cat "$1.lock/pid" 2>/dev/null)" = "$$" ] && rm -rf "$1.lock"
}
