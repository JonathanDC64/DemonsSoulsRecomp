#!/usr/bin/env bash
# build.sh — build the runtime lib and/or relink des_boot via MSVC (vcvars + ninja).
# Whitelist as: Bash(tools/des/build.sh *)
#   build.sh rt      -> runtime lib only (ps3recomp_runtime)
#   build.sh boot    -> des_boot only
#   build.sh all     -> rt then boot (default)
set -uo pipefail
target="${1:-all}"
VCVARS="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
export PATH="/c/Program Files/LLVM/bin:/c/Program Files/CMake/bin:/c/Users/jonat/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe:$PATH"

build_one() {  # $1=build dir  $2=target
  # NOTE: `cmd //c "...&&..."` fails silently under Git bash (arg mangling), so drive
  # the vcvars+cmake build through powershell, which handles the nested quoting.
  powershell -NoProfile -Command "cmd /c '\"$VCVARS\" >nul 2>&1 && cmake --build \"$1\" --target $2'"
}
[ "$target" = rt ] || [ "$target" = all ] && build_one "C:/dev/projects/demons-souls-pc-port/ps3recomp/build" ps3recomp_runtime
[ "$target" = boot ] || [ "$target" = all ] && build_one "C:/dev/projects/demons-souls-pc-port/port/build" des_boot
echo "build.sh done (target=$target)"
