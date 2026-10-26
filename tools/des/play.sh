#!/usr/bin/env bash
# play.sh [minutes] [ENV=val ...]: play the game. Like desrun.sh, but with the
# whole machine: normal priority, every CPU, sound on. desrun.sh's defaults
# (below-normal priority on half the logical CPUs, muted) are for test runs
# that must not starve the machine; a play session got them too. The game
# fills its shader cache itself at startup, behind a progress bar
# (port/des_prework.cpp); precompile_shaders.sh does the same from a shell.
# Log: work/tmp/play.txt (default 60 minutes). F11 starts and stops a profile
# session, written to work/tmp/prof/prof_NNN_*.txt.
#   tools/des/play.sh            tools/des/play.sh 120 RSX_HOTKEY_FRAMES=4
#
# The session holds the save folder (des_save_lock in deslib.sh), so the game
# never starts while a test run has another save swapped in (a build from
# before PS3_SAVEDATA_ROOT; regress.sh, levelcap.sh), and no such run swaps it
# during the session. Like desrun.sh, it first ends an earlier play session
# and any test game, so their scripts let the folder go.
MIN=60
case "${1:-}" in ""|*[!0-9]*) ;; *) MIN="$1"; shift ;; esac
export DES_KIND=play   # own staging dir and pid file (deslib.sh)
export PS3_PRIORITY="${PS3_PRIORITY:-normal}" PS3_AFFINITY="${PS3_AFFINITY:-all}"
export AUDIO_MUTE="${AUDIO_MUTE:-0}"
. "$(dirname "$0")/deslib.sh"
des_kill_own
SAVE="$DES_ROOT/gamedata/dev_hdd0/home/00000001/savedata"
des_save_lock "$SAVE" || { echo "[play] the save folder is not free (the [save] lines say why); not starting"; exit 1; }
trap 'des_save_unlock "$SAVE"' EXIT
"$(dirname "$0")/desrun.sh" $((MIN * 60)) play.txt "$@"
