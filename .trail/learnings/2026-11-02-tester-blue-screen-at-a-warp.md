---
title: Tester blue screen at a warp and a save that would not load
author: maintainer
date: 2026-11-02
branch: pin/save-durable
touches:
  - ps3recomp.pin
---

## Decided
- A tester's PC hung at a warp (black screen ~2 min, only the pad still worked), then blue-screened, and the save no longer loaded. Windows' record: Kernel-Power 41 with BugcheckCode 239 = 0xEF CRITICAL_PROCESS_DIED and no nvlddmkm / 4101 events, so not a GPU TDR (unlike #205). Leads: memory running out (RAM + page file), page-file disk errors.
- Our save writes went to a temp file and were renamed over the old one but never flushed, so a crash could keep the rename and lose the data. ps3recomp #221 (pinned here) syncs each file (_commit / fsync) before a write-through rename; PARAM.SFO the same.

## Tried and ruled out
- Reading the hang from des.log: after a blue screen the log just stops (here mid-play, 250 NUL bytes at the end), so whatever happened after the cache's last flush is gone. Ask for Windows' events instead.
- Our own memory as the cause, at that tester's settings on a 32 GB PC: ~2.5 GB private plus ~1.8 GB GPU memory, flat in a level. Their RAM and page file are still to come.

## Watch out
- All-zero slot files are normal in Demon's Souls saves (the pinned regress save has several), so zeros alone don't prove a save was damaged.
- To get a tester's crash record, have them run (PowerShell, any language): Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddDays(-2)} | Where-Object { $_.Id -in 41,1001,4101 -or $_.ProviderName -match 'nvlddmkm' } | Format-List TimeCreated,Id,ProviderName,Message,@{n='Data';e={($_.Properties | ForEach-Object { $_.Value }) -join ' | '}} | Out-File "$env:USERPROFILE\Desktop\crash_events.txt". The file is UTF-16; the Data line of event 41 starts with the bugcheck code (decimal).
