#!/usr/bin/env bash
# appimage.sh -- pack your own Linux build of des_boot into an AppImage (docs/76).
#
# The AppImage holds code lifted from your copy of the game, the decrypted EBOOT and libsre. It is for your own
# use, for example to copy a build to a Steam Deck. Never share it, as you would never share des_boot itself.
#
#   tools/linux/appimage.sh [--appdir-only] <port build dir> [out.AppImage]
#
# What goes in (an AppDir, then packed with appimagetool if it is on PATH or in $APPIMAGETOOL):
#   usr/bin/des_boot                    the build
#   usr/lib/libvkd3d*.so.1              vkd3d's three libraries, as the build loads them (LGPL-2.1: shared, unchanged,
#                                       with usr/share/doc/vkd3d/NOTICE; replace them to use another vkd3d)
#   usr/share/des/EBOOT.ELF             work/game/EBOOT.ELF
#   usr/share/des/libsre/libsre_image.bin   work/libsre/libsre_image.bin
#   AppRun                              the launcher (below): the environment play.sh sets, and a writable folder
# SDL2, the Vulkan loader and driver, and FFmpeg come from the system. FFmpeg is loaded for the movies when one
# starts and must be the major version the runtime was built against; without it a movie ends at once.
set -euo pipefail

usage() { sed -n 4,9p "$0" | sed 's/^# \{0,1\}//'; exit 2; }
APPDIR_ONLY=0
[ "${1:-}" = --appdir-only ] && { APPDIR_ONLY=1; shift; }
BUILD=${1:-}; OUT=${2:-DemonsSoulsRecomp-x86_64.AppImage}
[ -n "$BUILD" ] || usage
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$BUILD/des_boot"
[ -x "$EXE" ] || { echo "appimage: no des_boot in $BUILD"; exit 1; }
EBOOT="$ROOT/work/game/EBOOT.ELF"; LIBSRE="$ROOT/work/libsre/libsre_image.bin"
for f in "$EBOOT" "$LIBSRE"; do [ -f "$f" ] || { echo "appimage: $f is missing (README, Building)"; exit 1; }; done
if ! python3 "$ROOT/tools/ci/check_release_no_dev.py" "$EXE" > /dev/null; then
  echo "appimage: $EXE is a developer build (DES_DEV=ON, the cheats are in it); build without it"; exit 1
fi

APPDIR="${OUT%.AppImage}.AppDir"
rm -rf "${APPDIR:?}"; mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/des/libsre" "$APPDIR/usr/share/doc/vkd3d"
cp "$EXE" "$APPDIR/usr/bin/des_boot"
cp "$EBOOT" "$APPDIR/usr/share/des/EBOOT.ELF"
cp "$LIBSRE" "$APPDIR/usr/share/des/libsre/libsre_image.bin"

# vkd3d: the libraries the build resolves (from PKG_CONFIG_PATH's prefix at build time, or LD_LIBRARY_PATH now).
n=0
while read -r name path; do
  cp -L "$path" "$APPDIR/usr/lib/$name"; n=$((n + 1))
done < <(ldd "$EXE" | awk '$1 ~ /^libvkd3d/ && $3 ~ /^\// { print $1, $3 }')
if [ "$n" -lt 3 ]; then
  echo "appimage: found $n of vkd3d's 3 libraries in 'ldd $EXE' (set LD_LIBRARY_PATH to vkd3d's lib folder)"; exit 1
fi
# libvkd3d-utils pulls in libvkd3d-shader without des_boot naming it.
for lib in "$APPDIR"/usr/lib/libvkd3d*.so.1; do
  ldd "$lib" | awk '$1 ~ /^libvkd3d/ && $3 ~ /^\// { print $1, $3 }' | while read -r name path; do
    [ -e "$APPDIR/usr/lib/$name" ] || cp -L "$path" "$APPDIR/usr/lib/$name"
  done
done
VKD3D_VERSION=$(pkg-config --modversion libvkd3d 2>/dev/null || echo "unknown")
cat > "$APPDIR/usr/share/doc/vkd3d/NOTICE" <<EOF
usr/lib/libvkd3d*.so.1 are vkd3d $VKD3D_VERSION (https://gitlab.winehq.org/wine/vkd3d), Wine's Direct3D 12 on
Vulkan, licensed under the GNU Lesser General Public License version 2.1 or later
(https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html). They are unchanged builds of the release source,
https://dl.winehq.org/vkd3d/source/vkd3d-$VKD3D_VERSION.tar.xz, and des_boot loads them as shared libraries:
put other builds of the same libraries in their place to use them instead.
EOF

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# Starts des_boot from the AppImage. Everything the game writes (saves, the HDD cache, the shader cache, settings,
# crash reports) goes to one folder, $DES_DATA_DIR (default ~/.local/share/DemonsSoulsRecomp/DemonsSouls, where
# the settings already live). The game itself: $PS3_VFS_ROOT, the folder holding PS3_GAME, or a folder (or a
# symbolic link to one) called "game" in that data folder.
HERE=$(dirname "$(readlink -f "$0")")
DATA=${DES_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/DemonsSoulsRecomp/DemonsSouls}
mkdir -p "$DATA/work" "$DATA/gamedata" || exit 1
GAME=${PS3_VFS_ROOT:-$DATA/game}
if [ ! -d "$GAME/PS3_GAME" ]; then
  echo "DemonsSoulsRecomp: no PS3_GAME folder in $GAME." >&2
  echo "Put the game there (or a link to it: ln -s <folder with PS3_GAME> \"$DATA/game\"), or start with PS3_VFS_ROOT=<folder>." >&2
  exit 1
fi
# libsre is read from work/libsre in the working folder; the mount point changes at every start.
ln -sfn "$HERE/usr/share/des/libsre" "$DATA/work/libsre"
export PS3_VFS_ROOT="$GAME"
export DES_DATA_DIR="$DATA"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# As tools/des/play.sh (deslib.sh's des_env_defaults): the known-good libsre setup and the vertex buffer size.
export LIBSRE_LLE="${LIBSRE_LLE:-1}"
export LIBSRE_LLE_ONLY="${LIBSRE_LLE_ONLY:-011EE38B,1656D49F,8A85674D,73E06F91,082BFB09,E5443BE7,039D70B7,91066667}"
export SPURS_NO_ROUND_NOTIFY="${SPURS_NO_ROUND_NOTIFY:-1}"
export PS3_VERBOSE="${PS3_VERBOSE:-0}"
export VP_VB_VERTS="${VP_VB_VERTS:-1572864}"
export PS3_HDD1_ROOT="${PS3_HDD1_ROOT:-$DATA/gamedata/dev_hdd1}"
export PS3_SHADER_CACHE="${PS3_SHADER_CACHE:-$DATA/gamedata/shader_cache}"
mkdir -p "$PS3_HDD1_ROOT" "$PS3_SHADER_CACHE"
cd "$DATA" || exit 1   # saves go to gamedata/dev_hdd0 here, as beside the exe on Windows
exec "$HERE/usr/bin/des_boot" "$HERE/usr/share/des/EBOOT.ELF" "$@"
EOF
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/bin/des_boot"

# The desktop entry and icon appimagetool asks for. The icon is the game's own ICON0.PNG when the disc folder is
# known (as the Windows build's exe icon is), else a plain one.
cat > "$APPDIR/demonssoulsrecomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Demon's Souls (recompiled)
Exec=des_boot
Icon=demonssoulsrecomp
Categories=Game;
Terminal=false
EOF
ICON0="${PS3_VFS_ROOT:-}/PS3_GAME/ICON0.PNG"
if [ -n "${PS3_VFS_ROOT:-}" ] && [ -f "$ICON0" ]; then
  cp "$ICON0" "$APPDIR/demonssoulsrecomp.png"
else
  python3 - "$APPDIR/demonssoulsrecomp.png" <<'EOF'
import struct, sys, zlib
w = h = 256
def px(x, y): return (96, 88, 72) if 64 <= x < 192 and 64 <= y < 192 else (20, 18, 16)
rows = b"".join(b"\0" + b"".join(bytes(px(x, y)) for x in range(w)) for y in range(h))
def chunk(k, b): return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
EOF
fi
ln -sfn demonssoulsrecomp.png "$APPDIR/.DirIcon"

echo "appimage: $APPDIR ($(du -sh "$APPDIR" | cut -f1)); run it as is with $APPDIR/AppRun"
[ "$APPDIR_ONLY" = 1 ] && exit 0
TOOL=${APPIMAGETOOL:-$(command -v appimagetool || true)}
if [ -z "$TOOL" ]; then
  echo "appimage: no appimagetool (on PATH or in \$APPIMAGETOOL): the AppDir is ready, but not packed"; exit 1
fi
ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$OUT"
echo "appimage: $OUT -- yours only: it holds code from your copy of the game, never share it"
