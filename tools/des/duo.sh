#!/usr/bin/env bash
# duo.sh -- run two copies of the game on this PC at once, for online tests (docs/69): co-op, invasions and
# messages between two players. Each copy ("a" and "b") has its own folder under $DUO_DIR with its own saves,
# HDD cache, shader cache, settings.ini (so its own network settings and account), logs and pid file, and its own
# P2P port. Needs ps3recomp's PS3_SAVEDATA_ROOT and PS3_HDD1_ROOT; an older runtime shares one save folder and cache.
#
#   tools/des/duo.sh start [a|b|both] [ENV=val ...]   start one copy or both (default both)
#   tools/des/duo.sh stop  [a|b|both]                  stop them (only the copies this script started)
#   tools/des/duo.sh status                            which copies run, and the last lines of their logs
#
# ENV=val applies to both copies; A_ENV=val or B_ENV=val to one (e.g. B_AUDIO_MUTE=0).
# DUO_DIR       the copies' folders (default $DES_SCRATCH/duo; created on first start)
# DUO_SAVE      a save folder to copy into each copy's saves on its first start, e.g. a test save
#               (default: none, the copy starts with no save). Only ever copied from, never written.
# DUO_COPY_CACHE=0  start each copy with an empty HDD cache instead of a copy of the disc tree's (slower first boot)
# DUO_TEST=1    an unattended run: no start screen, no user input, offline, quiet (as test runs). Without it each
#               copy is a play session: its own settings.ini, the start screen, and its Network settings.
# DUO_MINUTES   how long a copy may run (default 120)
# DES_EXE       the build to run (default port/build/des_boot.exe); each copy runs its own staged copy of it.
#
# The two windows share the PC's input: keyboard and mouse go to the window in front, and so do the controllers in
# a play session (PAD_FOCUS_ONLY=1, which it sets unless given): one controller plays whichever copy is in front;
# click the other window or Alt+Tab to switch. PAD_FOCUS_ONLY=0 lets every copy read the controllers at once.
# Cores are split: "a" runs on the lower half of the logical CPUs and "b" on the upper half. Audio: "a" is heard,
# "b" is muted (A_AUDIO_MUTE / B_AUDIO_MUTE change it).
set -uo pipefail
. "$(dirname "$0")/deslib.sh"
cd "$DES_ROOT" || exit 1

cmd="${1:-status}"; shift || true
which=both
case "${1:-}" in a|b|both) which="$1"; shift ;; esac
DUO_DIR="${DUO_DIR:-$DES_SCRATCH/duo}"
copies() { case "$which" in both) echo a b ;; *) echo "$which" ;; esac; }
idx() { [ "$1" = a ] && echo 0 || echo 1; }
win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

duo_pids() { local f="$DUO_DIR/$1/des_boot.pids"; [ -f "$f" ] && tr -cd '0-9\n' < "$f"; }
duo_running() {
  local pid
  for pid in $(duo_pids "$1"); do
    tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi des_boot && return 0
  done
  return 1
}
duo_stop() {
  local pid
  for pid in $(duo_pids "$1"); do
    tasklist //FI "PID eq $pid" //FO CSV //NH 2>/dev/null | grep -qi des_boot && taskkill //F //PID "$pid" > /dev/null 2>&1
  done
  local t=$SECONDS   # taskkill returns before the process is gone
  while duo_running "$1" && [ $((SECONDS - t)) -lt 15 ]; do sleep 0.5; done
  : > "$DUO_DIR/$1/des_boot.pids" 2>/dev/null
}

# A copy's folder: saves/ (PS3_SAVEDATA_ROOT), hdd1/ (PS3_HDD1_ROOT; the cache), shader_cache/, trophies/
# (PS3_TROPHY_DIR), data/ (settings.ini, reports), scratch/ (logs, console, the staged exe). Filled on the first start
# only; the shader cache and trophies start as copies of the main ones.
duo_prepare() {
  local d="$DUO_DIR/$1"
  mkdir -p "$d/saves" "$d/hdd1" "$d/data" "$d/scratch" || return 1
  if [ -n "${DUO_SAVE:-}" ] && [ -z "$(ls -A "$d/saves")" ]; then
    [ -d "$DUO_SAVE" ] || { echo "duo: DUO_SAVE=$DUO_SAVE is not a folder"; return 1; }
    cp -a "$DUO_SAVE/." "$d/saves/" || return 1
    echo "duo: $1: saves copied from $DUO_SAVE"
  fi
  if [ ! -e "$d/hdd1/.seeded" ]; then
    local cache; cache="$(des_cache_dir)"
    if [ "${DUO_COPY_CACHE:-1}" = 1 ] && [ -d "$cache" ]; then
      echo "duo: $1: copying the HDD cache ($(du -sh "$cache" 2>/dev/null | cut -f1))"
      mkdir -p "$d/hdd1/cache" && cp -a "$cache/." "$d/hdd1/cache/" || return 1
    fi
    : > "$d/hdd1/.seeded"
  fi
  if [ ! -d "$d/shader_cache" ]; then
    mkdir -p "$d/shader_cache"
    if [ -d "$DES_ROOT/gamedata/shader_cache" ]; then cp -a "$DES_ROOT/gamedata/shader_cache/." "$d/shader_cache/" || return 1; fi
  fi
  if [ ! -d "$d/trophies" ]; then
    mkdir -p "$d/trophies"
    if [ -d "$DES_ROOT/gamedata/trophies" ]; then cp -a "$DES_ROOT/gamedata/trophies/." "$d/trophies/" || return 1; fi
  fi
  return 0
}

duo_start() {
  local n="$1" i d half n_cpu mask
  i=$(idx "$n"); d="$DUO_DIR/$n"
  duo_running "$n" && { echo "duo: $n is already running"; return 0; }
  duo_prepare "$n" || return 1
  n_cpu=$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")
  half=$(( n_cpu / 2 )); [ "$half" -lt 1 ] && half=1; [ "$half" -gt 31 ] && half=31
  if [ "$i" = 0 ]; then mask=$(( (1 << half) - 1 )); else mask=$(( ((1 << half) - 1) << half )); fi
  (
    export DES_SCRATCH="$d/scratch" DES_BIN="$d/scratch/bin" DES_PIDFILE="$(win "$d")/des_boot.pids"
    if [ "${DUO_TEST:-0}" = 1 ]; then
      export DES_KIND=test PS3_NO_USER_INPUT=1 DES_AUTO_CONTINUE=1 DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0
      export DES_ONLINE="${DES_ONLINE:-0}"
      DES_OWN_MARK=.des_staged
    else
      # a play session's staged exe has no test-run marker, so other test scripts' des_kill_own leave it alone
      export DES_KIND=play
      export PAD_FOCUS_ONLY="${PAD_FOCUS_ONLY:-1}"   # one controller for both: it plays the copy in front
      DES_OWN_MARK=.des_play
    fi
    des_env_defaults
    export PS3_PIDFILE="$DES_PIDFILE"
    export PS3_SAVEDATA_ROOT="$(win "$d/saves")" PS3_HDD1_ROOT="$(win "$d/hdd1")"
    export PS3_SHADER_CACHE="$(win "$d/shader_cache")" PS3_TROPHY_DIR="$(win "$d/trophies")" DES_DATA_DIR="$(win "$d/data")"
    export PS3_DEBUG="$(win "$d/scratch/console.txt")"
    export RSX_HOTKEY_DIR="$(win "$d/scratch/rsx_hotkey")" PROF_DIR="$(win "$d/scratch/prof")"
    export PS3_NET_P2P_PORT=$(( 3658 + i )) DES_NET_P2P_PORT=$(( 3658 + i ))
    export PS3_AFFINITY="$(printf '%X' "$mask")" PS3_PRIORITY="${PS3_PRIORITY:-below}"
    [ "$i" = 0 ] && export AUDIO_MUTE="${AUDIO_MUTE:-0}" || export AUDIO_MUTE=1
    export PS3_TITLE="Demon's Souls [$(echo "$n" | tr ab AB)]"
    local kv k up; up=$(echo "$n" | tr ab AB)
    for kv in "$@"; do
      case "$kv" in
        A_*=*|B_*=*) [ "${kv%%_*}" = "$up" ] && export "${kv#?_}" ;;
        *=*) export "$kv" ;;
      esac
    done
    EXE="$(des_stage_exe)" || { echo "duo: $n: could not stage the exe"; exit 1; }
    # The working folder stays the repository's, as for every run: the runtime reads files relative to it
    # (work/libsre/libsre_image.bin, gamedata/dev_bdvd). What a copy writes goes to the folders above.
    nohup timeout $(( ${DUO_MINUTES:-120} * 60 )) "$EXE" "$(win "$DES_ROOT/work/game/EBOOT.ELF")" \
      > "$d/scratch/play.txt" 2>&1 &
    echo "duo: $n started (pid $!, P2P port $PS3_NET_P2P_PORT, cores 0x$PS3_AFFINITY, log $d/scratch/play.txt)"
  )
}

case "$cmd" in
  start)  # Without PS3_SAVEDATA_ROOT (ps3recomp before #80) both copies would run on the default save folder, your own.
          src="${DES_EXE:-$DES_ROOT/port/build/des_boot.exe}"
          grep -aq PS3_SAVEDATA_ROOT "$src" || {
            echo "duo: $src doesn't know PS3_SAVEDATA_ROOT; both copies would use your own save. Build it with a newer ps3recomp."
            exit 1; }
          for n in $(copies); do duo_start "$n" "$@" || exit 1; done ;;
  stop)   for n in $(copies); do duo_stop "$n"; echo "duo: $n stopped"; done ;;
  status) for n in a b; do
            if duo_running "$n"; then echo "== $n: running"; else echo "== $n: not running"; fi
            [ -f "$DUO_DIR/$n/scratch/play.txt" ] && tail -3 "$DUO_DIR/$n/scratch/play.txt" | cut -c1-160
          done ;;
  *) sed -n '2,24p' "$0"; exit 2 ;;
esac
