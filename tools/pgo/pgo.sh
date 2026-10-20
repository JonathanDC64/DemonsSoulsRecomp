#!/usr/bin/env bash
# pgo.sh: profile-guided (PGO) builds of des_boot, with clang-cl's IR instrumentation. They live beside the normal
# build, in their own folders (port/build_pgo_gen and _use, ps3recomp/build-pgo-gen and -use), configured like
# port/build and ps3recomp/build, which stay as they are. A profile comes from running the game, so nothing this
# makes (work/pgo/) is committed.
#   pgo.sh gen [jobs]                     instrumented build: port/build_pgo_gen/des_boot.exe. 3 jobs by default:
#                                         an instrumented chunk compile holds about 3 GB.
#   pgo.sh play [minutes] [ENV=val ...]   play.sh on the instrumented build (it runs slower: it counts every
#                                         branch). The counts go to work/pgo/profraw while the game runs, so
#                                         quitting or a kill loses nothing. Play where the build should be fast;
#                                         5 minutes is plenty.
#   pgo.sh merge                          work/pgo/profraw/*.profraw -> work/pgo/des.profdata
#   pgo.sh use [profdata] [jobs]          optimized build: port/build_pgo_use/des_boot.exe, from work/pgo/des.profdata
#                                         by default; 4 jobs by default. It builds from a copy named by the profile's
#                                         contents (work/pgo/use/), so a new profile recompiles everything by itself.
#                                         Prints how many functions the profile no longer fits.
# Then play it: DES_EXE=port/build_pgo_use/des_boot.exe tools/des/play.sh
# A profile fits the sources and the lift it was made from. After a re-lift or a source change, run gen, play and
# merge again; a stale profile still builds (a function that changed just gets no profile data), only less fast.
# Needs what the normal build needs (configured port/build and ps3recomp/build, clang-cl, ninja, vcvars64.bat;
# VCVARS and NINJA_DIR come from tools/des/local.env) plus llvm-profdata, which LLVM installs beside clang-cl.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$ROOT/tools/des/local.env" ] && . "$ROOT/tools/des/local.env"
VCVARS="${VCVARS:-C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat}"
export PATH="/c/Program Files/LLVM/bin:/c/Program Files/CMake/bin${NINJA_DIR:+:$NINJA_DIR}:$PATH"
W=$(cygpath -m "$ROOT")
PGO=$W/work/pgo
say() { echo "[pgo] $*"; }

# configure_like <normal build dir> <src dir> <new build dir> <mode> <arg> [extra -D...]: a .bat that configures
# <new build dir> with the normal build's settings and pgocc.py as the compiler launcher, then runs it.
configure_like() {
  local from=$1 src=$2 dir=$3 mode=$4 arg=$5; shift 5
  local cache=$from/CMakeCache.txt py
  [ -f "$cache" ] || { say "no $cache: configure the normal build first"; return 2; }
  py=$(command -v python) || { say "no python on PATH"; return 2; }
  py=$(cygpath -w "$py")
  mkdir -p "$dir" || return 1
  {
    echo "@echo off"
    echo "call \"$VCVARS\" >nul 2>&1"
    printf 'cmake -S "%s" -B "%s" -G Ninja' "$src" "$dir"
    # The settings that make the normal build what it is; anything else keeps its default. PS3RECOMP_BUILD_DIR is
    # left out: a PGO build links its own runtime build (an extra argument below). UNINITIALIZED is what a -D without
    # a type leaves (-DCMAKE_C_COMPILER=clang-cl); without it the copy falls back to cl.exe.
    grep -aE '^(CMAKE_(C|CXX)_COMPILER|CMAKE_(LINKER|AR|MT|RC_COMPILER|MAKE_PROGRAM|BUILD_TYPE)|CMAKE_(C|CXX)_FLAGS|RECOMP_DIR|PS3RECOMP_DIR|DES_[A-Z0-9_]+|PS3RECOMP_[A-Z0-9_]+|FETCHCONTENT_SOURCE_DIR_[A-Z0-9_]+):(BOOL|STRING|PATH|FILEPATH|UNINITIALIZED)=' "$cache" \
      | grep -avE '^(FETCHCONTENT_SOURCE_DIR_[A-Z0-9_]*:PATH=$|PS3RECOMP_BUILD_DIR:)' | tr -d '\r' \
      | while IFS= read -r kv; do printf ' "-D%s"' "$kv"; done
    local x; for x in "$@"; do printf ' "%s"' "$x"; done
    printf ' "-DCMAKE_C_COMPILER_LAUNCHER=%s;%s;%s;%s"' "$py" "$W/tools/pgo/pgocc.py" "$mode" "$arg"
    printf ' "-DCMAKE_CXX_COMPILER_LAUNCHER=%s;%s;%s;%s"\r\n' "$py" "$W/tools/pgo/pgocc.py" "$mode" "$arg"
  } > "$dir/pgo_configure.bat"
  cmd //c "$(cygpath -w "$dir/pgo_configure.bat")" > "$dir/pgo_configure.log" 2>&1 \
    || { say "configure of $dir failed ($dir/pgo_configure.log):"; tail -15 "$dir/pgo_configure.log"; return 1; }
}

# build_in <dir> <target> <jobs> [_LINK_ value]
build_in() {
  local dir=$1 tgt=$2 jobs=$3 link=${4:-}
  {
    echo "@echo off"
    echo "call \"$VCVARS\" >nul 2>&1"
    if [ -n "$link" ]; then echo "set \"_LINK_=$link\""; else echo "set _LINK_="; fi
    printf 'cmake --build "%s" --target %s --parallel %s\r\n' "$dir" "$tgt" "$jobs"
  } > "$dir/pgo_build.bat"
  cmd //c "$(cygpath -w "$dir/pgo_build.bat")" > "$dir/pgo_build.log" 2>&1 \
    || { say "build in $dir failed ($dir/pgo_build.log):"; grep -a "error" "$dir/pgo_build.log" | grep -av "errors\? generated" | head -20; return 1; }
}

# build <gen|use> <arg> <jobs>: the runtime, then des_boot against it.
build() {
  local mode=$1 arg=$2 jobs=$3 link=""
  local rt=$W/ps3recomp/build-pgo-$mode pb=$W/port/build_pgo_$mode
  # The launcher's arguments are part of every compile command, so ninja recompiles everything when they change (a
  # new profile has a new path); configure runs again to set them.
  if [ ! -f "$rt/build.ninja" ] || [ "$(cat "$rt/pgo.arg" 2>/dev/null)" != "$arg" ]; then
    configure_like "$W/ps3recomp/build" "$W/ps3recomp" "$rt" "$mode" "$arg" || return 1
    echo "$arg" > "$rt/pgo.arg"
  fi
  say "runtime ($rt)"
  build_in "$rt" ps3recomp_runtime "$jobs" || return 1
  if [ ! -f "$pb/build.ninja" ] || [ "$(cat "$pb/pgo.arg" 2>/dev/null)" != "$arg" ]; then
    configure_like "$W/port/build" "$W/port" "$pb" "$mode" "$arg" "-DPS3RECOMP_BUILD_DIR=$rt" || return 1
    echo "$arg" > "$pb/pgo.arg"
  fi
  if [ "$mode" = gen ]; then
    # The instrumented objects pull in the profile runtime, built /MT: it asks for libcmt, and des_boot is /MD.
    local res; res=$(clang-cl --print-resource-dir 2>/dev/null | tr -d '\r')
    [ -f "$res/lib/windows/clang_rt.profile-x86_64.lib" ] || { say "no clang_rt.profile-x86_64.lib under $res"; return 2; }
    MSYS_NO_PATHCONV=1 clang-cl -nologo -c -O2 -Zl -Fo"$pb/getenv_thunk.obj" "$W/tools/pgo/getenv_thunk.c" \
      || { say "getenv_thunk.c did not compile"; return 1; }
    link="/LIBPATH:\"$(cygpath -w "$res/lib/windows")\" /NODEFAULTLIB:libcmt.lib $(cygpath -w "$pb/getenv_thunk.obj")"
  fi
  say "des_boot ($pb, $jobs jobs)"
  build_in "$pb" des_boot "$jobs" "$link" || return 1
  [ -f "$pb/des_boot.exe" ] || { say "no $pb/des_boot.exe"; return 1; }
  say "built $pb/des_boot.exe"
}

cmd=${1:-}; shift || true
case $cmd in
gen)
  mkdir -p "$PGO/profraw" || exit 1
  build gen "$PGO/profraw" "${1:-3}" || exit 1
  say "next: tools/pgo/pgo.sh play"
  ;;
play)
  X=$W/port/build_pgo_gen/des_boot.exe
  [ -f "$X" ] || { say "no $X: run tools/pgo/pgo.sh gen first"; exit 2; }
  mkdir -p "$PGO/profraw" || exit 1
  # %p: one file per run; %c: continuous mode, the counts are written while the game runs.
  DES_EXE=$X exec "$ROOT/tools/des/play.sh" "$@" "LLVM_PROFILE_FILE=$PGO/profraw/des-%p%c.profraw"
  ;;
merge)
  shopt -s nullglob; f=("$PGO"/profraw/*.profraw)
  [ ${#f[@]} -gt 0 ] || { say "no profiles in $PGO/profraw: run tools/pgo/pgo.sh play first"; exit 2; }
  llvm-profdata merge -o "$PGO/des.profdata" "${f[@]}" || exit 1
  say "merged ${#f[@]} run(s) into $PGO/des.profdata"
  say "next: tools/pgo/pgo.sh use"
  ;;
use)
  P=${1:-$PGO/des.profdata}
  [ -f "$P" ] || { say "no profile at $P: run tools/pgo/pgo.sh merge first"; exit 2; }
  # The build reads a copy named by its contents, so a new profile is a new path (see build).
  mkdir -p "$PGO/use" || exit 1
  U=$PGO/use/des-$(md5sum < "$P" | cut -c1-12).profdata
  [ -f "$U" ] || cp "$P" "$U" || exit 1
  build use "$U" "${2:-4}" || exit 1
  # clang warns once per function whose code no longer matches its profile data (-Wbackend-plugin). One that loses
  # no counts is left out: a runtime stub that shares its name with the port function that replaces it at link time
  # (runtime/spu/spu_tsp_weak.c) warns on every build.
  n=$(cat "$W/ps3recomp/build-pgo-use/pgo_build.log" "$W/port/build_pgo_use/pgo_build.log" | grep -a "hash mismatch" \
    | grep -avc "up to 0 count discarded")
  say "functions the profile no longer fits, in what this build compiled: $n (0 when the sources and the lift are"
  say "the ones it was made from)"
  say "play it: DES_EXE=port/build_pgo_use/des_boot.exe tools/des/play.sh"
  ;;
*)
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
  ;;
esac
