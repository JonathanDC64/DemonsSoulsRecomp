#!/usr/bin/env bash
# play.sh [minutes] [ENV=val ...]: play the game. Like desrun.sh, but with the
# whole machine: normal priority, every CPU, sound on. desrun.sh's defaults
# (below-normal priority on half the logical CPUs, muted) are for test runs
# that must not starve the machine; a play session got them too.
# Log: work/tmp/play.txt (default 60 minutes).
#   tools/des/play.sh            tools/des/play.sh 120 RSX_HOTKEY_FRAMES=4
MIN=60
case "${1:-}" in ""|*[!0-9]*) ;; *) MIN="$1"; shift ;; esac
export PS3_PRIORITY="${PS3_PRIORITY:-normal}" PS3_AFFINITY="${PS3_AFFINITY:-all}"
export AUDIO_MUTE="${AUDIO_MUTE:-0}"
exec "$(dirname "$0")/desrun.sh" $((MIN * 60)) play.txt "$@"
