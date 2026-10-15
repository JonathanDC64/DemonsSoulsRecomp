#!/usr/bin/env bash
# precompile_shaders.sh: compile every shader program in the game's archives
# into the shader cache (PS3_SHADER_CACHE, default gamedata/shader_cache)
# before the game runs, so no draw waits on the shader compiler. Without it a
# new area stalled on dozens of compiles (~40 ms each, 100+ ms for the large
# vertex programs). play.sh runs it; it takes ~20 s the first time and a few
# seconds once the cache is full. Needs ps3recomp's rsx_shader_precompile
# target (built here if missing).
#
# The extracted programs are game data: they go to the scratch directory and
# are removed afterwards.
set -euo pipefail
. "$(dirname "$0")/deslib.sh"
des_env_defaults
EXE="$DES_ROOT/ps3recomp/build/rsx_shader_precompile.exe"
if [ ! -f "$EXE" ]; then
  echo "precompile_shaders: building rsx_shader_precompile"
  (cd "$DES_ROOT" && cmd //c "scripts\\vsenv.bat cmake --build ps3recomp/build --target rsx_shader_precompile") > /dev/null
fi
SRC="$PS3_VFS_ROOT/PS3_GAME/USRDIR/shader"
[ -d "$SRC" ] || { echo "precompile_shaders: no $SRC (set PS3_VFS_ROOT)"; exit 1; }
CG="$DES_SCRATCH/shader_cg"
rm -rf "$CG"
trap 'rm -rf "$CG"' EXIT
python -I "$DES_ROOT/tools/des/shader_extract.py" "$SRC" "$CG" > /dev/null
# Demon's Souls binds the same kind of texture to the same unit (seen in
# play, PS3_SHADER_CACHE_HLSL=1): the shadow map on 7, cube maps on 8 and 11
# (12-14 also hold cubes when bound); post effects read the depth buffer as
# colour on 1 or 2. Variants the game has used are rebuilt from the cache's
# fp_variants.txt too, so this list only has to cover a first run.
"$EXE" --unit 7=shadow,8=cube,11=cube,12=cube,13=cube,14=cube --alt 1=d2c,2=d2c \
  "$(cygpath -w "$CG")"
