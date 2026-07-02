#!/usr/bin/env bash
# dump.sh <hexaddr> [hexaddr2 ...] -- decompile the function(s) containing each
# address from the already-analyzed DeS Ghidra project, printing the C to stdout.
# Requires the project at work/ghidra/DeS (built by the import+analyze step).
#
#   tools/ghidra/dump.sh 0x00C278C8
#
# Uses -process (reuses analysis) + -noanalysis + dump_one.java (writes
# ghidra_one.txt in the cwd), then cats it.
set -u
G='/c/dev/tools/ghidra_12.1.2_PUBLIC/support/analyzeHeadless.bat'
PROJ='C:\dev\projects\demons-souls-pc-port\work\ghidra'
SCRIPTS='C:\dev\projects\demons-souls-pc-port\tools\ghidra'
cd "$(dirname "$0")"
for addr in "$@"; do
  rm -f ghidra_one.txt
  GHIDRA_MAXMEM=10G "$G" "$PROJ" DeS -process EBOOT.ELF -noanalysis \
    -scriptPath "$SCRIPTS" -postScript dump_one.java "$addr" >/dev/null 2>&1
  echo "########## $addr ##########"
  cat ghidra_one.txt 2>/dev/null || echo "(no output for $addr)"
  echo
done
