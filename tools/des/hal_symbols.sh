#!/usr/bin/env bash
# hal_symbols.sh [--list] [--require] [--match <regex>] <lib|object|build dir>...:
# the portable renderer's export check (docs/120). Every external symbol that
# an object of ps3recomp's libs/video/rsxcore/, libs/video/gpu/ or
# libs/video/rsx_out.* defines has to start with rsxc_, gpu_ or rsx_out_, so
# the new path never collides with today's renderer (rsx_d3d12_*) or anything
# else in the runtime.
#
# Inputs: a static library (ps3recomp_runtime.lib or .a; its members are named
# by their object paths, CMakeFiles/<target>.dir/libs/video/gpu/...), object
# files, or a build directory (every .obj / .o under it). Objects are picked by
# their path; --match <regex> (extended, on the path with forward slashes)
# picks others instead, e.g. to try the check on today's renderer.
#
# Allowed besides the three prefixes, as the compiler or a header puts them
# in every object: MSVC string literals (??_C@), float and vector constants
# (__real@, __xmm@, __ymm@, __zmm@), the UCRT's inline stdio functions
# (printf, snprintf, sscanf, _vsnprintf_l ...: defined, COMDAT, wherever
# they are called), __local_stdio_*_options, _fltused, @feat.00, the
# knob header's ps3_env_gen, and C++ symbols of the standard library (std::,
# type_info, __cxxabiv). A C++ symbol of our own passes when its name or class
# starts with a prefix (?gpu_x@@..., ?f@gpu_vk@@..., _ZN6gpu_vk...).
# HAL_SYMBOLS_ALLOW=<regex> allows more.
#
# llvm-nm: $LLVM_NM, else llvm-nm on PATH, else CMAKE_NM from the
# CMakeCache.txt of the build the input is in; on Linux and macOS, nm.
#
#   --list      print every symbol checked, not only the ones that fail
#   --require   fail when no object matched (in CI, once the files exist)
# Exit status: 0 every symbol passes (or no object matched, without
# --require), 1 a symbol outside the prefixes (or nothing matched with
# --require), 2 bad arguments or no nm.
set -uo pipefail
LIST=0; REQ=0; MATCH='(^|/)libs/video/(rsxcore|gpu)/|(^|/)libs/video/rsx_out\.'
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --list) LIST=1 ;;
    --require) REQ=1 ;;
    --match) MATCH=${2:?--match needs a regex}; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "[sym] unknown option $1"; exit 2 ;;
    *) ARGS+=("$1") ;;
  esac
  shift
done
[ ${#ARGS[@]} -gt 0 ] || { echo "usage: hal_symbols.sh [--list] [--require] [--match <regex>] <lib|object|build dir>..."; exit 2; }
for a in "${ARGS[@]}"; do [ -e "$a" ] || { echo "[sym] no file $a"; exit 2; }; done

# The build an input belongs to: the nearest CMakeCache.txt above it.
cmake_nm() {
  local d="$1"; [ -d "$d" ] || d=$(dirname "$d")
  while [ -n "$d" ] && [ "$d" != / ] && [ "$d" != . ]; do
    if [ -f "$d/CMakeCache.txt" ]; then
      sed -n 's/^CMAKE_NM:[A-Z]*=//p' "$d/CMakeCache.txt" | tr -d '\r' | head -1; return
    fi
    [ "$(dirname "$d")" = "$d" ] && return
    d=$(dirname "$d")
  done
}
NM=${LLVM_NM:-}
[ -n "$NM" ] || NM=$(command -v llvm-nm 2>/dev/null || true)
if [ -z "$NM" ]; then
  for a in "${ARGS[@]}"; do c=$(cmake_nm "$a"); [ -n "$c" ] && [ -x "$c" ] && { NM=$c; break; }; done
fi
[ -n "$NM" ] || NM=$(command -v nm 2>/dev/null || true)
[ -n "$NM" ] || { echo "[sym] no llvm-nm (set LLVM_NM)"; exit 2; }

# "<object path>\t<type>\t<symbol>" for each defined external symbol of the
# objects in $1 that match, one object or library at a time.
syms() {
  local f="$1" norm
  norm=$(printf %s "$f" | tr '\\' /)
  # (Regexes go to awk through the environment: -v would read their
  # backslashes as escapes.)
  "$NM" --defined-only --extern-only "$f" 2>/dev/null | tr -d '\r' | HS_SRC=$norm HS_RE=$MATCH awk '
    function pick(p) { gsub(/\\/, "/", p); obj = p; keep = (obj ~ ENVIRON["HS_RE"]) }
    BEGIN { pick(ENVIRON["HS_SRC"]) }
    /^[0-9A-Fa-f]+ [A-Za-z?] / { if (keep) print obj "\t" $2 "\t" $3; next }
    /:$/ { m = substr($0, 1, length($0) - 1)
           # llvm-nm prints "<archive>(<member>):" or "<member>:"
           if (m ~ /\)$/ && index(m, "(")) { m = substr(m, index(m, "(") + 1); m = substr(m, 1, length(m) - 1) }
           pick(m) }'
}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for a in "${ARGS[@]}"; do
  if [ -d "$a" ]; then
    find "$a" -type f \( -name '*.obj' -o -name '*.o' \) | tr '\\' / | grep -E "$MATCH" | while IFS= read -r o; do syms "$o"; done
  elif [ -f "$a" ]; then
    syms "$a"
  else
    echo "[sym] no file $a" >&2; exit 2
  fi
done > "$TMP/all.tsv" || exit 2

OK_PREFIX='^_?(rsxc_|gpu_|rsx_out_)'
GEN='^_?(\?\?_C@|__real@|__xmm@|__ymm@|__zmm@|__local_stdio_(printf|scanf)_options$|_fltused$|@feat\.00$|ps3_env_gen$|_?v?(f|s|sn|w|fw|sw|snw)?(printf|scanf)(_s)?(_c)?(_p)?(_l)?$)'
CXX_STD='@std@@|@stdext@@|type_info@@|^_?_ZN?K?St|^_?_ZT[VIS]St|__cxxabiv|^_?_ZT[VIS]N10__cxxabiv'
CXX_OURS='(^|[?@$]|\?A[VU]|[0-9])(rsxc_|gpu_|rsx_out_)'
EXTRA=${HAL_SYMBOLS_ALLOW:-}
HS_OK=$OK_PREFIX HS_GEN=$GEN HS_STD=$CXX_STD HS_OURS=$CXX_OURS HS_EXTRA=$EXTRA \
awk -F '\t' -v list="$LIST" -v req="$REQ" -v out="$TMP/bad.tsv" '
  BEGIN { ok = ENVIRON["HS_OK"]; gen = ENVIRON["HS_GEN"]; std = ENVIRON["HS_STD"]
          ours = ENVIRON["HS_OURS"]; extra = ENVIRON["HS_EXTRA"] }
  { objs[$1] = 1; n++; s = $3
    if (s ~ ok) why = "ok"
    else if (s ~ gen) why = "compiler"
    else if (s ~ /^_?(\?|_Z)/ && (s ~ std || s ~ ours)) why = "c++"
    else if (extra != "" && s ~ extra) why = "allowed"
    else { why = "BAD"; bad++; print $1 "\t" $2 "\t" s > out }
    if (list) printf "%-8s %s %-40s %s\n", why, $2, s, $1 }
  END {
    no = 0; for (o in objs) no++
    if (no == 0) { print "hal_symbols: no objects matched" (req ? " (--require)" : ""); exit req ? 1 : 0 }
    printf "hal_symbols: %d objects, %d defined external symbols, %d outside rsxc_/gpu_/rsx_out_\n", no, n, bad + 0
    exit bad ? 1 : 0 }' "$TMP/all.tsv"
rc=$?
if [ -s "$TMP/bad.tsv" ]; then
  echo "outside the prefixes (object, type, symbol):"
  sort "$TMP/bad.tsv" | awk -F '\t' '{printf "  %s  %s %s\n", $1, $2, $3}' | head -200
fi
exit $rc
