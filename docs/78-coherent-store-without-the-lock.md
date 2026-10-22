# HighGraphics' coherent store without the lock (2026-10-21)

docs/72 ranked "the put-pointer store's lock" first among the next Palace candidates: about 12% of HighGraphics. This is the design for it and its measurement. The code is behind a live knob, `SPU_COH_FAST` (default 0). It is parked: a local ps3recomp branch `perf/coh-fast-store` (commit `6e77425`), with a copy of the patch in `work/tmp/scratch_20261021/coh/` (not in the repository).

## Short answer

- **What it changes.** A plain PPU store into a line an SPU has reserved no longer takes the line's stripe lock. Three things depend on that lock today, and each gets its own mechanism (section 2):
  - a PUTLLC commits only the 16-byte chunk it changed, with CMPXCHG16B against its snapshot (RPCS3 commits the same way);
  - GETLLAR checks its copy against memory;
  - the store's notify runs without the stripe, with a per-thread marker for the two rare paths that must wait for it.
- **Deep Palace:** it works, and it does not move the frame rate.
  - Every HighGraphics store to the line takes the fast path, about 5 million a second. HighGraphics' own work drops by about 6% of a core.
  - Frame rate: +0.4% alone, and +1.3% on top of the render thread (docs/68). Both are inside the 1.5% bar of the in-boot A/B.
  - HighGraphics spends the time it saves waiting in its usleep (10% of the thread before, 16% after). It waits on image 2, which is now the busiest worker (94%).
- **Correctness:** a new unit test (`runtime/spu/tests/test_spu_coh_fast.c`, 28 checks) with two threads on one line: no store and no commit lost, and no copy that shows a line that never existed. The existing coherency test passes (68 checks). A deep-Palace run with the knob on: no freeze, every HighGraphics store on the fast path.
- **Parked, not merged.** It adds a path that once froze the game and buys nothing measurable today. Take it up again once image 2 is faster (docs/72, next #2), and A/B it then.

## 1. What the lock costs (read, docs/72)

- HighGraphics writes the ring's put pointer once per command, about 5 million stores a second, to line 0x01B51080.
- Image 2 holds a reservation on that line: it GETLLARs it to compare put with get.
- A store into a reserved line goes through `VM_WRITE_COH` (`runtime/ppu/ppu_loader.cpp`): take the stripe (a locked bit-test-and-set), store, notify the reservation holder, release the stripe (a locked increment).
- Image 2 reads the stripe word on every lock-free GETLLAR (`spu_lockline_read_begin` and `_ok`), so both locked operations usually pull the stripe's cache line back from image 2's core.
- In the profile: `spu_lockline_unlock_ea` 4.5%, `spu_ll_acquire` 3.2%, `spu_lockline_lock_ea` 3.0%, `spu_coh_notify_write` 1.5% of HighGraphics.
- The slope: 30 ns added per coherent store took 0.71 ms off a 6.2 ms frame (slope 0.60, docs/72).

## 2. The design

### 2.1 What the stripe does for a plain store, and what replaces it

| The stripe makes sure that | Without it (`SPU_COH_FAST=1`) |
|---|---|
| The store cannot land inside a PUTLLC's compare and commit, and be written back over | PUTLLC writes only the 16-byte chunk it changed, with CMPXCHG16B against the snapshot. A store into that chunk makes the exchange fail, and the PUTLLC with it. A store into another chunk is not written over: it reads as landing after the commit. A PUTLLC that changes two or more chunks waits for the fast stores to the line (2.3) |
| A GETLLAR copy is from one instant: the lock-free copy is redone under the lock when a writer took the stripe | The copy is compared with memory after it is taken, and taken again until they match (`spu_coh_copy_line`). Without that, a copy could read an old word and then a newer one, a line that never existed |
| The reservation holder is told: `resv_valid` cleared, `SPU_EVENT_LR` raised, its host thread woken | The same notify, without the stripe (`coh_notify_fast`). A reservation it misses in a race is still caught by the data compares every PUTLLC and every wait already make (`spu_resv_lost_poll`, the GETLLAR nap) |
| `spu_coh_unregister` can free a context: every notify holds a stripe, and unregister takes them all | A fast store sets its thread's marker for its whole duration. Unregister empties the slot, fences and waits for the markers |

### 2.2 The fast store (`spu_coh_store_fast`)

1. Only an aligned store of 1, 2, 4 or 8 bytes, which x86 makes in one access. Anything else takes the locked path.
2. Set this thread's marker to the line with an exchange. The exchange is also the full fence before step 3.
3. If a wide commit has this line pending, clear the marker and take the locked path. The commit holds the stripe, so the store waits for it there.
4. Store, then notify (the table walk of `spu_coh_notify_write`, with atomic loads).
5. Clear the marker.

### 2.3 The wide commit

1. Publish the line as pending for its stripe.
2. Fence.
3. Wait until no marker names the line.
4. Compare memory with the snapshot again, copy the line, and clear the pending line.

Steps 1-3 and the store's steps 2-3 are a Dekker pair: either the store sees the pending line, or the commit sees its marker.

### 2.4 What stays as it is

- The lock-free GETLLAR and the no-op PUTLLC paths compare memory with the snapshot themselves, so they need no change apart from the checked copy.
- Stores from HLE code (`ppu_memory.h`), `stwcx.`/`stdcx.`, SPU DMA PUTs and PUTLLUC still take the stripe. Each of them is either a compare-and-swap or a plain write a racing store may follow, as on hardware.
- Linux: the fast path is Windows-only, like the striped lock it bypasses (the Linux build has one global lock-line lock). The functions are the old memcpy there.

### 2.5 Risks

1. **This path froze the game before** (the lwarx order, the no-op PUTLLC, the task claim: docs/28 and the memory notes). The new invariants are the ones in 2.1. The two-thread test checks them; a soak run is still to do.
2. **A missed wake.** A fast store whose notify runs just before a GETLLAR sets `resv_valid` leaves that reservation valid on a line that already changed. The PUTLLC's compare fails it, and a nap's compare before sleeping sees the change. What remains is the window the locked path has too: a store between the nap's check and its sleep, bounded by the 2 ms timeout.
3. **An A-B-A store.** A store that writes back the value already there, between a GETLLAR and its PUTLLC, no longer always fails the PUTLLC. The same holds in RPCS3. A store that changes a word and changes it back would have to fall between the reservation and the commit.

## 3. Measurements

All on the deep Palace save (`work/tmp/lc_deep/save`), not presenting (`RSX_PRESENT=0`), uncapped (`DES_FPS=unlocked DES_VSYNC=0 RSX_VSYNC=0`, vblank 960 kHz), at normal priority on every CPU.

### 3.1 The fast path at work (`SPU_COH_FAST_STAT=1`, one run)

Per 2 s, five readings in a row:

| Fast stores | Locked stores | One-chunk PUTLLCs (failed) | Wide PUTLLCs | GETLLAR re-copies |
|---|---|---|---|---|
| 9.3 to 10.3 million | 0 | 40,500 to 44,800 (1 to 4) | 0 | 490 to 800 |

- No wide commit happened on the deep Palace. The wide path is covered only by the unit test.
- About one PUTLLC in 20,000 failed because a store landed in its chunk after its compare. The PUTLLC is retried.
- A few hundred GETLLARs a second took their copy again, of about 1.8 million (the count in `spu_coh_reserve`'s comment).

### 3.2 Frame rate (`tools/des/knobab.sh`, 3 boots, windows ABBAAB, 6 s each)

| Build | A (knob off) | B (`SPU_COH_FAST=1`) | B - A | p | Verdict (bar 1.5%) |
|---|---|---|---|---|---|
| Fork master | 147.54 fps | 148.09 fps | +0.37% | 0.37 | does not count |
| With the render thread on (docs/68, `RSX_RF_THREAD=1`) | 153.27 fps | 155.27 fps | +1.30% | 0.07 | does not count |

### 3.3 Where the time goes (console `cpu 5`, one run each)

| Thread | Knob off | Knob on | With the render thread, knob on |
|---|---|---|---|
| FIFO thread (`vblank_ticker`) | 99.4% | 98.8% | 85.6% |
| HighGraphics | 98.8%, of it 10% in the OS (its usleep) | 97.8%, 16% in the OS | 97.2%, 15% in the OS |
| Image 2 (`spu_async_thread`, `ring_loop`) | 91.9% | 94.4% | 93.8% |

- The knob frees HighGraphics. HighGraphics still waits for image 2's reply each frame, so it waits longer instead of finishing sooner.
- With the render thread on as well, the FIFO thread also has room. That leaves image 2 and its exchange with HighGraphics as what sets the frame.

### 3.4 Two more things ruled out the same night

| Check | Result |
|---|---|
| The GPU (`nvidia-smi`, every 0.5 s for 15 s, master, 1280x720, about 155 fps) | 21-28% busy. The frame is not waiting on the GPU. |
| Image 2's GETLLAR nap off (`SPU_LLAR_NAP=0`, knobab, 3 boots, master) | 149.10 → 148.43 fps (-0.45%, p 0.53). The wake from the nap is not what HighGraphics waits for. |

The profile behind section 4 (`tools/des/kprof.sh`, master, 8 s each), in percent of the thread:

| Image 2 | | HighGraphics | |
|---|---|---|---|
| `ring_loop` | 13.3 | usleep's wait (`ntdll`) | 10.9 |
| `mfc_fast_xfer` | 9.4 | the lock-line path (lock, acquire, unlock, notify, `vm_write32`) | 13.8 |
| a wait in `ntdll` | 9.2 | lifted game code, the rest | most |
| the command regions (0x8CB8 9.1, 0x4498 5.7, 0x59C0 4.3, 0xCA40 4.2, 0x7140 4.0, 0xAB30 3.3, 0xC4D8 2.5) | 33.1 | | |
| channel writes (`spu_wrch_word`, `spu_wrch`) | 9.0 | | |

## 4. Next

1. **Image 2.** It is the busiest worker and the one HighGraphics waits on. docs/72 next #2 (commands 31-34 natively, about a quarter of image 2) is the large lever. docs/67 rank 2 (HighGraphics' reply wait on the reply word) shortens the wait between the two.
2. **This knob** comes back after that, with the same A/B. The design and the test are ready.
3. **Before a merge:** a soak run (`tools/des/soak.sh`, Nexus save) with the knob on, regress at 30 and 240, and a code review of the memory ordering in 2.2 and 2.3.
