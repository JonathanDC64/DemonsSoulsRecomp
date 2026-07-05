#!/usr/bin/env bash
# milestones.sh -- boot des_boot and report how far it progressed against an ORDERED
# list of known boot milestones. A regression safety net: run before/after any change
# to see if the boot advanced, held, or regressed -- and to bisect.
# Whitelist as: Bash(tools/des/milestones.sh *)
#
# Usage: milestones.sh [timeout_sec]   (default 45)
#   env passthrough: any VAR=val after the timeout is exported to des_boot.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
TIMEOUT="${1:-45}"; shift || true
SCRATCH="/c/Users/jonat/AppData/Local/Temp/claude/c--dev-projects-demons-souls-pc-port/e916f056-9f0d-434a-81ed-1ea83209d4b0/scratchpad"
OUT="$SCRATCH/milestones_run.txt"
export PS3_VFS_ROOT="D:/Users/jonat/Documents/roms/ps3/Demon's Souls (USA)"
for kv in "$@"; do export "$kv"; done
timeout "$TIMEOUT" ./port/build/des_boot.exe work/game/EBOOT.ELF > "$OUT" 2>&1
rc=$?

# Ordered milestones: "label<TAB>grep-ERE". Each must appear in the log to count as reached.
# Patterns match our RUNTIME log lines (prefixed), NOT import/registration lines.
MS=(
  "D3D12 backend ready|D3D12\] Backend ready"
  "SPURS init|cellSpurs\] InitializeWithAttribute"
  "cellUserInfo stat|cellUserInfo\] GetStat"
  "HighGraphics thread|sys_ppu_thread_create name=\"HighGraphics\""
  "Core.Res subsystem|sys_ppu_thread_create name=\"Core.Res.Decompressor\""
  "Core.Res.FileLoader|sys_ppu_thread_create name=\"Core.Res.FileLoader\""
  "Havok PPU Thread|sys_ppu_thread_create name=\"Havok PPU Thread\""
  "MO audio|sys_ppu_thread_create name=\"MOMainThread\""
  "NexusRevolution Main|sys_ppu_thread_create name=\"NexusRevolution Main\""
  "DsSaveLoadMan|sys_ppu_thread_create name=\"DsSaveLoadMan"
  "SLSession thread|sys_ppu_thread_create name=\"SLSession\""
  "cellGameDataCheckCreate2 CALL|cellGame\] DataCheckCreate2\(version"
  "init worker threads exit (game-data path done)|\[SYS\] sys_ppu_thread_exit\(tid=1[0-9] "
  "cellSaveDataAutoLoad2 (DIALOG)|cellSaveData\].*AutoLoad|cellSaveDataAutoLoad2\("
)

echo "=== des_boot milestones (exit=$rc, $(wc -l < "$OUT") lines) ==="
# reached = highest index whose pattern matched (monotonic: a missed intermediate
# grep due to SPU-race noise no longer understates progress -- docs/18 domain D R3).
reached=0
for i in "${!MS[@]}"; do
  pat="${MS[$i]#*|}"
  if grep -aqE "$pat" "$OUT"; then reached=$((i+1)); fi
done
for i in "${!MS[@]}"; do
  label="${MS[$i]%%|*}"; pat="${MS[$i]#*|}"
  if grep -aqE "$pat" "$OUT"; then printf "  [x] %2d. %s\n" "$((i+1))" "$label"
  else printf "  [ ] %2d. %s\n" "$((i+1))" "$label"; fi
done
# next gate = first unmet milestone AFTER the highest reached (not the first noisy gap)
frontier="<all met!>"
if [ "$reached" -lt "${#MS[@]}" ]; then frontier="${MS[$reached]%%|*}"; fi
crash=$(grep -acE '\[CRASH\]|panic|Runtime error' "$OUT")
echo "--- reached $reached/${#MS[@]} milestones (monotonic); next gate = ${frontier}; crash-markers=$crash"
echo "    NOTE: boot is nondeterministic -- use tools/des/repro.sh N for a reliable distribution."
[ "$crash" -gt 0 ] && echo "    WARNING: crash/panic markers present (may be the non-fatal SPU race)"
# Thread liveness summary -- CRUCIAL: thrdiag 'running/waiting' is STALE for EXITED
# threads, so cross-check here before ever calling a thread "hung".
created=$(grep -acE 'host thread started' "$OUT")
exited=$(grep -acE '\[SYS\] sys_ppu_thread_exit|entry RETURNED' "$OUT")
echo "--- threads: created=$created  exited(exit/returned)~=$exited  (a thread that RETURNED is NOT hung, whatever thrdiag says)"
