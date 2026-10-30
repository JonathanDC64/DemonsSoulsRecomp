#!/usr/bin/env bash
# halgate.sh [options] <capset dir>: the renderer gates on a capture set
# (capset.sh; docs/120 G2-G4). Replays every capture through rsx_replay once
# per route (RSX_BACKEND=<route>: d3d12 is today's renderer; hal-d3d12 and
# vulkan come with the portable path) and compares route B with route A.
#
#   -a <route>     route A (default d3d12)
#   -b <route>     route B (default hal-d3d12)
#   --repeat N     A/A instead: route A N times (N >= 2), each pass compared
#                  with the first: is today's replay deterministic?
#   --time         G4 instead: replay CPU time per frame on each route (below)
#   --frames F     frames per capture (default 4)
#   --chain        also replay each chain of the set (capset.sh @chain) and
#                  every capture without knobs, each as one chained run
#   --only <glob>  only the captures whose name matches
#   --img <mode>   halimg.py mode (default: exact, vk when B is vulkan)
#   --tol N        vk tolerance in 1/255 steps (default 1; 2 for D32S8)
#   --cold         start every pass from an empty shader cache, with the HLSL
#                  texts kept (PS3_SHADER_CACHE_HLSL=1), and compare the caches
#   --no-rtcap     no RSX_CAP render-target snapshots (they are large)
#   --out <dir>    results (default $DES_SCRATCH/halgate/<set>_<what>_<date>;
#                  a folder that exists is never reused)
#   --any-replay   use an rsx_replay other than the one the set names (a
#                  rebuild during a conversion step); the replay's own check
#                  of the capture's state sizes still applies
#
# Each replay (rsx_replay <capture> F, run from a staged copy of the exe)
# writes into <out>/<pass>/<capture>/: shots/ (CELLMARK_DUMP: every present;
# with RSX_HDR=1 among the knobs also shots/hdr.hdrp, RSX_HDR_SNAP), dp/
# (RSX_DRAWPRINT, 8 batches a frame), zpass.txt (the [ZPASS] lines,
# ZPASS_LOG), rsxcap/ (RSX_CAP=<F-1>:1: the per-op manifest and every
# render target of one frame), replay.txt. A capture's knobs (capset.json:
# RSX_SCALE, RSX_AA, RSX_HDR) are set for its replay. Each pass has its own
# shader cache, a copy of HALGATE_CACHE_SEED (default the game's,
# PS3_SHADER_CACHE).
#
# The comparison, per capture (all must hold):
#   images     shots/ through halimg.py (exact: 0 differing pixels; vk: the
#              G3 rule), heat maps in <out>/heat/
#   rt images  rsxcap/ images the same way
#   drawprint  the same batch files, the same lines (values of 12 or more hex
#              digits, pointers, are left out)
#   zpass      exact: the same values; vk: zero / non-zero the same, values
#              within 0.5%
#   manifest   rsxcap manifests line by line (pointers left out)
#   legacy     no non-VP draw records ("d<n> legacy" in DRAWPRINT): the new
#              path does not copy them
#   route      a route other than d3d12 must say it ran ("[rsx_out] ... route
#              <name>" or "renderer <name>" in the replay's output), so a
#              build without it, or a fallback, never passes as that route
# plus, for hal-d3d12 and A/A, one row for the shader caches: the .dxbc and
# .hlsl files each pass added, by name and bytes.
#
# Same build only: capset.json names the rsx_replay that matches the game
# build that took the captures (its md5); any other is refused unless
# --any-replay, and a replay that says "captured by another build" fails.
#
# --time (G4): the captures without knobs, on a staged exe with no
# diagnostics, after one warm-up replay per route. For HALGATE_TIME_RUNS
# rounds (default 5; A B, then B A, ...) each route plays each capture for
# HALGATE_TIME_FRAMES frames (default 200), and a frame's time is taken from
# the replay's own lines: the median of its per-frame times when it prints
# every frame (each replay gets RSX_REPLAY_TIMES=1 for a replay that can;
# today's prints frames 0-2 only), else the wall clock from the moment its
# "frame 2" line arrives to its exit, over the frames after it (no load or
# start-up in it; the exit is). A round's figure is the mean over the
# captures; the verdict is on the median of the rounds per route: hal-d3d12
# at most +0.5% over A, vulkan no slower, other routes reported.
# Like the A/B batches it waits for a quiet PC (perf_wait_quiet), refuses
# while any game runs, pins RSX_PRESENT from the lock state and RSX_VSYNC=0.
#
# The table goes to the terminal and <out>/verdict.txt. Exit status: 0 every
# check passed, 1 one failed, 2 bad arguments, 3 refused (another build's
# replay, or a game running for --time), 4 the PC stayed busy (--time).
#
# Captures, images and fingerprints are game-derived: keep <out> outside the
# repository.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/perflib.sh"
cd "$DES_ROOT" || exit 1
A=d3d12; B=hal-d3d12; REPEAT=0; TIME=0; F=4; CHAIN=0; ONLY='*'; IMG=; TOL=1; COLD=0; RTCAP=1; OUTD=; ANY=0
CS=
while [ $# -gt 0 ]; do
  case "$1" in
    -a) A=${2:?-a needs a route}; shift ;;
    -b) B=${2:?-b needs a route}; shift ;;
    --repeat) REPEAT=${2:?--repeat needs N}; shift ;;
    --time) TIME=1 ;;
    --frames) F=${2:?--frames needs F}; shift ;;
    --chain) CHAIN=1 ;;
    --only) ONLY=${2:?--only needs a glob}; shift ;;
    --img) IMG=${2:?--img needs exact or vk}; shift ;;
    --tol) TOL=${2:?--tol needs N}; shift ;;
    --cold) COLD=1 ;;
    --no-rtcap) RTCAP=0 ;;
    --out) OUTD=${2:?--out needs a folder}; shift ;;
    --any-replay) ANY=1 ;;
    -h|--help) sed -n '2,/^set -uo/p' "$HERE/halgate.sh" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "[gate] unknown option $1"; exit 2 ;;
    *) [ -z "$CS" ] || { echo "[gate] one capture set only"; exit 2; }; CS=$1 ;;
  esac
  shift
done
[ -n "$CS" ] || { echo "usage: halgate.sh [options] <capset dir>   (--help)"; exit 2; }
num() { case "$1" in ''|*[!0-9]*) return 1 ;; esac; }
num "$F" && [ "$F" -ge 1 ] || { echo "[gate] --frames takes a whole number >= 1"; exit 2; }
num "$TOL" || { echo "[gate] --tol takes a whole number"; exit 2; }
num "$REPEAT" || { echo "[gate] --repeat takes a whole number"; exit 2; }
[ "$REPEAT" = 0 ] || [ "$REPEAT" -ge 2 ] || { echo "[gate] --repeat takes N >= 2"; exit 2; }
[ "$REPEAT" = 0 ] || [ "$TIME" = 0 ] || { echo "[gate] --repeat or --time, not both"; exit 2; }
case "$IMG" in ''|exact|vk) ;; *) echo "[gate] --img is exact or vk"; exit 2 ;; esac
for r in "$A" "$B"; do case "$r" in d3d12|hal-d3d12|vulkan|metal|auto) ;; *) echo "[gate] unknown route $r"; exit 2 ;; esac; done
[ -d "$CS" ] || CS=$(dirname "$CS")
CS=$(cd "$CS" 2>/dev/null && cygpath -m "$(pwd)") || { echo "[gate] no folder $CS"; exit 2; }
JSON=$CS/capset.json
[ -f "$JSON" ] || { echo "[gate] no capset.json in $CS (capset.sh makes it)"; exit 2; }
REPLAY=${RSX_REPLAY:-${PS3RECOMP_DIR:-$DES_ROOT/ps3recomp}/build/rsx_replay.exe}
[ -f "$REPLAY" ] || { echo "[gate] no rsx_replay at $REPLAY (RSX_REPLAY)"; exit 2; }

# The captures: "<name>|<file>|<knobs, spaces>|<group>" ("|": a field can
# be empty, and bash's read joins runs of tabs).
CAPS=$(python - "$(cygpath -m "$JSON")" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1], encoding='utf-8'))
print('#replay_md5|%s' % j.get('replay_md5', 'none'))
for c in j.get('captures', []):
    print('|'.join((c['name'], c['file'], ' '.join(c.get('knobs') or []), c.get('group') or '')))
EOF
) || { echo "[gate] cannot read $JSON"; exit 2; }
WANT_MD5=$(printf '%s\n' "$CAPS" | awk -F '|' '$1 == "#replay_md5" {print $2}')
HAVE_MD5=$(md5sum < "$REPLAY" | cut -d' ' -f1)
if [ "$WANT_MD5" != "$HAVE_MD5" ] && [ "$ANY" = 1 ]; then
  echo "[gate] note: $REPLAY (md5 $HAVE_MD5) is not the rsx_replay these captures were taken with (${WANT_MD5:-none}); --any-replay"
elif [ "$WANT_MD5" != "$HAVE_MD5" ]; then
  echo "[gate] refused: $REPLAY (md5 $HAVE_MD5) is not the rsx_replay these captures were taken with (${WANT_MD5:-none})."
  echo "[gate] Captures are same-build only: take a new set with capset.sh on the build that matches this replay."
  exit 3
fi
CAPS=$(printf '%s\n' "$CAPS" | awk -F '|' '$1 !~ /^#/ && NF >= 2')
SEL=$(printf '%s\n' "$CAPS" | while IFS='|' read -r n f k g; do
        # shellcheck disable=SC2053
        [[ $n == $ONLY ]] && printf '%s|%s|%s|%s\n' "$n" "$f" "$k" "$g"; done)
[ -n "$SEL" ] || { echo "[gate] no captures match '$ONLY'"; exit 2; }
for f in $(printf '%s\n' "$SEL" | cut -d'|' -f2); do [ -f "$CS/$f" ] || { echo "[gate] $CS/$f is missing"; exit 2; }; done

if [ "$TIME" = 1 ]; then WHAT=time_${A}_vs_$B
elif [ "$REPEAT" -gt 0 ]; then WHAT=repeat${REPEAT}_$A
else WHAT=${A}_vs_$B; fi
OUT=${OUTD:-$DES_SCRATCH/halgate/$(basename "$CS")_${WHAT}_$(date +%Y%m%d_%H%M%S)}
[ -e "$OUT" ] && { echo "[gate] $OUT exists; pick another --out"; exit 2; }
mkdir -p "$OUT/bin" || exit 2
OUT=$(cygpath -m "$(cd "$OUT" && pwd)")   # absolute: the replays run from their own folders
exec > >(tee -a "$OUT/halgate.log") 2>&1
des_env_defaults
SEED=${HALGATE_CACHE_SEED:-$PS3_SHADER_CACHE}
# A staged copy: the build output can be rebuilt meanwhile, and the md5 checked
# above is the one that runs.
cp -f "$REPLAY" "$OUT/bin/rsx_replay.exe" || exit 2
for d in "$(dirname "$REPLAY")"/*.dll; do [ -f "$d" ] && cp -f "$d" "$OUT/bin/"; done
BIN=$OUT/bin/rsx_replay.exe
[ "$(md5sum < "$BIN" | cut -d' ' -f1)" = "$HAVE_MD5" ] || { echo "[gate] the staged rsx_replay differs from $REPLAY; stopping"; exit 2; }
TMO=${HALGATE_TIMEOUT:-900}
echo "[gate] $(date '+%F %T') $WHAT on $(basename "$CS"): $(printf '%s\n' "$SEL" | grep -c .) capture(s)$([ "$TIME" = 1 ] || echo ", $F frame(s) each")"
echo "[gate] replay md5 $HAVE_MD5  RSX_PRESENT=$RSX_PRESENT  cache seed $SEED  out $OUT"

# new_cache <dir>: a pass's shader cache (empty with --cold), and its listing.
new_cache() {
  mkdir -p "$1"
  if [ "$COLD" = 0 ] && [ -d "$SEED" ]; then cp -r "$SEED/." "$1/" || return 1; fi
  ( cd "$1" && find . -type f \( -name '*.dxbc' -o -name '*.hlsl' \) -exec md5sum {} + | LC_ALL=C sort -k2 ) > "$1.before"
}
cache_added() {   # cache_added <dir>: "<md5> <file>" the passes added or changed
  ( cd "$1" && find . -type f \( -name '*.dxbc' -o -name '*.hlsl' \) -exec md5sum {} + | LC_ALL=C sort -k2 ) |
    LC_ALL=C comm -13 "$1.before" -
}

# replay <pass dir> <route> <name> <knobs> <frames> <diag 0|1> <capture>...
replay() {
  local p=$1 route=$2 name=$3 knobs=$4 fr=$5 diag=$6 d E=() k c caps=() rc
  shift 6
  d=$p/$name; mkdir -p "$d"
  for c in "$@"; do caps+=("$(cygpath -m "$CS/$c")"); done
  E=(RSX_BACKEND="$route" PS3_SHADER_CACHE="$(cygpath -m "$p/cache")")
  if [ "$diag" = 1 ]; then
    mkdir -p "$d/shots" "$d/dp"
    # Every present of the run: a replayed frame can present more than once,
    # and a chain plays several captures.
    E+=(CELLMARK_DUMP=100000 CELLMARK_DUMP_DIR="$(cygpath -m "$d/shots")"
        RSX_DRAWPRINT="$(cygpath -m "$d/dp")/dp" RSX_DRAWPRINT_N=$((fr * 8)) ZPASS_LOG=100000000)
    [ "$RTCAP" = 1 ] && [ "$fr" -ge 2 ] && E+=(RSX_CAP="$((fr - 1)):1" RSX_CAP_DIR="$(cygpath -m "$d/rsxcap")")
    [ "$COLD" = 1 ] && E+=(PS3_SHADER_CACHE_HLSL=1)
    case " $knobs " in *" RSX_HDR=1 "*) E+=(RSX_HDR_SNAP="$(cygpath -m "$d/shots/hdr.hdrp")") ;; esac
  fi
  for k in $knobs; do E+=("$k"); done
  if [ "$diag" = 1 ]; then
    ( cd "$d" && env "${E[@]}" RSX_REPLAY_TIMES=1 timeout "$TMO" "$BIN" "${caps[@]}" "$fr" ) > "$d/replay.txt" 2>&1
    rc=$?
  else
    # Timing: each line as it arrives (the replay's stdout is unbuffered) with
    # the wall clock, in stamped.txt, and "end <time>" once it has exited.
    ( cd "$d" && env "${E[@]}" RSX_REPLAY_TIMES=1 timeout "$TMO" "$BIN" "${caps[@]}" "$fr" 2>&1 ) |
      { while IFS= read -r l; do printf '%s\n' "$l" >&5; printf '%s %s\n' "$EPOCHREALTIME" "$l" >&6; done
        printf 'end %s\n' "$EPOCHREALTIME" >&6; } 5> "$d/replay.txt" 6> "$d/stamped.txt"
    rc=${PIPESTATUS[0]}
  fi
  echo "$rc" > "$d/rc"
  grep -a '^\[ZPASS\] report' "$d/replay.txt" > "$d/zpass.txt"
  return $rc
}

# replay_problem <dir> <route>: why that replay cannot be used, or nothing.
replay_problem() {
  local d=$1 route=$2 rc
  rc=$(cat "$d/rc" 2>/dev/null)
  grep -aq 'captured by another build' "$d/replay.txt" && { echo "another build's capture"; return; }
  grep -aq 'backend init failed' "$d/replay.txt" && { echo "backend init failed"; return; }
  grep -aq 'rsx_replay: cannot read' "$d/replay.txt" && { echo "cannot read the capture"; return; }
  [ "$rc" = 0 ] || { [ "$rc" = 124 ] && echo "timed out" || echo "exit $rc"; return; }
  local said
  said=$(grep -aE '^\[rsx_out\]' "$d/replay.txt" | grep -aiE '(route|renderer)[ :=]+[a-z0-9-]+' |
         sed -E 's/.*(route|renderer)[ :=]+([A-Za-z0-9-]+).*/\2/I' | tail -1)
  if [ "$route" != d3d12 ] && [ "$said" != "$route" ]; then echo "route not confirmed (said: ${said:-nothing})"; return; fi
  [ "$route" = d3d12 ] && [ -n "$said" ] && [ "$said" != d3d12 ] && { echo "ran $said"; return; }
  return 0
}

norm() { sed -E 's/(0x)?[0-9A-Fa-f]{12,16}\b/<ptr>/g' "$@" | tr -d '\r'; }

# compare <dir A> <dir B> <name> <img mode> <heat dir>: one table row's fields
# (tab-separated: images, rt images, drawprint, zpass, manifest, legacy,
# verdict), details in <heat dir>.txt.
compare() {
  local a=$1 b=$2 n=$3 mode=$4 h=$5 det=$5.txt fail=0 img rti dp zp mf lg pa pb s rc
  mkdir -p "$(dirname "$det")"; : > "$det"
  pa=$(replay_problem "$a" "$ROUTE_A"); pb=$(replay_problem "$b" "$ROUTE_B")
  if [ -n "$pa$pb" ]; then
    printf 'replay\t-\t-\t-\t-\t-\tFAIL: %s\n' "${pa:+A: $pa}${pa:+${pb:+; }}${pb:+B: $pb}"; return 1
  fi
  # images
  if ! compgen -G "$a/shots/*.bmp" > /dev/null; then img="FAIL no frames"; fail=1
  else
    python "$(cygpath -m "$HERE/halimg.py")" "$(cygpath -m "$a/shots")" "$(cygpath -m "$b/shots")" \
      --mode "$mode" --tol "$TOL" --out "$(cygpath -m "$h/shots")" --quiet >> "$det" 2>&1; rc=$?
    s=$(grep -a '^halimg:' "$det" | tail -1 | sed -E 's/.*images=([0-9]+) failed=([0-9]+).*/\1 \2/')
    if [ $rc = 0 ]; then img="ok (${s% *})"; else img="FAIL ${s#* } of ${s% *}"; fail=1; fi
  fi
  # render-target snapshots
  if [ -d "$a/rsxcap" ] && [ -d "$b/rsxcap" ]; then
    echo "-- rsxcap" >> "$det"
    if [ -z "$(find "$a/rsxcap" "$b/rsxcap" -name '*.bmp' | head -1)" ]; then rti="- (none)"
    else
      python "$(cygpath -m "$HERE/halimg.py")" "$(cygpath -m "$a/rsxcap")" "$(cygpath -m "$b/rsxcap")" \
        --mode "$mode" --tol "$TOL" --out "$(cygpath -m "$h/rsxcap")" --quiet >> "$det" 2>&1; rc=$?
      s=$(grep -a '^halimg:' "$det" | tail -1 | sed -E 's/.*images=([0-9]+) failed=([0-9]+).*/\1 \2/')
      if [ $rc = 0 ]; then rti="ok (${s% *})"; else rti="FAIL ${s#* } of ${s% *}"; fail=1; fi
    fi
    local ma mb nd=0 m
    for m in $( (cd "$a/rsxcap" && find . -name manifest.txt; cd "$b/rsxcap" && find . -name manifest.txt) | sort -u); do
      ma=$a/rsxcap/$m; mb=$b/rsxcap/$m
      if [ ! -f "$ma" ] || [ ! -f "$mb" ]; then nd=$((nd + 1)); echo "-- manifest $m on one side only" >> "$det"; continue; fi
      s=$(diff <(norm "$ma") <(norm "$mb") | grep -c '^[<>]')
      [ "$s" = 0 ] || { nd=$((nd + s)); echo "-- manifest $m: $s line(s) differ" >> "$det"; diff <(norm "$ma") <(norm "$mb") | head -20 >> "$det"; }
    done
    if [ "$nd" = 0 ]; then mf=ok; else mf="FAIL $nd"; fail=1; fi
  elif [ -d "$a/rsxcap" ] || [ -d "$b/rsxcap" ]; then
    rti="FAIL one side"; mf="FAIL one side"; fail=1
  else rti=-; mf=-; fi
  # drawprint
  local la lb i fa fb nl=0 nf
  mapfile -t la < <(cd "$a/dp" 2>/dev/null && ls dp_*.txt 2>/dev/null | sort)
  mapfile -t lb < <(cd "$b/dp" 2>/dev/null && ls dp_*.txt 2>/dev/null | sort)
  nf=${#la[@]}
  if [ "$nf" = 0 ]; then dp="FAIL no files"; fail=1
  elif [ "${#la[@]}" != "${#lb[@]}" ]; then dp="FAIL ${#la[@]} vs ${#lb[@]} files"; fail=1
  else
    for i in "${!la[@]}"; do
      fa=$a/dp/${la[$i]}; fb=$b/dp/${lb[$i]}
      [ "${la[$i]}" = "${lb[$i]}" ] || { nl=$((nl + 1)); echo "-- drawprint ${la[$i]} vs ${lb[$i]} (draw counts)" >> "$det"; }
      s=$(diff <(norm "$fa") <(norm "$fb") | grep -c '^[<>]')
      [ "$s" = 0 ] || { nl=$((nl + s)); echo "-- drawprint ${la[$i]}: $s line(s) differ" >> "$det"; diff <(norm "$fa") <(norm "$fb") | head -10 >> "$det"; }
    done
    if [ "$nl" = 0 ]; then dp="ok ($nf files)"; else dp="FAIL $nl lines"; fail=1; fi
  fi
  # legacy records
  local leg
  leg=$(cat "$a"/dp/dp_*.txt "$b"/dp/dp_*.txt 2>/dev/null | grep -c '^d[0-9]* legacy ')
  if [ "$leg" = 0 ]; then lg=none; else lg="FAIL $leg"; fail=1; fi
  # zpass
  local za zb
  za=$(grep -c . "$a/zpass.txt"); zb=$(grep -c . "$b/zpass.txt")
  if [ "$mode" = exact ]; then
    if cmp -s "$a/zpass.txt" "$b/zpass.txt"; then zp="ok ($za)"; else zp="FAIL"; fail=1; diff "$a/zpass.txt" "$b/zpass.txt" | head -10 >> "$det"; fi
  else
    s=$(paste -d '|' "$a/zpass.txt" "$b/zpass.txt" | awk -F '|' -v na="$za" -v nb="$zb" '
      function val(l, f) { split(l, f, " "); return f[5] + 0 }
      function idx(l, f) { split(l, f, " "); return f[3] }
      { x = val($1); y = val($2)
        if (idx($1) != idx($2)) { bad++; next }
        if ((x == 0) != (y == 0)) { bad++; next }
        m = x > y ? x : y; if (m > 0 && (x > y ? x - y : y - x) > 0.005 * m) bad++ }
      END { if (na != nb) bad++; print bad + 0 }')
    if [ "$s" = 0 ]; then zp="ok ($za)"; else zp="FAIL $s"; fail=1; fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$img" "$rti" "$dp" "$zp" "$mf" "$lg" "$([ $fail = 0 ] && echo ok || echo FAIL)"
  return $fail
}

# The runs: one entry per capture, plus the chains with --chain.
# "<name>|<knobs>|<capture files>".
RUNS=$(printf '%s\n' "$SEL" | awk -F '|' '{printf "%s|%s|%s\n", $1, $3, $2}')
if [ "$CHAIN" = 1 ]; then
  for g in $(printf '%s\n' "$SEL" | awk -F '|' '$4 != "" {print $4}' | awk '!s[$0]++'); do
    RUNS+=$'\n'"chain:$g||"$(printf '%s\n' "$SEL" | awk -F '|' -v g="$g" '$4 == g {printf "%s ", $2}')
  done
  all=$(printf '%s\n' "$SEL" | awk -F '|' '$3 == "" && $4 == "" {printf "%s ", $2}')
  [ "$(echo $all | wc -w)" -ge 2 ] && RUNS+=$'\n'"chain:all||$all"
fi

# ---------------------------------------------------------------- --time
if [ "$TIME" = 1 ]; then
  perf_refuse_if_game
  export PERF_LOG=$OUT/quiet.txt
  perf_wait_quiet 5 || exit 4
  perf_present; perf_uncapped
  TR=${HALGATE_TIME_RUNS:-5}; TF=${HALGATE_TIME_FRAMES:-200}
  num "$TR" && num "$TF" && [ "$TR" -ge 1 ] && [ "$TF" -ge 10 ] ||
    { echo "[gate] HALGATE_TIME_RUNS (>= 1) and HALGATE_TIME_FRAMES (>= 10) are whole numbers"; exit 2; }
  TC=$(printf '%s\n' "$SEL" | awk -F '|' '$3 == "" {print $2}')
  NC=$(printf '%s\n' "$TC" | grep -c .)
  [ "$NC" -gt 0 ] || { echo "[gate] no capture without knobs to time"; exit 2; }
  echo "[gate] timing $NC capture(s), $TF frames each, $TR rounds, RSX_PRESENT=$RSX_PRESENT RSX_VSYNC=$RSX_VSYNC"
  for r in "$A" "$B"; do   # a warm shader cache and pipelines, and the captures in the file cache
    new_cache "$OUT/time/$r/cache" || exit 2
    replay "$OUT/time/$r" "$r" warmup "" 2 0 $TC
    p=$(replay_problem "$OUT/time/$r/warmup" "$r"); [ -z "$p" ] || { echo "[gate] $r: $p"; exit 1; }
  done
  # per_frame <dir>: "<ms a frame> <source>" for one capture's replay.
  per_frame() {
    local own
    own=$(grep -ao '^\[replay\] frame [0-9]*: [0-9.]* ms' "$1/replay.txt" | awk '$3 != "0:" {print $4}')
    if [ "$(printf '%s\n' "$own" | grep -c .)" -ge $(( (TF - 1) * 9 / 10 )) ]; then
      printf '%s\n' "$own" | sort -g | awk '{v[NR] = $1} END {printf "%.4f replay\n", (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2}'
    else
      awk -v f="$TF" '/ \[replay\] frame 2: / && !t2 {t2 = $1} $1 == "end" {e = $2}
        END {if (t2 && e) printf "%.4f wall\n", 1000 * (e - t2) / (f - 3); else print "- none"}' "$1/stamped.txt"
    fi
  }
  : > "$OUT/times.tsv"
  for i in $(seq 1 "$TR"); do
    if [ $((i % 2)) = 1 ]; then order="$A $B"; else order="$B $A"; fi
    for r in $order; do
      perf_wait_quiet 3 || { echo "[gate] the PC stayed busy; stopping"; exit 4; }
      perf_present_ok || { echo "[gate] the session locked under a presenting run; stopping"; exit 4; }
      sum=0; srcs=
      for c in $TC; do
        x=r${i}_${c%.rcap}
        replay "$OUT/time/$r" "$r" "$x" "" "$TF" 0 "$c"
        p=$(replay_problem "$OUT/time/$r/$x" "$r"); [ -z "$p" ] || { echo "[gate] $r $x: $p"; exit 1; }
        read -r pf src <<< "$(per_frame "$OUT/time/$r/$x")"
        [ "$pf" != - ] || { echo "[gate] $r $x: no frame 2 line to time from"; exit 1; }
        sum=$(awk -v a="$sum" -v b="$pf" 'BEGIN {print a + b}'); srcs="$srcs $src"
      done
      pf=$(awk -v s="$sum" -v n="$NC" 'BEGIN {printf "%.4f", s / n}')
      src=$(printf '%s\n' $srcs | sort -u | tr '\n' '+' | sed 's/+$//')
      printf '%s\t%s\t%s\t%s\t%s\n' "$r" "$i" "$pf" "$src" "${PERF_BG:-?}" >> "$OUT/times.tsv"
      echo "[gate] round $i $r: $pf ms a frame ($src; background cpu ${PERF_BG:-?}%)"
    done
  done
  awk -F '\t' -v a="$A" -v b="$B" '
    function med(r,   n, i, j, t, v) { n = 0; for (i = 1; i <= cnt[r]; i++) v[++n] = x[r, i]
      for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
      return (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2 }
    { cnt[$1]++; x[$1, cnt[$1]] = $3 + 0; src[$1] = $4 }
    END {
      ma = med(a); mb = med(b)
      if (ma <= 0 || mb <= 0) { printf "no valid time (%s %.4f, %s %.4f ms a frame): FAIL\n", a, ma, b, mb; exit 1 }
      d = 100 * (mb / ma - 1)
      lim = (b == "hal-d3d12") ? 0.5 : (b == "vulkan") ? 0.0 : ""
      printf "%-10s %6s %12s %s\n", "route", "runs", "ms a frame", "(median)"
      printf "%-10s %6d %12.4f %s\n", a, cnt[a], ma, src[a]
      printf "%-10s %6d %12.4f %s\n", b, cnt[b], mb, src[b]
      if (lim == "") printf "B against A: %+.2f%% (no limit for %s)\n", d, b
      else printf "B against A: %+.2f%%, limit %+.1f%%: %s\n", d, lim, (d <= lim ? "ok" : "FAIL")
      exit (lim != "" && d > lim) }' "$OUT/times.tsv" | tee "$OUT/verdict.txt"
  rc=${PIPESTATUS[0]}
  echo "[gate] finished $(date '+%F %T'); $OUT"
  exit "$rc"
fi

# ------------------------------------------------------ comparisons and A/A
if [ "$REPEAT" -gt 0 ]; then
  PASSES=(); for i in $(seq 1 "$REPEAT"); do PASSES+=("$A.$i"); done
  MODE=${IMG:-exact}
else
  PASSES=("A.$A" "B.$B")
  MODE=${IMG:-$([ "$B" = vulkan ] && echo vk || echo exact)}
fi
route_of() { case "$1" in A.*|B.*) echo "${1#?.}" ;; *) echo "${1%.*}" ;; esac; }
for p in "${PASSES[@]}"; do
  new_cache "$OUT/$p/cache" || { echo "[gate] cannot copy the shader cache seed"; exit 2; }
  r=$(route_of "$p")
  while IFS='|' read -r n k files <&3; do
    [ -n "$n" ] || continue
    replay "$OUT/$p" "$r" "${n//:/_}" "$k" "$F" 1 $files
    echo "[gate] $p ${n}: exit $(cat "$OUT/$p/${n//:/_}/rc"), $(ls "$OUT/$p/${n//:/_}/shots" 2>/dev/null | grep -c '\.bmp$') frame(s)"
  done 3<<< "$RUNS"
done

ec=0
{
  printf '%-30s %-16s %-14s %-14s %-16s %-12s %-10s %-8s %s\n' capture knobs images "rt images" drawprint zpass manifest legacy verdict
  for k in $(seq 2 ${#PASSES[@]}); do
    P1=${PASSES[0]}; PK=${PASSES[$((k - 1))]}
    ROUTE_A=$(route_of "$P1"); ROUTE_B=$(route_of "$PK")
    [ ${#PASSES[@]} -gt 2 ] && echo "== $P1 against $PK"
    while IFS='|' read -r n kn files <&3; do
      [ -n "$n" ] || continue
      c=${n//:/_}
      row=$(compare "$OUT/$P1/$c" "$OUT/$PK/$c" "$c" "$MODE" "$OUT/heat/${P1}_vs_$PK/$c") || ec=1
      IFS=$'\t' read -r i1 i2 i3 i4 i5 i6 i7 <<< "$row"
      printf '%-30s %-16s %-14s %-14s %-16s %-12s %-10s %-8s %s\n' "$n" "${kn:--}" "$i1" "$i2" "$i3" "$i4" "$i5" "$i6" "$i7"
    done 3<<< "$RUNS"
    if [ "$ROUTE_B" = vulkan ] || [ "$ROUTE_A" = vulkan ]; then
      printf '%-30s %s\n' "shader cache" "- (vulkan keeps its own)"
    else
      ca=$(cache_added "$OUT/$P1/cache"); cb=$(cache_added "$OUT/$PK/cache")
      na=$(printf '%s' "$ca" | grep -c .); nb=$(printf '%s' "$cb" | grep -c .)
      if [ "$ca" = "$cb" ]; then printf '%-30s %s\n' "shader cache" "ok ($na file(s) added, the same names and bytes)"
      else
        printf '%-30s %s\n' "shader cache" "FAIL: $na vs $nb file(s) added, $(diff <(printf '%s\n' "$ca") <(printf '%s\n' "$cb") | grep -c '^[<>]') differ"
        ec=1
      fi
    fi
  done
} > "$OUT/verdict.txt"
cat "$OUT/verdict.txt"
grep -q 'FAIL' "$OUT/verdict.txt" && ec=1
echo "[gate] $( [ $ec = 0 ] && echo PASS || echo FAIL ): $WHAT, mode $MODE$([ "$MODE" = vk ] && echo " tol $TOL"); details in $OUT/heat/*/*.txt"
echo "[gate] finished $(date '+%F %T'); $OUT"
exit $ec
