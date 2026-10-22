#!/usr/bin/env bash
# perflib.sh -- shared helpers for the perf batch scripts (exeab.sh, knobab.sh,
# regfps.sh). Source it; it sources deslib.sh. How to run the batches: PERF.md.
#
#   perf_refuse_if_game       exit 3 if any des_boot.exe runs (ours or the user's)
#   perf_no_game [secs]       wait up to secs (default 0) until no des_boot.exe
#                             runs; false if one still does (the user's game)
#   perf_quiet [secs]         one CPU check over secs (quiet.ps1); false if busy.
#                             Sets PERF_BG (total CPU %) for the run log.
#   perf_wait_quiet [secs]    perf_quiet until it passes, up to PERF_QUIET_WAIT s
#                             (default 900), 30 s apart; returns 4 if never quiet
#   perf_present              pin RSX_PRESENT for the batch from the lock state
#   perf_present_ok           false if the session locked under a presenting batch
#   perf_uncapped             RSX_VSYNC=0 unless set: an uncapped run must not
#                             wait for the display's refresh
#   perf_header [wincheck]    the header lines every batch writes first
#   perf_md5 <file>           short md5 of a file
#   perf_save_snapshot <dir>  copy the user's save to <dir>/usersave_bak (refuses
#                             if that exists; never removed) and list its md5s
#   perf_save_check <dir>     compare the user's save with that list; false if
#                             it changed
#   perf_skip_cutscene <run> [cap]   press START / CROSS through the run's pad
#                             file while a cutscene plays, up to cap s (default 60)
#   perf_run_valid <run> [map] [vblank mHz]   print "ok" or why the boot
#                             cannot be used (a window within 10% of the vblank
#                             rate is one)
#   perf_game_pid             the pid this run's game wrote (DES_PIDFILE)
#   perf_wincap <pid> <png>   save that game's window with PrintWindow
#                             (wincap.ps1); prints "mean <0-255> size ... title ..."
#   perf_black <wincap line>  true if that capture is black (mean under
#                             PERF_BLACK, default 2)
#
# Knobs: PERF_QUIET=0 (no quiet check: numbers from a busy PC are noise, so
# only for a try-out), PERF_QUIET_MAX (total CPU %, default 30), PERF_QUIET_OURS (process
# names that belong to the batch, comma-separated) with PERF_QUIET_OUT (the
# most CPU % everything else may use; no check unless set), PERF_QUIET_WAIT,
# PERF_BLACK. Machine-specific ones go in tools/des/local.env.
. "$(dirname "${BASH_SOURCE[0]}")/deslib.sh"
PERF_USER_SAVE="$DES_ROOT/gamedata/dev_hdd0/home/00000001/savedata"   # levelcap.sh's SAVE

# The whole listing is read before it is tested: `| grep -q` under pipefail
# reads false when tasklist is still writing as grep exits.
perf_game_up() {
  local out
  out=$(tasklist //FI "IMAGENAME eq des_boot.exe" //NH 2>/dev/null | tr '[:upper:]' '[:lower:]')
  [[ "$out" == *des_boot* ]]
}

perf_refuse_if_game() {
  perf_game_up || return 0
  echo "[perf] a des_boot.exe is running: $(des_games < /dev/null | tr -d '\r' | cut -d'|' -f1,2 | tr '\n' ' ')"
  echo "[perf] not starting (a batch never stops a game it did not start)"
  exit 3
}

# A game levelcap.sh has just stopped can take a moment to go.
perf_no_game() {
  local t0=$SECONDS
  while perf_game_up; do
    [ $((SECONDS - t0)) -lt "${1:-0}" ] || return 1
    sleep 0.5
  done
}

perf_quiet() {
  local line args=(-Secs "${1:-5}")
  if [ "${PERF_QUIET:-1}" = 0 ]; then PERF_BG=off; echo "[perf] quiet check off (PERF_QUIET=0)"; return 0; fi
  [ -n "${PERF_QUIET_OURS:-}" ] && args+=(-Ours "$PERF_QUIET_OURS")
  line=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$DES_ROOT/tools/des/quiet.ps1")" \
    "${args[@]}" < /dev/null 2>/dev/null | tr -d '\r' | grep '^cpu ' | tail -1)
  [ -n "${PERF_LOG:-}" ] && echo "$(date +%T) ${line:-no reading}" >> "$PERF_LOG"
  echo "[perf] quiet check: ${line:-no reading}"
  [ -n "$line" ] || return 1
  PERF_BG=$(echo "$line" | awk '{v = $2; sub("%", "", v); print v}')
  echo "$line" | awk -v max="${PERF_QUIET_MAX:-30}" -v omax="${PERF_QUIET_OUT:-}" '{
    v = $2; sub("%", "", v); bad = v + 0 > max + 0
    if (omax != "" && $3 == "outside") { o = $4; sub("%", "", o); bad = bad || o + 0 > omax + 0 }
    exit bad }'
}

perf_wait_quiet() {
  local t0=$SECONDS
  until perf_quiet "${1:-5}"; do
    if [ $((SECONDS - t0)) -ge "${PERF_QUIET_WAIT:-900}" ]; then
      echo "[perf] the PC stayed busy for ${PERF_QUIET_WAIT:-900} s; stopping"; return 4
    fi
    sleep 30
  done
}

# Behind the lock screen every frame waits on DWM and a run reads ~4 fps, so a
# batch renders without presenting there (deslib.sh decides per run; a batch
# decides once, so both sides match).
perf_present() { export RSX_PRESENT="${RSX_PRESENT:-$(des_session_locked && echo 0 || echo 1)}"; }
# False once the session has locked under a presenting batch: from then on its
# boots would read the lock screen's rate, not the build's.
perf_present_ok() { [ "${RSX_PRESENT:-1}" = 0 ] || ! des_session_locked; }

# The runtime presents with vsync by default (RSX_VSYNC=1), which holds a
# presenting run near the display's refresh. Without tearing support the
# backend presents with vsync anyway and logs it (perf_run_valid checks).
perf_uncapped() { export RSX_VSYNC="${RSX_VSYNC:-0}"; }

perf_header() {   # perf_header [wincheck]: with the window check's state
  local wc=
  if [ "${1:-}" = wincheck ]; then
    if [ "${RSX_PRESENT:-1}" = 0 ]; then wc="  window check off (RSX_PRESENT=0: nothing is presented)"
    elif [ "${PERF_WINCHECK:-1}" = 0 ]; then wc="  window check off (PERF_WINCHECK=0)"
    else wc="  window check on (black below ${PERF_BLACK:-2})"; fi
  fi
  echo "start $(date '+%F %T')  repo $(git -C "$DES_ROOT" rev-parse --short HEAD 2>/dev/null || echo ?)"
  echo "RSX_PRESENT=${RSX_PRESENT:-unset}  RSX_VSYNC=${RSX_VSYNC:-unset (vsync)}$wc"
  if [ "${PERF_QUIET:-1}" = 0 ]; then echo "quiet check off (PERF_QUIET=0)"
  else echo "quiet: total cpu <= ${PERF_QUIET_MAX:-30}%${PERF_QUIET_OUT:+, outside <= ${PERF_QUIET_OUT}%}"; fi
}

perf_md5() { md5sum < "$1" | cut -c1-12; }

# Under the save folder's lock (des_save_lock): a run that has the pinned save
# swapped in holds it, so the copy is always the user's own save.
perf_save_snapshot() {
  local d="$1" rc=0
  [ -e "$d/usersave_bak" ] && { echo "[perf] $d/usersave_bak exists; pick a new tag"; return 2; }
  des_save_lock "$PERF_USER_SAVE" || { echo "[perf] the save folder is not free; not starting"; return 3; }
  if [ -d "$PERF_USER_SAVE" ]; then
    { des_backup_save "$PERF_USER_SAVE" "$d/usersave_bak" && des_save_files "$PERF_USER_SAVE" > "$d/usersave.md5"; } || rc=1
  else
    : > "$d/usersave.md5"; echo "[perf] no user save yet"
  fi
  des_save_unlock "$PERF_USER_SAVE"
  [ $rc = 0 ] || { echo "[perf] could not copy the user's save aside; not starting"; return 1; }
  echo "[perf] user save: $(grep -c . "$d/usersave.md5") files, copy in $d/usersave_bak"
}

perf_save_check() {
  local d="$1" now rc
  des_save_lock "$PERF_USER_SAVE" || { echo "[perf] could not take the save folder to check it"; return 1; }
  now=$(des_save_files "$PERF_USER_SAVE"); rc=$?
  des_save_unlock "$PERF_USER_SAVE"
  if [ $rc = 0 ] && [ "$now" = "$(cat "$d/usersave.md5")" ]; then echo "[perf] user save identical"; return 0; fi
  echo "[perf] USER SAVE DIFFERS from the list taken before the batch (copy from then: $d/usersave_bak)"
  return 1
}

# The game logs "[des] DES_FPS: cutscene, 30 fps" when a cutscene starts (and
# again if DES_CUTFPS changes during it) and "... cutscene over" when it ends,
# so the last of those lines decides.
perf_cutscene_open() {
  grep -a 'DES_FPS: cutscene' "$1/run.txt" 2>/dev/null | tail -1 | grep 'cutscene,' > /dev/null
}

perf_skip_cutscene() {
  local r="$1" cap="${2:-60}" t0=$SECONDS n=0
  perf_cutscene_open "$r" || return 0
  echo "[perf] cutscene playing; pressing START / CROSS"
  while perf_cutscene_open "$r" && [ $((SECONDS - t0)) -lt "$cap" ]; do
    if [ ! -s "$r/pad" ]; then
      if [ $((n % 2)) = 0 ]; then echo "0x0008 6" > "$r/pad"; else echo "0x4000 6" > "$r/pad"; fi
      n=$((n + 1))
    fi
    sleep 0.7
  done
  perf_cutscene_open "$r" && { echo "[perf] cutscene still playing after ${cap} s"; return 1; }
  echo "[perf] cutscene skipped after $((SECONDS - t0)) s ($n presses)"
}

perf_run_valid() {   # perf_run_valid <run> [map] [vblank mHz]
  local r="$1" map="${2:-}" got hit
  [ -f "$r/run.txt" ] || { echo "no run.txt"; return; }
  if [ -n "$map" ]; then
    got=$(grep -ao 'level loading ([^)]*/m[0-9][0-9]\.luabnd' "$r/run.txt" | head -1 | grep -o 'm[0-9][0-9]\.' | tr -d .)
    [ "$got" = "$map" ] || { echo "map ${got:-none} (want $map)"; return; }
  fi
  perf_cutscene_open "$r" && { echo "cutscene open (30 fps) during the windows"; return; }
  perf_present_ok || { echo "session locked while presenting"; return; }
  if [ "${RSX_PRESENT:-1}" != 0 ] && [ "${RSX_VSYNC:-1}" = 0 ] &&
     grep -aq 'RSX_VSYNC=0: tearing is not supported' "$r/run.txt"; then
    echo "presented with vsync (no tearing support): held at the display's refresh"; return
  fi
  # (compgen, not ls: with pipefail an ls over a glob that matches nothing fails the pipe)
  compgen -G "$r/prof_*_summary.txt" > /dev/null || compgen -G "$r/plain_*.txt" > /dev/null ||
    { echo "no windows"; return; }
  # The vblank has to stay well above the frame rate measured: a window within
  # 10% of it may be held by it (at 240 Hz the Tutorial spot sat at 240.0).
  if [ -n "${3:-}" ]; then
    hit=$(for f in "$r"/prof_*_summary.txt "$r"/plain_*.txt; do [ -f "$f" ] && head -1 "$f"; done | grep -o '[0-9.]* fps average' |
          awk -v cap="$3" '$1 + 0 >= 0.9 * cap / 1000 {print $1; exit}')
    [ -z "$hit" ] || { echo "near the vblank cap ($hit fps at $(( $3 / 1000 )) Hz; raise PERF_VBLANK)"; return; }
  fi
  echo ok
}

perf_game_pid() { tr -cd '0-9\n' < "$DES_PIDFILE" 2>/dev/null | grep . | tail -1; }

perf_wincap() {
  [ -n "$1" ] || { echo "no pid"; return; }
  mkdir -p "$(dirname "$2")"
  powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$DES_ROOT/tools/des/wincap.ps1")" \
    -ProcessId "$1" -Out "$(cygpath -w "$2")" < /dev/null 2>/dev/null | tr -d '\r' | grep -E '^(mean|no) ' | tail -1
}

perf_black() {
  case "$1" in "mean "*) ;; *) return 1 ;; esac
  echo "$1" | awk -v b="${PERF_BLACK:-2}" '{exit !($2 + 0 < b + 0)}'
}
