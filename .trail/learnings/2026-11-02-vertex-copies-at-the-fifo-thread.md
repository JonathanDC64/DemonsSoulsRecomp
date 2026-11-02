---
title: "Vertex copies at the FIFO thread: copy once a stretch, and which syncs end a stretch"
author: maintainer
date: 2026-11-02
branch: master
touches:
  - ps3recomp/libs/video/rsx_d3d12_backend.c
  - ps3recomp/libs/video/cellGcmSys.c
  - ps3recomp/libs/video/rsx_commands.c
  - ps3recomp/libs/video/rsxcore/rsxc_vertex.inc
  - ps3recomp/libs/video/tests/test_rsx_synth.c
---

## Decided
- The GPU fetch path's span copies are safe to reuse within a "stretch": the stream between two g_rsx_sync_hook calls (labels, semaphores, reports, the reference, drain-pass end, 0x1D70/0x1D74/0x1800) and the frame ring reset. Inside one, the title cannot know the RSX has read a range, so it cannot have rewritten it. ps3recomp #215 keeps a table of the stretch's copies (by guest start, plus by 64 KB page for spans that start inside a copy) and points later descriptors at them.
- A 2D-engine inline write (NV308A COLOR words, used by Demon's Souls to patch fragment program constants, ~9,200 a frame on the 1-2 bridge) is no fence for vertex reads, so it ends a stretch only when it lands on a page a copy of this stretch read, or while a CPU-path vertex job is queued (g_rsx_sync_write_hook, VF_SYNC_2D).
- Measure each step inside one exe with a live knob (knobab.sh); the bridge varies between sessions, so only the ratio inside a batch counts.

## Tried and ruled out
- Keying the copy table by the first array's vertex-0 address: every draw of one big level buffer lands in the same set and they push each other out. Worse than the exact-start key.
- A second vertex worker (VP_FETCH_THREADS=2): +0.31%, noise. The worker has room; the cost is copies the FIFO thread finishes itself at a sync.

## Watch out
- The copy table must be on with or without vertex workers: test_rsx_synth's second process runs VP_FETCH_THREADS=0, and the two processes must give the same hashes. The sync hook is installed in vpj_threads() before it reads VP_FETCH_THREADS.
- #213 (deferred occlusion store) and #215 both change test_rsx_synth's golden table; whichever merges second needs the combined goldens. The goldens are compiled in: rebuild the test after changing them.
- A profile run needs DES_FPS=unlocked RSX_VSYNC=0 DES_VSYNC=0 in its own environment; knobab.sh sets them itself, kprof2.sh doesn't.
