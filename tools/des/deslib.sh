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

# des_window_launch [route]: how a test run's window opens, as DES_LAUNCH, the
# words to put before the program (winmin.py, or none). DES_WINDOW:
#   auto    (the default) while someone is at the PC (the session unlocked):
#           min. While the session is locked: as usual.
#   min     minimized, without the focus, so it never covers what they are
#           looking at; on the vulkan route hidden instead, as the Vulkan
#           backend cannot make a swap chain for a window that starts
#           minimized yet.
#   hide, normal   always that.
# The route is the argument, else RSX_BACKEND. play.sh does not use this: a
# play session's window opens as usual. The A/B tools ask for normal (they
# save the window with PrintWindow, which a minimized window does not draw).
des_window_launch() {
  DES_LAUNCH=()
  [ "${OS:-}" = Windows_NT ] || return 0
  local m=${DES_WINDOW:-auto} route=${1:-${RSX_BACKEND:-d3d12}}
  if [ "$m" = auto ]; then
    if des_session_locked; then m=normal; else m=min; fi
  fi
  [ "$m" = min ] && [ "${route,,}" = vulkan ] && m=hide
  case "$m" in
    min)  DES_LAUNCH=(python "$(cygpath -m "$DES_ROOT/tools/des/winmin.py")") ;;
    hide) DES_LAUNCH=(python "$(cygpath -m "$DES_ROOT/tools/des/winmin.py")" --hide) ;;
  esac
}

des_env_defaults() {
  export PS3_VFS_ROOT="${PS3_VFS_ROOT:-$DES_ROOT/work/disc}"
  # Known-good boot config (docs/25): the LFQueue + SpursQueue families on
  # real libsre, and no round-notify heuristic.
  export LIBSRE_LLE="${LIBSRE_LLE:-1}"
  export LIBSRE_LLE_ONLY="${LIBSRE_LLE_ONLY:-011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667}"
  export SPURS_NO_ROUND_NOTIFY="${SPURS_NO_ROUND_NOTIFY:-1}"
  # The main thread's stack in the stack region, as lv2 places it: the lifter's
  # stack stores take their direct path only there (docs/98).
  export PS3_MAIN_STACK_LV2="${PS3_MAIN_STACK_LV2:-1}"
  # Behind the lock screen DWM holds the back buffers and every frame waits on
  # the GPU (docs/28, unattended runs). A test run checks the lock at launch
  # and renders without presenting there; set RSX_PRESENT to pin it instead.
  if [ "${DES_KIND:-test}" != play ] && [ -z "${RSX_PRESENT:-}" ]; then
    export RSX_PRESENT=$(des_session_locked && echo 0 || echo 1)
  fi
  # Test runs ignore the user's pads, keyboard and mouse, and their window
  # never takes the focus (PS3_NO_USER_INPUT, ps3recomp's ps3_knob.h): someone
  # at the PC cannot nudge a run, and their typing stays in their own window.
  # Scripted input still drives it. PS3_NO_USER_INPUT=0 to drive one by hand.
  if [ "${DES_KIND:-test}" != play ]; then
    export PS3_NO_USER_INPUT="${PS3_NO_USER_INPUT:-1}"
    # Test runs stay offline (settings Online is on by default); DES_ONLINE=1 to test online play.
    export DES_ONLINE="${DES_ONLINE:-0}"
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
  # F8 to F11 are developer keys: a player's build ignores them, and these runs
  # (play.sh too) turn them on (ps3recomp's RSX_DEV_HOTKEYS; docs/28).
  # RSX_DEV_HOTKEYS=0 to see what a player gets.
  export RSX_DEV_HOTKEYS="${RSX_DEV_HOTKEYS:-1}"
  export PROF_DIR="${PROF_DIR:-$(cygpath -m "$DES_SCRATCH/prof" 2>/dev/null || echo "$DES_SCRATCH/prof")}"
}

# des_stage_exe: copy the game binary (DES_EXE, default port/build/des_boot.exe)
# with its .pdb/.map and any DLLs beside it into $DES_BIN ($DES_SCRATCH/bin, or
# play_bin for a play session), and print the copy's path. Runs launch the
# copy, so the build output is never locked and the next fix can be linked
# while a test is still running. The copy keeps
# the name des_boot.exe (des_kill_own and des_running match on it). For
# sym.py against a staged run, pass --map "$DES_BIN/des_boot.map".
# A DLL an earlier staging left that this exe has none of beside it goes first:
# in an A/B whose sides differ in DLLs (one side with vkd3d-proton's d3d12.dll),
# it would otherwise load with the other side's exe too.
des_stage_exe() {
  local src="${DES_EXE:-$DES_ROOT/port/build/des_boot.exe}"
  local dir; dir="$(dirname "$src")"
  local bin="$DES_BIN"; mkdir -p "$bin"
  local f
  for f in "$bin"/*.dll; do
    [ -f "$f" ] && [ ! -f "$dir/$(basename "$f")" ] || continue
    rm -f "$f" || { echo "[des] cannot remove $f from the last staging" >&2; return 1; }
  done
  cp -f "$src" "$bin/des_boot.exe" || return 1
  : > "$bin/$DES_OWN_MARK"   # des_kill_own: this copy is ours
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
# It returns once they have gone: taskkill only starts a process's end, and a
# save put back meanwhile could take the game's last writes. Then it looks
# once more, for a game one of them had just started (setup's Play starts the
# game anew, docs/88 section 7). Fails if one is still running after that; its
# pid stays in the pid file, for des_stop_games.
des_kill_own() {
  local pid exe ids="" round left=""
  if [ -f "$DES_PIDFILE" ]; then
    while read -r pid; do pid="${pid//[!0-9]/}"   # boot_main writes CRLF
      [ -n "$pid" ] || continue
      if tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot'; then
        taskkill //F //PID "$pid" > /dev/null 2>&1; ids="$ids $pid"
      fi
    done < "$DES_PIDFILE"
    : > "$DES_PIDFILE"
  fi
  for round in 1 2; do
    while IFS='|' read -r pid exe _; do
      pid="${pid//[!0-9]/}"; exe="$(printf %s "$exe" | tr -d '\015')"   # powershell writes CRLF
      [ -n "$pid" ] && [ -n "$exe" ] || continue
      [ -f "$(dirname "$(cygpath -u "$exe")")/$DES_STAGED_MARK" ] || continue
      taskkill //F //PID "$pid" > /dev/null 2>&1; ids="$ids $pid"
    done < <(des_games)
    [ -n "$ids" ] || break
    des_wait_gone $ids
    left="$left$ids"; ids=""
  done
  [ -n "$left" ] || return 0
  for pid in $(printf '%s\n' $left | sort -u); do
    tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot' || continue
    echo "[des] des_boot $pid is still running after taskkill" >&2; ids="$ids $pid"
  done
  [ -n "$ids" ] || return 0
  for pid in $ids; do echo "$pid"; done >> "$DES_PIDFILE"
  return 1
}

# des_wait_gone <pid>...: wait, at most 10 s, for those processes to end.
des_wait_gone() {
  local ids; ids=$(echo "$*" | tr -s ' ' ','); ids="${ids#,}"
  [ -n "$ids" ] || return 0
  powershell -NoProfile -Command "Wait-Process -Id $ids -Timeout 10 -ErrorAction SilentlyContinue" > /dev/null 2>&1
  return 0
}

# des_games: "<pid>|<exe path>|<start>" for each running des_boot.exe, the
# start in seconds since 1970 (empty when it cannot be read).
des_games() {
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='des_boot.exe'\" | ForEach-Object { \$t = ''; if (\$_.CreationDate) { \$t = ([DateTimeOffset]\$_.CreationDate).ToUnixTimeSeconds() }; '{0}|{1}|{2}' -f \$_.ProcessId, \$_.ExecutablePath, \$t }" 2>/dev/null
}

# des_stop_games <pidfile>: stop the games of one run, the pids in its pid
# file (DES_PIDFILE, where boot_main writes its pid), and wait until they have
# gone; then empty the file. A pid there is stopped only while it is a
# des_boot.exe started from a staged test copy (the marker beside it), and only
# if it started before the file was last written: a game writes its pid when it
# starts, so a pid reused since, by the player's game or another run's, is left
# alone. Fails if one is still running after 10 s, or if one's start cannot be
# read.
des_stop_games() {
  local pid exe start since want=" " i left=0
  [ -s "$1" ] || return 0
  since=$(stat -c %Y "$1" 2>/dev/null) || return 1
  while read -r pid || [ -n "$pid" ]; do
    pid="${pid//[!0-9]/}"; [ -n "$pid" ] && want="$want$pid "
  done < "$1"
  while IFS='|' read -r pid exe start; do
    pid="${pid//[!0-9]/}"; exe="$(printf %s "$exe" | tr -d '\015')"; start="${start//[!0-9]/}"
    [ -n "$pid" ] && [ -n "$exe" ] || continue
    case "$want" in *" $pid "*) ;; *) continue ;; esac
    [ -f "$(dirname "$(cygpath -u "$exe")")/$DES_STAGED_MARK" ] || continue
    if [ -z "$start" ]; then
      echo "[save] des_boot $pid ($exe): cannot tell when it started" >&2; left=1; continue
    fi
    if [ "$start" -gt "$since" ]; then
      echo "[save] des_boot $pid ($exe) started after $1 was written: not that run's game; left alone" >&2
      continue
    fi
    taskkill //F //PID "$pid" > /dev/null 2>&1
    for i in $(seq 1 50); do
      tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot' || continue 2
      sleep 0.2
    done
    echo "[save] des_boot $pid ($exe) is still running" >&2; left=1
  done < <(des_games)
  [ $left = 0 ] && : > "$1"
  return $left
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

# des_wait_own <secs>: wait, at most <secs>, for the des_boot processes of the
# pid file that still run to end. Setup's Play (the launch hook, docs/88
# section 7) starts the game as a new process of the same exe and ends the
# first, so a run goes on past the process it started.
des_wait_own() {
  local pid ids=""
  [ "${1:-0}" -gt 0 ] && [ -f "$DES_PIDFILE" ] || return 0
  while read -r pid; do pid="${pid//[!0-9]/}"
    [ -n "$pid" ] && tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi 'des_boot' && ids="${ids:+$ids,}$pid"
  done < "$DES_PIDFILE"
  [ -n "$ids" ] || return 0
  powershell -NoProfile -Command "Wait-Process -Id $ids -Timeout $1 -ErrorAction SilentlyContinue" > /dev/null 2>&1
  return 0
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
  if [ -d "$1" ]; then echo "[save] could not put $1 back into $2; the backup is still there" >&2
  else echo "[save] could not put $1 back into $2: that backup is missing" >&2; fi
  return 1
}

# A test run plays from its own copy of a save, in a folder of its scratch
# directory that the game takes as its save folder (ps3recomp's
# PS3_SAVEDATA_ROOT, cellSaveData.c). The player's save folder is then neither
# swapped nor put back, and a game that outlives its run writes only the copy.
# Builds from before PS3_SAVEDATA_ROOT (ps3recomp #80) swap the player's
# save instead (regress.sh, levelcap.sh).
#
# des_save_root_ok: true if the build that runs (DES_EXE, or the default that
# des_stage_exe copies) or a DLL beside it reads PS3_SAVEDATA_ROOT.
des_save_root_ok() {
  local src="${DES_EXE:-$DES_ROOT/port/build/des_boot.exe}" f
  for f in "$src" "$(dirname "$src")"/*.dll; do
    [ -f "$f" ] && grep -aqF PS3_SAVEDATA_ROOT "$f" && return 0
  done
  return 1
}

# des_run_save <from> <dir>: make <dir> a checked copy of the save folder
# <from> (empty if there is none: a new game), and print <dir> as
# PS3_SAVEDATA_ROOT takes it. A copy of the player's own save folder is made
# under its lock (des_save_lock), so not while a play session (play.sh) or
# another run holds it.
des_run_save() {
  local live="$DES_ROOT/gamedata/dev_hdd0/home/00000001/savedata" rc
  if [ ! -d "$1" ]; then
    rm -rf "$2" && mkdir -p "$2" || return 1
  elif [ "$(realpath -m "$1")" = "$(realpath -m "$live")" ]; then
    des_save_lock "$live" || return 1
    des_backup_save "$1" "$2"; rc=$?
    des_save_unlock "$live"
    [ $rc = 0 ] || return 1
  else
    des_backup_save "$1" "$2" || return 1
  fi
  cygpath -m "$2"
}

# des_save_files <dir>: "<md5> <path under dir>" for each file in <dir>,
# sorted by path; nothing for an empty or missing folder.
des_save_files() {
  local d="${1%/}" rels=() paths=() r h i=0
  [ -d "$d" ] || return 0
  mapfile -t rels < <(find "$d" -type f -printf '%P\n' | LC_ALL=C sort)
  [ ${#rels[@]} -gt 0 ] || return 0
  for r in "${rels[@]}"; do paths+=("$d/$r"); done
  while read -r h _; do echo "${h#\\} ${rels[i]}"; i=$((i + 1)); done < <(md5sum "${paths[@]}")
  [ $i = ${#rels[@]} ]
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
# capped run; 0 tries once). Returns 0 once it holds the folder, 1 if another
# run still holds it, 2 if a run that has gone left the save swapped out and
# this one did not undo that: the lock stays, and the [save] lines say what
# was found and what to do.
#
# The lock of a run that has gone (its pid has exited, or it never wrote one)
# is taken over by one waiter, the first to make claim.<pid> in it; the
# others wait for that one. If the run had swapped the save and not put it
# back (it was killed outright, or the copy back failed), its game is stopped
# first, then its backup goes back, but only over what that run left there:
# files of the save it put in or of the backup, or none. Any other file may be
# newer play, so then the folder and the lock are left as they are. Running
# again with DES_SAVE_TAKEOVER=restore:<id> puts the backup back all the
# same; DES_SAVE_TAKEOVER=keep:<id> goes on from what the folder holds. The
# message gives the <id> (that lock's), so a setting left behind does not
# apply to a later lock.
#
# des_save_note <save> <backup>: record in the lock, before the swap, that
# <backup> holds the player's save, the files it holds, and where this run's
# game writes its pid.
# des_save_swap <from> <save>: record in the lock the files of <from>, then
# put a copy of <from> in <save>.
# des_save_unlock <save>: let the folder go, once the save is back.
des_save_lock() {
  local lock="$1.lock" wait="${DES_SAVE_LOCK_WAIT:-900}" owner tries i nopid=0 said=
  case "$wait" in ''|*[!0-9]*)
    echo "[save] DES_SAVE_LOCK_WAIT=$wait: it takes a whole number of seconds" >&2; return 1 ;;
  esac
  mkdir -p "$(dirname "$1")" || return 1
  tries=$(( 10#$wait * 2 )); [ "$tries" -ge 1 ] || tries=1
  for ((i = 1; i <= tries; i++)); do
    if mkdir "$lock" 2>/dev/null; then echo $$ > "$lock/pid"; des_save_id "$lock"; return 0; fi
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
# in it. Returns as des_save_lock does.
des_save_takeover() {
  local save="$1" lock="$1.lock" who="run $2" b id how= bad= have unknown sig= traps rc
  b=$(cat "$lock/backup" 2>/dev/null); id=$(cat "$lock/id" 2>/dev/null)
  case "${DES_SAVE_TAKEOVER:-}" in "restore:$id") how=restore ;; "keep:$id") how=keep ;; esac
  if [ -n "$b" ]; then
    # If only the script was killed, its game is still running, and it can
    # write the save while the backup goes back.
    if ! des_stop_games "$(cat "$lock/pidfile" 2>/dev/null)"; then
      echo "[save] $who ended with $save swapped out, and its game is still running; leaving $lock in place" >&2
      return 2
    fi
    if [ ! -d "$b" ]; then bad="is missing"
    elif [ -f "$lock/backup.md5" ] && [ "$(des_save_files "$b")" != "$(cat "$lock/backup.md5")" ]; then
      bad="has changed since it was checked"
    fi
    if [ -n "$bad" ]; then
      if [ "$how" != keep ]; then
        echo "[save] $who ended with $save swapped out, and its backup $b $bad." >&2
        des_save_found "$save" "$lock/swapped"
        echo "[save] $save is left as it is, and so is $lock. If $save holds a save to go on from, run again with DES_SAVE_TAKEOVER=keep:$id." >&2
        return 2
      fi
    elif [ -z "$how" ]; then
      if ! have=$(des_save_files "$save"); then
        echo "[save] $who ended with $save swapped out, and not every file there can be read; leaving it and $lock as they are" >&2
        return 2
      fi
      unknown=$(printf '%s\n' "$have" | grep . | grep -vxF -f <(des_save_files "$b"; cat "$lock/swapped" 2>/dev/null))
      if [ -n "$unknown" ]; then
        echo "[save] $who ended with $save swapped out, and the folder has changed since: $(printf '%s\n' "$unknown" | wc -l) file(s) there are in neither its backup nor the save it put there:" >&2
        printf '%s\n' "$unknown" | head -5 | while read -r _ f; do echo "[save]   $(date -r "$save/$f" '+%F %T') $f" >&2; done
        echo "[save] That may be newer play, so it is left as it is, and so is $lock; $b still holds the save from before that run." >&2
        echo "[save] To put that backup back anyway, run again with DES_SAVE_TAKEOVER=restore:$id; to go on from what $save holds, DES_SAVE_TAKEOVER=keep:$id." >&2
        return 2
      fi
      how=restore
    fi
    if [ "$how" = restore ]; then
      echo "[save] $who ended with $save swapped out; putting back $b" >&2
      # The copy ignores signals; one that comes meanwhile takes effect once
      # the save is back.
      traps=$(trap -p TERM INT HUP)
      trap 'sig=TERM' TERM; trap 'sig=INT' INT; trap 'sig=HUP' HUP
      ( trap '' TERM INT HUP; des_restore_save "$b" "$save" ); rc=$?
      trap - TERM INT HUP; eval "$traps"
      if [ $rc != 0 ]; then
        echo "[save] leaving $lock in place" >&2
        if [ -n "$sig" ]; then kill -s "$sig" $$; fi
        return 2
      fi
    else
      echo "[save] $who ended with $save swapped out; going on from what it holds (DES_SAVE_TAKEOVER=keep:$id)" >&2
    fi
  fi
  rm -f "$lock/backup" "$lock/backup.md5" "$lock/swapped" "$lock/pidfile"
  rm -rf "$lock"/claim.*
  des_save_id "$lock"
  if [ -n "$sig" ]; then kill -s "$sig" $$; fi
  return 0
}

# des_save_found <save> <files>: say what <save> holds, next to the file
# list <files> (des_save_files) of the save a run put there.
des_save_found() {
  local all n other
  all=$(des_save_files "$1"); n=$(printf '%s' "$all" | grep -c .)
  if [ "$n" = 0 ]; then echo "[save] $1 is empty." >&2; return 0; fi
  other=$(printf '%s\n' "$all" | grep -vxF -f <(cat "$2" 2>/dev/null) | grep -c .)
  if [ "$other" = 0 ]; then echo "[save] $1 holds the save that run put there ($n files)." >&2
  else echo "[save] $1 holds $n files; $other of them are not from the save that run put there." >&2; fi
}

des_save_id() { echo "$$-$(date +%s)" > "$1/id"; }
des_save_note() {
  rm -f "$1.lock/swapped"
  echo "$DES_PIDFILE" > "$1.lock/pidfile" && des_save_files "$2" > "$1.lock/backup.md5" &&
    realpath "$2" > "$1.lock/backup"
}
des_save_swap() { des_save_files "$1" > "$2.lock/swapped" && des_put_save "$1" "$2"; }
des_save_unlock() {
  [ "$(cat "$1.lock/pid" 2>/dev/null)" = "$$" ] && rm -rf "$1.lock"
}
