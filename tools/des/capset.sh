#!/usr/bin/env bash
# capset.sh [spot...] [-- ENV...]: take the capture set for the renderer gates
# (halgate.sh, docs/120) from live game runs.
#
# One boot per spot (the list below; give spot names to take only those):
# each boots like knobab.sh (abboot.sh: levelcap.sh from the spot's save, with
# DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 and PS3_SAIL_MOVIES=0, a cutscene
# pressed through), waits for the level and CAPSET_SETTLE s (default 40), then
# takes each variant below: it sets the variant's knobs live (debug console
# setenv), waits CAPSET_FLIPS presents (default 60; a knob that logs when it
# takes effect is waited for too), saves the next present as the capture's
# live picture (CELLMARK_DUMP_NOW), arms RSX_REPLAY_CAP=<capture> and waits for
# "[RCAP] wrote the capture", then clears the knobs. A variant whose knob
# cannot take effect is skipped with a note: RSX_HDR=1 when the display has
# no HDR. "@chain:N" takes N captures back to back (replayed chained by
# halgate.sh --chain), "@menu" presses START (PAD_FILE) and captures with the
# pause menu open; it runs last in the boot, whatever its place in the list,
# and its live picture is the only check that the menu opened.
#
# Captures go to $DES_SCRATCH/capset/<md5 of the exe, 12 digits>/ (CAPSET_DIR
# to pick another): <spot>_<variant>.rcap, the backend's live fingerprints
# beside each (<name>_cap0_d<n>.txt, for capdiff.py), live/<name>.bmp, and
# capset.json (what halgate.sh reads):
#   exe, exe_md5          the game build that took them (DES_EXE, default
#                         port/build/des_boot.exe)
#   replay, replay_md5    the rsx_replay of the same build (RSX_REPLAY,
#                         default $PS3RECOMP_DIR/build/rsx_replay.exe, the
#                         checkout default ps3recomp/): halgate.sh
#                         refuses any other, as the state sections are raw
#                         structs of one build
#   ps3recomp_rev, port_rev, present, created
#   captures: name, file, spot, map, variant, knobs (["RSX_SCALE=150"]: the
#             replay needs them too), group (a chain), live, methods, mb, note
#   skipped:  spot, variant, why
#   replay_check          one capture replayed for a frame (CAPSET_VERIFY=0
#                         skips it): "ok" or what the replay said
# A folder that exists already is left alone (the script stops).
#
# Checks (perflib.sh, as knobab.sh): no des_boot.exe or des_host.exe may run
# when it starts, nor before a boot (the user's game: it stops); the user's
# save is copied aside once (work/usersave_bak, never removed) and compared
# after every boot (a change stops it); each game plays from its own copy of
# the spot's save (PS3_SAVEDATA_ROOT) and never sees the user's save folder.
# RSX_PRESENT is pinned once from the lock state (0 while locked). Only the
# games this script started are ever stopped. Extra ENV... after "--" go to
# every boot.
#
# Exit status: 0 done (skipped variants are in capset.json), 1 a spot's boot
# failed or a capture did not come, 2 bad arguments or the folder exists,
# 3 a game was running or the save folder was not free, 5 the
# user's save changed.
#
# The captures are the title's own data: keep them out of any repository.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

# ---------------------------------------------------------------------------
# The spots: <name> <save folder, from the repository root> <map it loads>.
# A spot whose save is missing is skipped with a note.
CAPSET_SPOTS=${CAPSET_SPOTS:-'
regress  work/tmp/regress/save  m08
deep     work/tmp/lc_deep/save  m02
nexus    work/tmp/lc_nex/save   m01
'}
# The variants of every boot: <name> <knobs, comma-separated, or "-" for
# none, or @chain:N, or @menu>.
CAPSET_VARIANTS=${CAPSET_VARIANTS:-'
default   -
scale150  RSX_SCALE=150
scale200  RSX_SCALE=200
aa1       RSX_AA=1
aa2       RSX_AA=2
hdr       RSX_HDR=1
chain     @chain:3
menu      @menu
'}
# ---------------------------------------------------------------------------

if [ -n "${CAPSET_CHILD:-}" ]; then
  # One boot: <spot> <map> [ENV...]. CAPSET_CHILD is the batch's work folder,
  # CAPSET_OUT the capture folder.
  SPOT=$1; MAP=$2; shift 2
  TAG=cs_$SPOT
  OUT=$CAPSET_OUT; ROWS=$CAPSET_CHILD/captures.tsv; SKIPS=$CAPSET_CHILD/skipped.tsv
  . "$HERE/perflib.sh"
  . "$HERE/abboot.sh"   # boots, waits for the level and KAB_SETTLE; defines con, ab_finish
  LOG=$RUN/run.txt; NFL=${CAPSET_FLIPS:-60}; bad=0
  say() { echo "[capset] $(date +%T) $SPOT: $*"; }
  skip() { printf '%s\t%s\t%s\n' "$SPOT" "$1" "$2" >> "$SKIPS"; say "skipped $1: $2"; }
  alive() { kill -0 "$LC" 2>/dev/null; }
  flips() { con stat 5 | awk '$1 == "flips" {print $2; exit}'; }
  count() { grep -ac -- "$1" "$LOG" 2>/dev/null || true; }
  # wait_flips <n> [cap s]: until the game has presented n more frames.
  wait_flips() {
    local f0 f t0=$SECONDS
    f0=$(flips)
    while :; do
      alive || return 1
      f=$(flips)
      [ -n "$f0" ] && [ -n "$f" ] && [ $((f - f0)) -ge "$1" ] && return 0
      [ $((SECONDS - t0)) -lt "${2:-60}" ] || return 1
      sleep 0.5
    done
  }
  # wait_line <pattern> <count before> [cap s]: until the log has one more.
  wait_line() {
    local t0=$SECONDS
    while [ "$(count "$1")" -le "$2" ]; do
      alive || return 1
      [ $((SECONDS - t0)) -lt "${3:-30}" ] || return 1
      sleep 0.3
    done
  }
  # wait_pad [cap s]: until the runtime has taken the press from the pad file.
  wait_pad() {
    local t0=$SECONDS
    while [ -s "$RUN/pad" ]; do
      alive || return 1
      [ $((SECONDS - t0)) -lt "${1:-10}" ] || return 1
      sleep 0.2
    done
  }
  m=$(grep -ao 'level loading ([^)]*/m[0-9][0-9]\.luabnd' "$LOG" | head -1 | grep -o 'm[0-9][0-9]\.' | tr -d .)
  [ "$m" = "$MAP" ] || say "note: loaded map ${m:-unknown}, the spot list says $MAP"
  MAP=${m:-$MAP}
  mkdir -p "$OUT/live"
  # take <name> <knobs> <group> <shot|noshot> [note]: one capture, its live
  # picture first (so the readback never lands in the captured frame). The
  # first CELLMARK_DUMP_NOW also ends levelcap.sh's own early dumps.
  take() {
    local name=$1 knobs=$2 group=$3 f="$OUT/$1.rcap" live="" n0 e0 d0 p w note=${5:-}
    [ -e "$f" ] && { skip "${name#"${SPOT}"_}" "$f exists"; return 1; }
    if [ "$4" = shot ]; then
      d0=$(count '\[D3D12\] dumped')
      con "setenv CELLMARK_DUMP_DIR=$(cygpath -m "$OUT/live")" > /dev/null
      con "setenv CELLMARK_DUMP_NOW=" > /dev/null; con "setenv CELLMARK_DUMP_NOW=1" > /dev/null
      if wait_line '\[D3D12\] dumped' "$d0" 20; then
        p=$(grep -a '\[D3D12\] dumped' "$LOG" | tail -1 | sed 's/^.*dumped \(.*\.bmp\) (.*$/\1/')
        if [ -f "$p" ] && mv -f "$p" "$OUT/live/$name.bmp"; then live="live/$name.bmp"; fi
      fi
      con "setenv CELLMARK_DUMP_NOW=" > /dev/null
      [ -n "$live" ] || note="${note:+$note; }no live picture"
    fi
    n0=$(count '\[RCAP\] wrote the capture'); e0=$(count '\[RCAP\] cannot write')
    con "setenv RSX_REPLAY_CAP=$(cygpath -m "$f")" > /dev/null
    local t0=$SECONDS
    while [ "$(count '\[RCAP\] wrote the capture')" -le "$n0" ]; do
      if [ "$(count '\[RCAP\] cannot write')" -gt "$e0" ] || ! alive || [ $((SECONDS - t0)) -ge 120 ]; then
        say "$name: no capture ($(grep -a '\[RCAP\]' "$LOG" | tail -1))"; bad=1; return 1
      fi
      sleep 0.3
    done
    w=$(grep -a '\[RCAP\] wrote the capture' "$LOG" | tail -1)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$name.rcap" "$SPOT" "$MAP" "$VAR" "$knobs" \
      "$group" "$live" "$(echo "$w" | sed -n 's/.*: \([0-9]*\) methods.*/\1/p')" \
      "$(echo "$w" | sed -n 's/.*, \([0-9]*\) MB.*/\1/p')" "$note" >> "$ROWS"
    say "$name: ${w#*: }"
  }
  # set_knobs <K=V,...>: set them live and wait until they show.
  set_knobs() {
    local kv h0 s0 a0
    for kv in ${1//,/ }; do
      h0=$(count 'HDR: on ('); s0=$(count "RSX_SCALE: render targets at ${kv#*=}%"); a0=$(count '\[AA\] SMAA')
      con "setenv $kv" > /dev/null
      case "$kv" in
        RSX_HDR=0|RSX_HDR=) ;;
        RSX_HDR=*)
          if ! wait_line 'HDR: on (' "$h0" 30; then
            grep -aq "display has HDR off" "$LOG" && echo "the display has HDR off" || echo "no 'HDR: on' within 30 s"
            return 1
          fi ;;
        RSX_SCALE=*) wait_line "RSX_SCALE: render targets at ${kv#*=}%" "$s0" 30 || { echo "the targets did not change"; return 1; } ;;
        RSX_AA=2) wait_line '\[AA\] SMAA' "$a0" 30 || true ;;   # compiles once a run; quiet after
      esac
    done
    wait_flips "$NFL" 60 || { echo "the game stopped presenting"; return 1; }
  }
  clear_knobs() {
    local kv; for kv in ${1//,/ }; do con "setenv ${kv%%=*}=" > /dev/null; done
    wait_flips "$NFL" 60
  }
  menu=""
  # (The lists are read on fd 3, so nothing in the loop can take its lines.)
  while read -r VAR spec _ <&3; do
    [ -n "$VAR" ] || continue
    case "$VAR" in '#'*) continue ;; esac
    alive || { say "the game stopped"; bad=1; break; }
    case "$spec" in
      @menu) menu=$VAR; continue ;;
      @chain:*)
        n=${spec#@chain:}; case "$n" in ''|*[!0-9]*) skip "$VAR" "bad chain length"; continue ;; esac
        for k in $(seq 0 $((n - 1))); do
          take "${SPOT}_${VAR}_$k" "" "${SPOT}_$VAR" "$([ "$k" = 0 ] && echo shot || echo noshot)" || break
        done ;;
      -) take "${SPOT}_$VAR" "" "" shot ;;
      *=*)
        if why=$(set_knobs "$spec"); then take "${SPOT}_$VAR" "$spec" "" shot; else skip "$VAR" "$why"; fi
        clear_knobs "$spec" ;;
      *) skip "$VAR" "unknown variant '$spec'" ;;
    esac
  done 3<<< "$CAPSET_VARIANTS"
  if [ -n "$menu" ] && alive; then
    VAR=$menu
    echo "0x0008 6" > "$RUN/pad"   # START
    if wait_pad 10 && wait_flips "$NFL" 60; then
      take "${SPOT}_$menu" "" "" shot "START pressed: see the live picture"
    else
      skip "$menu" "the START press was not taken"
    fi
  fi
  ab_finish
  exit $bad
fi

. "$HERE/perflib.sh"
cd "$DES_ROOT" || exit 1
WANT=(); EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --) shift; EXTRA=("$@"); break ;;
    -h|--help) sed -n '2,/^set -uo/p' "$HERE/capset.sh" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "[capset] unknown option $1"; exit 2 ;;
    *) WANT+=("$1") ;;
  esac
  shift
done
EXE=${DES_EXE:-port/build/des_boot.exe}
[ -f "$EXE" ] || { echo "[capset] no exe at $EXE"; exit 2; }
PR=${PS3RECOMP_DIR:-$DES_ROOT/ps3recomp}
REPLAY=${RSX_REPLAY:-$PR/build/rsx_replay.exe}
H=$(perf_md5 "$EXE")
D=${CAPSET_DIR:-$DES_SCRATCH/capset/$H}; W=$D/work
[ -e "$D" ] && { echo "[capset] $D exists; set CAPSET_DIR to a new folder (nothing there is replaced)"; exit 2; }
# Spots to take, checked before anything starts.
SPOTS=$(printf '%s\n' "$CAPSET_SPOTS" | awk 'NF >= 3 && $1 !~ /^#/')
for s in ${WANT[@]+"${WANT[@]}"}; do
  printf '%s\n' "$SPOTS" | awk -v s="$s" '$1 == s {f = 1} END {exit !f}' || { echo "[capset] no spot '$s' in the list"; exit 2; }
done
perf_refuse_if_game
mkdir -p "$W"
exec > >(tee -a "$D/capset.log") 2>&1
echo "[capset] $(date '+%F %T') into $D"
perf_save_snapshot "$W" || exit 3
perf_present
: > "$W/captures.tsv"; : > "$W/skipped.tsv"
NV=$(printf '%s\n' "$CAPSET_VARIANTS" | awk 'NF >= 2 && $1 !~ /^#/' | wc -l)
SETTLE=${CAPSET_SETTLE:-40}; LCM=$(( 400 + SETTLE + NV * 90 )); [ "$LCM" -lt 900 ] && LCM=900
echo "exe $EXE  md5 $H  replay $REPLAY  RSX_PRESENT=$RSX_PRESENT  settle ${SETTLE}s  extra: ${EXTRA[*]:-none}"
kpid=; ec=0
stop_kid() { [ -n "$kpid" ] && kill -0 "$kpid" 2>/dev/null && { kill "$kpid" 2>/dev/null; wait "$kpid" 2>/dev/null; }; }
trap stop_kid EXIT
while read -r spot save map _ <&3; do
  if [ ${#WANT[@]} -gt 0 ] && ! printf '%s\n' "${WANT[@]}" | grep -qx "$spot"; then continue; fi
  if [ ! -d "$save" ]; then printf '%s\t%s\t%s\n' "$spot" "*" "no save at $save" >> "$W/skipped.tsv"; echo "[capset] $spot: no save at $save; skipped"; continue; fi
  perf_no_game 20 || { echo "[capset] a des_boot.exe is running; stopping before $spot"; ec=3; break; }
  echo "[capset] $(date +%T) $spot ($save, $map)"
  CAPSET_CHILD=$W CAPSET_OUT=$D DES_SCRATCH=$W DES_EXE=$EXE LC_SAVE=$save KAB_SETTLE=$SETTLE \
    LC_MAX=$LCM AB_IDLE=$LCM bash "$HERE/capset.sh" "$spot" "$map" PS3_SAIL_MOVIES=0 \
    ${EXTRA[@]+"${EXTRA[@]}"} > "$W/$spot.log" 2>&1 &
  kpid=$!
  wait "$kpid"; rc=$?; kpid=
  grep -a '^\[capset\]\|^\[ab\]' "$W/$spot.log" | sed 's/^/  /'
  [ "$rc" = 0 ] || { echo "[capset] $spot: the boot did not finish cleanly (exit $rc, $W/$spot.log)"; ec=1; }
  perf_save_check "$W" || { echo "[capset] stopping: the user's save changed"; ec=5; break; }
done 3<<< "$SPOTS"

check=skipped
first=$(awk -F '\t' 'NR == 1 {print $2}' "$W/captures.tsv")
if [ "${CAPSET_VERIFY:-1}" != 0 ] && [ -n "$first" ] && [ -f "$REPLAY" ] && [ $ec != 5 ]; then
  # One frame of one capture: the replay says so when its structs differ.
  ( des_env_defaults; des_window_launch; cd "$W" && timeout 600 "${DES_LAUNCH[@]}" "$REPLAY" "$(cygpath -m "$D/$first")" 1 ) > "$W/replay_check.txt" 2>&1
  rc=$?
  if grep -aq 'captured by another build' "$W/replay_check.txt"; then check="other build: $(grep -a 'captured by another build' "$W/replay_check.txt" | head -1)"
  elif [ $rc != 0 ]; then check="replay exit $rc ($(tail -1 "$W/replay_check.txt"))"
  else check=ok; fi
  echo "[capset] replay check: $check"
fi
PRV=$(git -C "$PR" describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)
PORTV=$(git -C "$DES_ROOT" describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)
RMD5=none; [ -f "$REPLAY" ] && RMD5=$(md5sum < "$REPLAY" | cut -d' ' -f1)
python - "$D" "$(cygpath -m "$(realpath "$EXE")")" "$(md5sum < "$EXE" | cut -d' ' -f1)" "$(cygpath -m "$REPLAY")" "$RMD5" \
    "$PRV" "$PORTV" "$RSX_PRESENT" "$check" <<'EOF'
import json, os, sys, time
d, exe, exe_md5, replay, replay_md5, prv, portv, present, check = sys.argv[1:]
cols = ('name', 'file', 'spot', 'map', 'variant', 'knobs', 'group', 'live', 'methods', 'mb', 'note')
caps = []
for l in open(os.path.join(d, 'work', 'captures.tsv'), encoding='utf-8', errors='replace'):
    v = l.rstrip('\r\n').split('\t')
    if len(v) < len(cols):
        continue
    c = dict(zip(cols, v))
    c['knobs'] = [k for k in c['knobs'].split(',') if k]
    for k in ('methods', 'mb'):
        c[k] = int(c[k]) if c[k].isdigit() else None
    caps.append(c)
skipped = []
for l in open(os.path.join(d, 'work', 'skipped.tsv'), encoding='utf-8', errors='replace'):
    v = l.rstrip('\r\n').split('\t')
    if len(v) >= 3:
        skipped.append({'spot': v[0], 'variant': v[1], 'why': v[2]})
j = {'version': 1, 'created': time.strftime('%Y-%m-%d %H:%M:%S'), 'exe': exe, 'exe_md5': exe_md5,
     'replay': replay, 'replay_md5': replay_md5, 'ps3recomp_rev': prv, 'port_rev': portv,
     'present': int(present) if present.isdigit() else present, 'replay_check': check,
     'captures': caps, 'skipped': skipped}
with open(os.path.join(d, 'capset.json'), 'w', encoding='utf-8') as f:
    json.dump(j, f, indent=1)
print('[capset] capset.json: %d captures, %d skipped' % (len(caps), len(skipped)))
EOF
[ "$RMD5" = none ] && echo "[capset] note: no rsx_replay at $REPLAY: build it (cmake --build build --target rsx_replay) from the same ps3recomp before halgate.sh"
echo "[capset] finished $(date '+%F %T'); $D"
[ $ec = 0 ] && [ "$check" != "ok" ] && [ "$check" != skipped ] && ec=1
exit $ec
