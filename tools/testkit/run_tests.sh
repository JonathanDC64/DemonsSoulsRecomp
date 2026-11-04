#!/usr/bin/env bash
# run_tests.sh: the tester kit of the Demon's Souls PC port (docs/128).
#
#   bash run_tests.sh <game folder>
#
# The game folder is the one the game starts from (default: two folders up, for
# a build beside this script). For each renderer the game plays a short
# route twice, first with an empty shader cache and then with the one the first
# run made. It loads your save, stays where the save puts you, turns the camera,
# saves five screenshots and records frame times, then ends by itself. It plays
# from a copy of your save (yours is never written), muted, offline, at the
# original 30 fps, with your keyboard, mouse and pads left out so that nothing
# you touch moves the route. A run takes about two minutes after the game has
# started.
#
# The results go into des_test_results_<date>.tar.gz beside this script. Your
# user name, computer name and home folder are taken out of every log first.
# Nothing is sent anywhere: send the file yourself.
#
# Environment knobs:
#   DES_KIT_RENDERERS  the renderers, in order (default: vulkan legacy on Linux,
#                      d3d12 vulkan on Windows)
#   DES_KIT_ROUTE      look (default) or regress (walks from the first level's spawn)
#   DES_KIT_EXE        another game program
#   DES_KIT_SAVE       another save folder (default: the game's own)
#   DES_KIT_MAX        seconds a run may take before it is stopped (default 900)
# bash 3.2 (macOS) and later.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
GAME=$(cd "${1:-$HERE/../..}" 2>/dev/null && pwd) || { echo "No folder ${1:-}"; exit 2; }
case "$(uname -s)" in
  Linux*)               OS=linux;   EXE=des_host;     DEF="vulkan legacy" ;;
  Darwin*)              OS=macos;   EXE=des_host;     DEF="auto" ;;
  MINGW*|MSYS*|CYGWIN*) OS=windows; EXE=des_host.exe; DEF="d3d12 vulkan" ;;
  *)                    OS=other;   EXE=des_host;     DEF="" ;;
esac
# A player's install names its program in setup/release-info.txt.
named=$(sed -n 's/^exe=//p' "$GAME/setup/release-info.txt" 2>/dev/null | tr -d '')
[ -n "$named" ] && EXE=$named
EXE=${DES_KIT_EXE:-$GAME/$EXE}
RENDERERS=${DES_KIT_RENDERERS:-$DEF}
ROUTE=${DES_KIT_ROUTE:-look}
MAX=${DES_KIT_MAX:-900}
SAVE=${DES_KIT_SAVE:-$GAME/gamedata/dev_hdd0/home/00000001/savedata}

[ -f "$EXE" ] || { echo "No game program at $EXE: run this from the game's folder, or pass that folder."; exit 2; }
[ -d "$GAME/gamedata" ] || { echo "No gamedata folder in $GAME: finish the game's setup first."; exit 2; }
[ -d "$SAVE" ] || { echo "No save at $SAVE: play until the game has saved once."; exit 2; }
[ -n "$RENDERERS" ] || { echo "No renderers for this system: set DES_KIT_RENDERERS."; exit 2; }

NAME=des_test_results_$(date +%Y%m%d_%H%M%S)
OUT=$HERE/$NAME
mkdir -p "$OUT" || { echo "Cannot write to $HERE"; exit 2; }

# A path as the game takes it (on Windows, C:/... rather than /c/...).
native() { if [ "$OS" = windows ]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

# One value of a summary.json.
val() { grep "\"$2\"" "$1" 2>/dev/null | head -1 | sed 's/.*: *//; s/[",]//g'; }

system_info() {
  echo "os: $OS"
  case $OS in
    linux)
      echo "system: $( (. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}") )"
      echo "kernel: $(uname -r)"
      echo "cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
      echo "ram_mb: $(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo 2>/dev/null)"
      echo "session: ${XDG_SESSION_TYPE:-unknown} ${XDG_CURRENT_DESKTOP:-}"
      if command -v vulkaninfo >/dev/null 2>&1; then
        vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|driverInfo|apiVersion' | sed 's/^ */vulkan: /'
      fi ;;
    macos)
      echo "system: $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null)"
      echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
      echo "ram_mb: $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 ))" ;;
    windows)
      echo "system: $(cmd //c ver 2>/dev/null | tr -d '\r' | grep -v '^$')"
      echo "cpu: $(reg query 'HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0' //v ProcessorNameString 2>/dev/null |
        tr -d '\r' | sed -n 's/.*REG_SZ *//p' | sed 's/ *$//')"
      echo "ram_mb: $(powershell -NoProfile -Command '[int64]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)' 2>/dev/null | tr -d '\r')" ;;
  esac
  echo "route: $ROUTE"
  echo "renderers: $RENDERERS"
}

# run <renderer> <cold|warm>: one run, its results in <renderer>_<cold|warm>/.
run() {
  local r=$1 kind=$2 dir=$OUT/$1_$2 save=$OUT/save_$1_$2 cache=$OUT/cache_$1 pid killer
  rm -rf "$save" && cp -R "$SAVE" "$save" || { echo "$r $kind: could not copy the save"; return 1; }
  [ "$kind" = cold ] && rm -rf "$cache"
  mkdir -p "$dir" "$cache"
  printf '%-7s %s shader cache: ' "$r" "$kind"
  ( cd "$GAME" && exec env DES_ROUTE="$ROUTE" DES_ROUTE_OUT="$(native "$dir")" DES_ROUTE_QUIT=1 \
      DES_SKIP_INTRO=1 DES_AUTO_CONTINUE=1 PS3_SAIL_MOVIES=0 AUDIO_MUTE=1 DES_FPS=0 RSX_BACKEND="$r" \
      PS3_NO_USER_INPUT=1 DES_ONLINE=0 \
      PS3_SAVEDATA_ROOT="$(native "$save")" PS3_SHADER_CACHE="$(native "$cache")" "$EXE" ) > "$dir/log.txt" 2>&1 &
  pid=$!
  ( sleep "$MAX"; kill "$pid" 2>/dev/null; sleep 5; kill -9 "$pid" 2>/dev/null ) > /dev/null 2>&1 &
  killer=$!
  wait "$pid" 2>/dev/null
  kill "$killer" 2>/dev/null; wait "$killer" 2>/dev/null
  rm -rf "$save"
  local s=$dir/summary.json
  if [ -f "$s" ]; then
    printf '%s, %s fps, 1%% low %s, %s hitches, worst frame %s ms\n' "$(val "$s" result)" "$(val "$s" fps)" \
      "$(val "$s" low_1pct_fps)" "$(val "$s" hitches)" "$(val "$s" worst_ms)" | tee -a "$OUT/results.txt"
  else
    echo "no result: the route did not finish (see ${r}_$kind/log.txt)" | tee -a "$OUT/results.txt"
  fi
}

system_info > "$OUT/system.txt"
echo "Testing $GAME"
echo "Results so far in $OUT"
for r in $RENDERERS; do
  for kind in cold warm; do
    printf '%s %s: ' "$r" "$kind" >> "$OUT/results.txt"
    run "$r" "$kind"
  done
  rm -rf "$OUT/cache_$r"   # (made from the game's files, and large: not packed)
done
# The renderer and GPU each run named.
for d in "$OUT"/*_cold "$OUT"/*_warm; do
  [ -f "$d/log.txt" ] || continue
  grep -a -E '^\[rsx_out\] route|adapter:|\[VK\].*device|vkd3d:' "$d/log.txt" | head -4 | sed "s|^|$(basename "$d"): |"
done >> "$OUT/system.txt"

# Names out of every text file: home folder, user, computer.
U=${USER:-${USERNAME:-}}
H=$(hostname 2>/dev/null || uname -n)
esc() { printf '%s' "$1" | sed 's/[][\.*^$|/]/\\&/g'; }
# Any case where sed can (GNU sed; not macOS's): Windows paths ignore case.
G=g; printf 'a\n' | sed 's/A/b/gI' > /dev/null 2>&1 && G=gI
scrub() {
  local f=$1 e=()
  [ -n "${HOME:-}" ] && e+=(-e "s|$(esc "$HOME")|~|$G")
  [ "$OS" = windows ] && [ -n "${HOME:-}" ] && e+=(-e "s|$(esc "$(cygpath -m "$HOME")")|~|$G" -e "s|$(esc "$(cygpath -w "$HOME")")|~|$G")
  # The longer name first: a computer name often holds the user's
  # (user-pc), and replacing that first would leave the rest of it.
  if [ ${#H} -ge ${#U} ]; then
    [ ${#H} -ge 3 ] && e+=(-e "s|$(esc "$H")|<computer>|$G")
    [ ${#U} -ge 3 ] && e+=(-e "s|$(esc "$U")|<user>|$G")
  else
    [ ${#U} -ge 3 ] && e+=(-e "s|$(esc "$U")|<user>|$G")
    [ ${#H} -ge 3 ] && e+=(-e "s|$(esc "$H")|<computer>|$G")
  fi
  [ ${#e[@]} -gt 0 ] || return 0
  sed "${e[@]}" "$f" > "$f.tmp" && mv "$f.tmp" "$f"
}
find "$OUT" -type f \( -name '*.txt' -o -name '*.json' \) | while IFS= read -r f; do scrub "$f"; done
# The check is in any case. A pattern, not grep -F: Git for Windows' grep 3.0
# aborts on -i -F, and a grep that fails must not read as "none left".
gesc() { printf '%s' "$1" | sed 's/[][\.*^$]/\\&/g'; }
left=""
for w in "$U" "$H"; do
  [ ${#w} -ge 3 ] || continue
  hits=$(grep -rIil -- "$(gesc "$w")" "$OUT" 2>/dev/null); rc=$?
  [ $rc -gt 1 ] && { echo "Not packed: the check for your names failed (grep exit $rc)."; exit 1; }
  [ -n "$hits" ] && left="$left$hits"$'\n'
done
if [ -n "$left" ]; then
  echo "Not packed: these files still have your user or computer name in them:"
  printf '%s' "$left" | sort -u
  echo "Edit them, or send the folder $OUT by hand after checking it."
  exit 1
fi

tar -czf "$HERE/$NAME.tar.gz" -C "$HERE" "$NAME" || { echo "Could not pack $OUT"; exit 1; }
echo
cat "$OUT/results.txt"
echo
echo "Done: $HERE/$NAME.tar.gz"
echo "Send that file to the developers. The folder $OUT holds the same files."
