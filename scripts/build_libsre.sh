#!/usr/bin/env bash
# build_libsre.sh [libsre.prx]: scripts/build_libsre.py under its old name. The
# steps, their outputs and the options are described there.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python scripts/build_libsre.py "$@"
