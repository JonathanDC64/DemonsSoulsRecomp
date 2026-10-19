# Native code for SPU image 2's ring loop (plan)

Date: 2026-10-18. Status: static research and a plan. Nothing was built, lifted or run.

Addresses are local-store (LS) addresses in image 2 unless marked EA. Lift line numbers are for the lift as generated today.

Three labels are used below:
- **Read**: seen in the lift (`ps3recomp/work_spu/spu_0001_at_0176C200/spu_recomp.c`, generated and gitignored), in the SPU ELF (`work/spu_images/spu_0001_at_0176C200.elf`), or in our source (ps3recomp fork master `f15a4f0`, port master).
- **Measured**: numbers from earlier runs, quoted from docs/28. Nothing new was run for this plan.
- **Inferred**: a conclusion that still needs a run. Section 9 lists the runs.

## Short answer

- **What the two regions are (read).**
  - Image 2 (`spu_0001_at_0176C200`) is the SPU program that turns HighGraphics' 16-byte ring commands into RSX FIFO words.
  - `0x4498` is its command loop. Per command it:
    1. takes the command from the ring (the native fetch block since ps3recomp PR #21);
    2. PUTs the new get back to memory;
    3. dispatches on the command byte through a 37-entry jump table at `0x475C`;
    4. runs the handler, then calls `0x3C90`.
  - `0x3C90` is the end-of-command step. While the 32 KB local command buffer is under its limit, it only records where the next command's output starts (one 4-byte store to `0x1335C`).
    - Past the limit, it closes the 32 KB FIFO segment (semaphore release, JUMP, PUT, RSX put register). Then it moves the command that overflowed to the start of the buffer, switches to the next segment, and waits on a GCM label until the RSX has finished with that segment.
    - Command 35 only flushes the new bytes and publishes the write position, without a switch.
- **Why the lifted form is slow (read, inferred).** Each command pays three costs:
  - The jump-table `bi` at `0x4758` returns to the drain loop and goes through `spu_indirect_branch`, `spu_lookup` and a region re-entry.
  - It makes two call brackets, to `0xC358` and `0x3C90`, each with its own region prologue.
  - The SPU code keeps its registers in memory. For example, 0x3C90's fast path is 54 SPU instructions, 29 of them LS accesses, to do one compare and one store.
  - In docs/28's profiles these come to about 22% of image 2's time: region 0x4498 10.5% and indirect dispatch 6.5% (at ~118 fps), and 0x3C90 ~5.5% (at ~97 and ~105 fps).
- **The plan has five stages.** Each new piece of code has a live knob that defaults to the lifted code.
  - **Stage 0, count first.** `SPU_RING_HIST` adds three things:
    - a command histogram;
    - the 0x3C90 slow-path rate;
    - a one-time print of the context's image fields while the loop runs. Stage 1's enable test depends on them.

    Then take a fresh profile of image 2 and HighGraphics, and re-measure the causal slope with a temporary `TMP_SLOW_RING_NS`. Nothing behaves differently.
  - **Stage 1, `--bi-switch` in `spu_lifter.py`.**
    - A jump-table `bi` becomes a C `switch` over the table's targets. Each case is an `SPU_RGOTO`, and the old dispatch is the default.
    - It is exact while its enable test holds: `spu_indirect_branch` would send the branch to this image's own code and change nothing else. It works for any image whose table is listed.
    - It applies to two sites in image 2: the loop's at `0x4758` (37 entries) and one at `0x81A8` (16 entries), on the path of commands 30-34.
    - Knob: `SPU_BI_SWITCH`.
  - **Stage 2, a second native block, `0x4808-0x4818 = spu_hook_ring_post`.** It replaces the call to 0x3C90 when 0x3C90 would take its fast path. No lifter change. Knob: `SPU_NATIVE_POST`.
  - **Stage 3, a native steady-state loop inside the existing fetch block (`0x4618-0x4730`).**
    - The block keeps its contract, so the lifter does not change.
    - For each hot inline command, the loop does the whole command in C: the state PUT, the handler and the end-of-command step. Then it fetches the next command.
    - It returns to the lifted code at `0x4730` for anything else, and at `0x4618` when the ring is empty.
    - Knob: `SPU_NATIVE_LOOP`.
  - **Stage 4, only if the profile asks.** Native hooks for the hot out-of-line handlers. Commands 31-34 reach the large `0x8CB8` through `0x5400`.
- **Exactness.**
  - The native code makes the same channel calls in the same order: `spu_wrch` for every MFC command and tag wait, and `spu_llar_noop_commit` for the line.
  - It writes the same LS bytes.
  - At every exit, these are as the lifted code leaves them: r1, r80-r127, `ctx->pc`, and the volatile registers that are live at that exit.
  - A record/replay harness checks this against recorded all-lifted batches before any game run.
- **Expected gain (inferred).**
  - **CPU.** Image 2's busy time per command drops by about 20-35 ns of about 150 ns (13-23%), which frees about 0.1-0.17 of a core in the Palace uncapped.
  - **Frame rate at today's balance: 0 to about +2%.** In docs/28's last profiles, HighGraphics sets the frame (97% of a core), with the FIFO thread close behind.
    - Image 2's speed reaches the frame mostly through HighGraphics' reply poll, 6.8% of HighGraphics (about 0.55 ms a frame). A 13-23% faster image 2 shortens that poll by about the same share: +0.9-1.6% at most.
    - That would likely sit inside the A/B window spread (about one frame, ~0.8%).
    - docs/28 calls image 2 "off the margin" after an A/B at the 120 Hz cap, which squashed gains near 117 fps. A related trim re-checked at 240 Hz read +0.6 to +1.2%, so the real slope may be larger than this estimate. R1 measures it.
  - **Frame rate where image 2 sets the frame: +8-14%.** That was the case at ~106 fps, where 71% of added image 2 time showed in the frame. It could happen again in heavier views, or after HighGraphics and the FIFO thread get faster.
  - At 30 or 60 fps only the CPU time changes.
  - R0 and R1 (section 9) show which case holds before Stage 2 or 3 is written. Stage 1 is cheap and generic, so its A/B is worth running either way.
- **Main risks.**
  - The frame-rate gain may not show while HighGraphics sets the frame. Earlier image 2 trims read nothing or landed inside the spread. Under the house rule, a gain inside the spread is dropped. Keeping a change for its CPU saving alone is the maintainer's call.
  - This is the only path graphics commands take, so a wrong published get or state word shows up as corruption or a hang.
  - Reservation and coherence ordering (the lwarx and PUTLLC freezes).
- **Runs to request later.** Section 9:
  - R0: profile today's build.
  - R1: histogram and slope.
  - R2: A/B of Stages 1 and 2.
  - R3: recordings for the replay.
  - R4: A/B of Stage 3.
  - Soaks and regress after each A/B.

## 1. Image 2 and where its code is (read)

| What | Where |
|---|---|
| Image | `spu_0001_at_0176C200`, the second image `scripts/build_spu.py` registers (ids count from 1), so image id 2. Fingerprint `0x4FBCAB40231BF72B`. Host thread `spu_async_thread`. It is a SPURS task: `cellSpursCreateTask(id=0, ..., arg=01B51000)` (docs/21), and the fetch hook's header calls it "SPURS image 2". |
| ELF | `work/spu_images/spu_0001_at_0176C200.elf` (game-derived, not in git). Code `0x3000-0x1316F` (file offset 0x100), data from `0x13180` (file offset 0x10280), entry `0x3070`. |
| Lift | `ps3recomp/work_spu/spu_0001_at_0176C200/spu_recomp.c`. Region `0x3C90` is lines 941-1391 (17 entry cases). Region `0x4498` is lines 1522-2579 (57 entry cases). |
| Lift options | `scripts/build_spu.py`: `REGIONS` (image 2 is lifted with `--regions`), `NATIVE_HOOKS` (`0xC358=spu_hook_put_wait`), `NATIVE_BLOCKS` (`0x4618-0x4730=spu_hook_ring_fetch`). All are keyed by the fingerprint; another build of the game lifts without them. |
| Native code today | `ps3recomp/runtime/spu/spu_ring_hook.c` (fetch, knob `SPU_NATIVE_RING`), `spu_put_hook.c` (state PUT, knob `SPU_NATIVE_PUT`). |
| Runtime dispatch | `spu_drain_call` (`runtime/spu/spu_drain.c:450`), `spu_indirect_branch` (`runtime/spu/spu_channels.c`, master line 2845), `SPU_RGOTO` and `spu_call_done` (`runtime/spu/spu_context.h`). |
| Callers | `0x4498` has one caller: `0x53C8`, at `0x53F4` (r5/r6/r7 = 0x13600/0x1F600/0x27600). `0x53C8` is called from the call that returns to `0x30B0`. The loop returns only on command 3. |

## 2. The ring loop, 0x4498 (read)

### 2.1 The ring

These facts are from docs/24 and docs/28:
- The ring is 1 MB at EA 0x45A4B280.
- The control block is at EA 0x01B51000. Its word 0 is the consumer's get, written by image 2's state PUT.
- HighGraphics stores the put pointer at EA 0x01B51080 once per command.
- HighGraphics posts command 35 (`w0 = 0x9023`, a 0x90-byte payload) and spins in `usleep(30)` until image 2 writes the reply. docs/28 found one such poll site, `FUN_00acafa8` (lr 0x00ACB0A0), at about 1,100 polls a second.

A command is four big-endian words, w0..w3:
- w0 = `(size << 8) | cmd`.
- Some commands carry a payload in the ring after the 16-byte command. For those, size is the payload's length, and the handler advances get by it.
  - Read: the inline handlers 9 (16 bytes), 19, 21 and 37.
  - Inferred: command 35's 0x90 bytes, consumed by its out-of-line handler.

### 2.2 LS layout

Read from the set-up code at 0x4498-0x4614, the flush at 0x4300 and 0x3C90. The loop's caller passes r5/r6/r7 = 0x13600/0x1F600/0x27600 (`0x53D0-0x53E0`). At set-up, the loop GETs the 256-byte control block into 0x13400-0x134FF (`0x457C`), clears the state block, and calls `0x30C8` to set up the FIFO context.

| LS | Contents |
|---|---|
| `0x13300` | Staging for two PUTs. The 4-byte RSX put value goes out from `0x13300 + (EA & 15)` to the put register's EA. Then word 0 becomes the current write EA, and 16 bytes are PUT to the EA in word 3 of 0x13340. |
| `0x13310`, `0x13320`, `0x13330` | word 0: 0x13600 (local buffer), 0x1F600 (state block), 0x27600 (payload staging) |
| `0x13340` | FIFO context, set up by `0x30C8`. w0 = the EA that IO offset 0x8000 maps to (inferred from the put and JUMP arithmetic); w1 = the current segment's EA; w2 = the GCM label block's EA; w3 = where the 16-byte position PUT goes. |
| `0x13350` | w0 = semaphore value (+1 per segment switch); w1 = segment count; w2 = segment index; w3 (`0x1335C`) = where the next command's output starts |
| `0x13360` | w0 = bytes of the buffer already PUT; w2 = mode 0/1/2 (set back to 0 at each switch); w3 = which word of the label block to poll |
| `0x13370` | w0 = the RSX put register's EA |
| `0x13380` | local buffer descriptor {base 0x13600, limit base + 0x7FEC, cur, 0} |
| `0x13390` | word 0: the state block, 0x1F600 |
| `0x133A0` | word 0: the dirty block, 0x22740 (= 0x1F600 + 0x3140) |
| `0x13400` | ring state {get, ?, EA hi, EA lo}. Its 16 bytes are PUT to EA hi:lo after every command, which publishes get. |
| `0x13410` | word 0: the ring start (command 1's wrap target) |
| `0x13480` | copy of the control line (put in word 0, line EA in word 3); the GETLLAR target |
| `0x13500-0x13550` | pointer table, word 0 of each quadword: 0x13560, 0x13400, 0x27600, 0x13340, 0x1F600, 0x22740 |
| `0x13560` | the current command |
| `0x13580-0x135FF` | 128-byte GET buffer for the ring line |
| `0x13600-0x1B5FF` | the 32 KB local RSX command buffer |
| `0x12D60` | constant byte table used by command 7 (in the code segment) |

### 2.3 One command

1. **Fetch, `0x4618-0x472C`.** Since PR #21 this is the native block `spu_hook_ring_fetch`. The lifted fallback runs when the ring is empty or the line cannot be read without the lock. The steps:
   - set the event mask to 1024;
   - GETLLAR the control line into 0x13480;
   - if put == get, wait at `0x5120` (`rdch SPU_RdEventStat`, acknowledge, GETLLAR again);
   - otherwise PUTLLC the line back unchanged;
   - GET the 128-byte ring line at `get & ~127` into 0x13580;
   - copy the command to 0x13560;
   - store get + 16 at 0x13400.
2. **`0x4730`:** call `0xC358` put_wait(EA = words 2-3 of 0x13400, LSA 0x13400, 16), which publishes get. Since PR #29 this is the native hook `spu_hook_put_wait`.
3. **`0x4734-0x4758`:** cmd = byte 3 of 0x13560. If cmd - 1 > 36 (unsigned), go to `0x4804`. Otherwise load the target from the table and `bi`. Lifted (lines 1763-1766):
   ```c
   ctx->gpr[65] = spu_ls_read128(ctx, ctx->gpr[84]._u32[0] + ctx->gpr[67]._u32[0]);
   ctx->gpr[2] = spu_rotqby(ctx->gpr[65], ctx->gpr[66]);
   if (ctx->gpr[64]._u32[0] != 0) SPU_RGOTO(ctx, 0x4804u, spu_0001_at_0176C200_spu_func_00004804, loc_00004804);
   ctx->pc = ctx->gpr[2]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return;
   ```
4. **The handler** (section 2.4).
   - Inline handlers end with `r5 = LS[0x13560]`, then go to `0x4808`.
   - Out-of-line handlers call a function with r3 = 0x13380 and r4 = 0x13500. Then they set `r81 = r83 & 255` and `r5 = LS[0x13560]`, and go to `0x4808`.
5. **`0x4804`:** `r81 = r83 & 255`. **`0x4808`:** `r5 &= 255` (every word), r3 = 0x13340, r4 = 0x13380, then call `0x3C90`.
6. **`0x4818`:** if r81 == 0, go to `0x4618`. Otherwise run the epilogue: r1 += 128, restore r0 and r80-r85, return.

### 2.4 The command table

The table is 37 words at `0x475C`, indexed by cmd - 1. Anything above 36 goes to `0x4804`.

In the table below:
- "state" is the block at 0x1F600 and "dirty" the block at 0x22740.
- "GET" and "PUT" go through the helpers `0xC4D8` get_wait and `0xC358` put_wait, each a DMA on tag 0 followed by a wait.

| cmd | target | kind | What it does |
|---|---|---|---|
| 1 | 0x4850 | inline, PUT | ring wrap: get = word 0 of 0x13410; then `0x486C` PUTs the state again |
| 2 | 0x48BC | call 0x4300 | flush the local buffer: PUT the new bytes, eieio, PUT the RSX put register, then the 16-byte position PUT (a tail call to 0xC358) |
| 3 | 0x4880 | call 0x4300 | flush, then r83 = 1: the loop ends |
| 4 | 0x48A0 | call 0xF7B0 | |
| 5 | 0x48D0 | inline | w1 == 0: state word 0x3134 \|= 1; w1 == 1: \|= 2 (via 0x5218); any other w1: nothing |
| 6 | 0x4920 | inline | state word 0x3134 &= ~3 |
| 7 | 0x4958 | inline | state word 0x2850 + 4·w1 = w2; the dirty quadword \|= a byte pick from the table at 0x12D60 (Stage 3 has the exact form) |
| 8 | 0x49C0 | inline | state word 0x29B0 + 4·(24·w1 + w2) = w3; then `0x4A08` stores {1, 1, 1, 1} at dirty + 16 + (w1 & 16) |
| 9 | 0x4A34 | inline, GET, PUT | GET 16 bytes at get into state + 0x50; get += 16; PUT the state; dirty + 0x30 = all ones |
| 10 | 0x4AA0 | inline, GET | GET 32 bytes from EA w2 into state + 0xF0 + 32·w1; then `0x4A08` |
| 11 | 0x4AD8 | inline, GET | GET 128 bytes from EA w1 into state + 0x27D0 |
| 12 | 0x4B08 | inline | In the record at state + 0x370 + 16·b4 (b4 and b5 = w1's top two bytes): halfword +10 = b5, halfword +8 = w1 & 0xFFFF, word +0 = w2, word +4 = w3, in that order. Bytes 12-15 are kept. |
| 13 | 0x4B9C | inline, GET | GET 16 bytes from EA w1 into state + 0x470 |
| 14 | 0x4BC4 | inline, GET | GET 16 bytes from EA w2 into state + 16·w1. When w1 == 0, field copies follow (0x4BF0-0x4CEC, or 0x5148), depending on state words +8 and +0x48. Then `0x4D5C` sets dirty + 0x30 = all ones. |
| 15 | 0x4CF0 | inline, GET | GET 16 bytes from EA w1 into state + 0x40. Field copies at 0x5248 when state word +8 is 0 and word +0x48 is not. Then `0x4D5C` sets dirty + 0x30 = all ones. |
| 16 | 0x4DC0 | inline | state words 0x60, 0x64, 0x68, 0x6C = w1 >> 16, w1 & 0xFFFF, w2 >> 16, w2 & 0xFFFF; dirty + 0x30 = all ones |
| 17 | 0x4D68 | inline, GET | GET 32 bytes from EA w1 into state + 0x480 |
| 18 | 0x4D94 | inline, GET | GET 48 bytes from EA w1 into state + 0x4A0 |
| 19 | 0x4E60 | inline, GET, PUT | GET w0 >> 8 bytes at get into state + 0x4D0 + 16·w1; dirty + 0x40 = all ones; get += size; PUT the state |
| 20 | 0x4ED0 | inline | state word 0x3130 = w1 |
| 21 | 0x4F00 | inline, GET, PUT | GET w0 >> 8 bytes at get into state + 0x14D0 + 16·w1; get += size; PUT the state. Unlike 19, it sets no dirty flag. |
| 22 | 0x4F5C | call 0x128E8 | |
| 23 | 0x4804 | none | no-op |
| 24 | 0x4F74 | call 0x127F8 | |
| 25 | 0x4F8C | call 0xF9B0 | |
| 26 | 0x4FA4 | call 0xFB00 | |
| 27 | 0x4FBC | call 0xFA28 | |
| 28 | 0x4FD4 | call 0xC640 | |
| 29 | 0x4FEC | call 0xF238 | |
| 30 | 0x5004 | call 0xC7D0 | calls `0x7D70` |
| 31 | 0x501C | call 0xDEE0 | calls `0x5400` (which calls `0x7D70` and `0x8CB8`) |
| 32 | 0x5034 | call 0xE800 | calls `0x5400` |
| 33 | 0x504C | call 0xCA40 | calls `0x5400` |
| 34 | 0x5064 | call 0xD3C8 | calls `0x5400` |
| 35 | 0x507C | call 0x10550 | the `0x9023` request whose reply HighGraphics waits for |
| 36 | 0x47F0 | call 0xFC50 | |
| 37 | 0x5094 | inline, GET, PUT | GET w0 >> 8 bytes at get into 0x27600; copy w1 bytes of it to the local buffer at cur (`0xC1E8`); cur += w1; get += size; PUT the state |

Notes:
- Commands 2 and 3 pass r3 = 0x13340 and r4 = 0x13380 to `0x4300`.
- `0xC1E8` is a memcpy with no stack frame. It merges whole quadwords at both edges with `selb` masks, so for source and destination that do not overlap, its result is a byte copy. Read; the replay's synthetic cases check the edges.
- `0xC358` and `0xC4D8` each store the link register at sp + 0x10 and the back chain at sp - 0x20.
  - Their main case: size 16..16384, with LSA, EA and size all 16-byte aligned. It writes the latches LSA, EAH, EAL, Size, then TagID 0, Cmd (0x20 PUT or 0x40 GET), WrTagMask 1, WrTagUpdate 1, then reads `RdTagStat` into r3.
  - Sizes 1, 2, 4 and 8 take a short path with the same channel writes.
  - Over 16384 goes through `0x129F0` in pieces.
  - A misaligned request or size 0 halts (`heqi`).

### 2.5 What is live across the loop

- These registers are set before the loop and never written in it: r1, r82 = 0x13480, r84 = 0x475C, r85 = 0x13580.
- r83 is the exit flag. It is 0 until command 3 sets it to 1.
- Callee-saved registers written inside the loop (found by a scan of the region's register writes per label):
  - r80, by handlers 7, 12, 14, 16 and 37;
  - r81, everywhere (64 in the fetch, then r83 & 255);
  - r83, in command 3 only.
- r80's value reaches LS, because 0x3C90's prologue stores it on the stack. r81's does too.
- No volatile register (r2-r79) is live at 0x4618. The back edge comes straight from the call at 0x4814, and the SPU ABI clobbers volatile registers in a call. The same holds after the call at 0x4730, except r3 (its result).
- The existing put hook already relies on this rule: it sets only r3 and leaves the other volatile registers as they were.

## 3. The end-of-command step, 0x3C90 (read)

### 3.1 Fast path

The fast path is `0x3C90-0x3D64`: 54 instructions, 29 of them LS accesses.

1. The prologue saves 13 quadwords:

   | slot | register |
   |---|---|
   | sp - 0x10 | r80 |
   | sp - 0x20 | r81 |
   | sp - 0x30 | r82 |
   | sp - 0x40 | r83 |
   | sp - 0x50 | r84 |
   | sp - 0x60 | r85 |
   | sp - 0x70 | r86 |
   | sp - 0x80 | r87 |
   | sp - 0x90 | r88 |
   | sp - 0xA0 | r89 |
   | sp - 0xB0 | r90 |
   | sp + 0x10 | r0, in the caller's link slot |
   | sp - 0x100 | r1 |

   Then it sets r1 -= 256.
2. It loads the descriptor at 0x13380, {base, limit, cur, 0}, twice. It rotates the two copies into r3 = {cur, 0, base, limit} and r2 = {limit, cur, 0, base}.
3. If limit <= cur (unsigned), it goes to `0x3D68`, the slow path. If cmd == 35, it goes to `0x4130`.
4. Otherwise (`0x3D18`) it inserts cur into word 3 of 0x13350, so LS[0x1335C] = cur. Then it restores r0 and r80-r90, sets r1 += 256 and returns.

It leaves these volatile registers:

| Register | Value |
|---|---|
| r2 | {limit, cur, 0, base} |
| r3 | {cur, 0, base, limit} |
| r4 | `clgt(r2, r3)` |
| r6 | 0x13384 in every word |
| r7 | the descriptor |
| r8 | `ceqi(r5, 35)` |
| r29 | the old 0x13350 quadword |
| r30 | the `cwd` mask for word 3 |

### 3.2 Slow path

The slow path is `0x3D68-0x42F8`. It runs when the last command's output crossed the segment's limit. The limit leaves 0x8000 - 0x7FEC = 20 bytes for the closing words.

Names used below: `start` = LS[0x1335C], where the last command's output starts; `sem`, `count` and `index` = 0x13350 w0-w2; `segEA` = 0x13340 w1; `flushed` = 0x13360 w0.

The steps, in the order the code runs them (read):
1. **Rewind.** It copies the 32 bytes at `start` to sp + 48 (`0xC1E8`), then sets cur = start.
2. **Closing words**, chosen by the mode in 0x13360 w2:
   - Mode 0, and any value other than 1 or 2: `0x00041D6C`, `0x810`, `0x00041D74`, sem. That is SET_SEMAPHORE_OFFSET 0x810 (label 0x81), then TEXTURE_READ_SEMAPHORE_RELEASE with sem.
   - Mode 2 (`0x41D0`): the same words with `0x00041D70` (BACK_END_WRITE_SEMAPHORE_RELEASE), and sem with bytes 1 and 3 swapped. The RSX swaps them back when it writes the label (RPCS3 `back_end_write_semaphore_release`), so the label gets sem in both modes.
   - Mode 1: no words.
3. **JUMP** (`0x3E50`). The mode goes back to 0. next = (index + 1) mod count, from a division loop at `0x3EB0`; it halts if count is 0. It writes JUMP `0x20000000 | (next * 0x8000 + 0x8000)` and adds 4 to cur.
4. **PUT** (`0x4280`): put_wait(EA segEA + flushed, LSA base + flushed, size ((cur - base + 15) & -16) - flushed). Then flushed = (start - base) & -16; step 7 sets it to 0 again.
5. **Put register** (`0x3F30`). The value segEA + (start - base) - 0x13340 w0 + 0x8000 goes into the 0x13300 quadword at offset (put EA & 15). Then an MFC eieio (command 200), then a 4-byte put_wait to the put register's EA (0x13370 w0).
   - The value is the IO offset of the closing words, so the RSX runs up to them, not through them. Inferred: it runs them, and the JUMP, once a later put lands in the next segment.
6. **Copy back.** The 32 saved bytes go to base (`0xC1E8`). If the command's output is longer than 32 bytes, the rest is copied from start + 32 to base + 32 (`0x42C8`). Then cur = base + (old cur - start).
7. **Switch** (`0x3FD0-0x4088`): flushed = 0, index = next (the division runs again), segEA = 0x13340 w0 + next * 0x8000.
8. **Label wait** (`0x408C-0x40F8`). A 16-byte get_wait from the label EA (0x13340 w2) into sp + 32. It reads word 0x13360 w3 of those bytes and repeats until that word ≥ sem - count + 1 (unsigned). It also stops when sem - count + 1 and sem differ in their top bit, as when sem < count - 1.
   - Inferred: sem - count + 1 is the value released when the next segment was last closed. So this waits until the RSX has finished with that segment.
9. **`0x4100`:** sem += 1. Command 35 goes on to `0x4130`. Any other command goes to `0x3D18`, which stores cur at 0x1335C and returns.

**Command 35** (`0x4130`, from the fast path or after a switch) flushes without a switch:
- cur &= -16;
- put_wait(EA segEA + flushed, LSA base + flushed, size (cur - base) - flushed). A size of 0 would halt in 0xC358;
- flushed = cur - base, and 0x13300 w0 = segEA + flushed;
- a 16-byte put_wait from 0x13300 to the position EA (0x13340 w3);
- then `0x3D18`. There is no eieio and no put register write.

**The flush at `0x4300`** (commands 2 and 3) is a third variant. It PUTs the unflushed bytes when (cur - base + 15) & -16 is not 0, sets flushed = (cur - base) & -16, issues an eieio, writes the put register with the IO offset of cur, and ends with the 16-byte position PUT.

## 4. Why the lifted form is slow

**Read.**
- **(a) The dispatch.** The `bi` returns out of the region. Each command then goes through these steps:
  1. The drain loop of the call at 0x53F4 (`spu_drain_call`) runs the lockstep tick, `spu_step_check` (recent-pc ring, `steps`, the LS-0 test) and the interrupt test.
  2. `spu_indirect_branch` runs, in order:
     - the IRET-restore check (a call);
     - the policy-mode test, the 0xA70 test, the YDKJ and coverage knobs, and the 0xA00 test;
     - the task-region branch: targets are at or above 0x3000, so it calls `spu_taskset_resident_image`;
     - four resident-code slots;
     - `spu_lookup` (a hash);
     - the job-return test and `spu_check_stack_reset`;
     - a musttail call.
  3. The call lands in `spu_func_X` and enters the region body. Its prologue saves eight registers and xmm6-15 (docs/28), and the 57-case entry switch picks the label.

  Image 2 has two indirect-dispatch sites:
  - the loop's, which runs once per command;
  - `0x81A8` in region `0x7D70` (lift line 5924), a 16-entry table at `0x81AC`, reached from commands 30-34 through `0xC7D0` and `0x5400`.
- **(b) Two call brackets per command** (0xC358 and 0x3C90). Each bracket costs:
  - `spu_link`, the image id save and `host_depth`;
  - the callee region's prologue, epilogue and entry switch;
  - the `spu_call_done` test and the image id restore.
- **(c) The SPU code itself.** Registers live in `ctx->gpr`, which is memory. Every LS access byte-swaps four words. `shufb` is about 10 host operations, `cwd` and `cwx` are a compare and blend, and `rotqby` is a variable shuffle. Two examples:
  - 0x3C90's fast path: 54 instructions to do one compare and one 4-byte store.
  - Command 7's handler: 26 instructions, 9 of them LS accesses, and two `shufb`, for one word store and a 16-byte OR.
- **(d) `SPU_RGOTO` on every taken branch**: three globals (`g_spu_goto_slow`, `g_yz_lockstep_on`, `g_pm_flow_ctx`), the interrupt test and `steps++`.

**Measured (docs/28).**
- At ~118 fps: image 2 at 89% of a core, 15% of it napping. The DMA runtime is ~16% (`mfc_submit` 5.9%, `spu_wrch` 4.8%, `mfc_for` 2.7%). Indirect-branch dispatch is ~6.5% and region `0x4498` 10.5%.
- At ~97 and ~105 fps: region `0x3C90` was 5.4% and 5.7%.
- At ~67 fps: `0x4498` 13.8%, `0xC358` 7.0%, `0x3C90` 6.0%, `0x8CB8` 5.2%. "Within `0x4498` no instruction is above 0.6%."
- ~42,000 commands a frame in the Palace.
- Which thread sets the frame today is a separate question, covered in section 10.

**Inferred (per command at ~118 fps).**
- 42,000 × 118 ≈ 4.96 million commands a second. 89% of a core is then ~180 ns per command, of which ~27 ns is napping and ~152 ns busy. This takes the profile shares as shares of image 2's samples.
- Region 0x4498 is ~19 ns, the dispatch ~12 ns and 0x3C90 ~10 ns: ~40 ns, about 27% of the busy time.
- Part of the 6.5% dispatch share may come from the `0x81A8` site. R0 cannot split it by address; the histogram's command 30-34 count bounds it.
- The DMA runtime (~29 ns) stays, because the native code issues the same DMAs through the same calls.

## 5. The plan

Each stage is one change, with its own A/B and its own pull request: ps3recomp first, then the port when `build_spu.py` changes. PRs are based on master and never stacked. Tooling commits stay separate from fix commits.

### Stage 0: measure (no change in behaviour)

- `SPU_RING_HIST=<n>` (live), in `spu_hook_ring_fetch`. Just before `return 1`, it counts:
  - the command byte (byte 3 of 0x13560);
  - for command 37, size buckets for w0 >> 8 and for w1;
  - segment switches (the segment index at 0x13358 changing between two fetches);
  - fallbacks (the returns of 0).

  Every n commands it prints the counts. Once, on the first counted command, it also prints the context fields that `spu_indirect_branch` reads: `image_id`, `resident_task`, `resident_ovl`, `policy_mode`, `module_img_a00`, the four `resident_code` slots and `host_depth`. Stage 1's enable test depends on them.
- The first command after each wait is fetched by the lifted code and not counted. That is one per drain of the ring; the fallback count gives the rate.
- `TMP_SLOW_RING_NS` (temporary, as in docs/28) adds a spin per command, to measure today's causal slope (R1).
- The results decide:
  - which commands Stage 3 handles: the inline share against the out-of-line share (22, 24-36);
  - whether Stage 4 is needed: commands 30-34 lead into `0x7D70` and `0x8CB8`;
  - how often the exits happen.

### Stage 1: the jump-table switch (`spu_lifter.py`)

**The option.** `--bi-switch ADDR=TABLE:COUNT` (repeatable, `--regions` only).
- For the `bi` at ADDR, the lifter reads COUNT big-endian words at TABLE from the image.
- It keeps the targets that are in the same region body and makes them labels.
- It emits this ahead of the existing dispatch:

```c
        if (spu_bi_switch_on(ctx, s_self_img)) switch (ctx->gpr[2]._u32[0] & SPU_LS_MASK) {   /* RA of the bi */
        case 0x4850u: SPU_RGOTO(ctx, 0x4850u, spu_0001_at_0176C200_spu_func_00004850, loc_00004850);
        case 0x48BCu: SPU_RGOTO(ctx, 0x48BCu, spu_0001_at_0176C200_spu_func_000048BC, loc_000048BC);
        /* ... one case per distinct target: 37 here ... */
        default: break;
        }
        ctx->pc = ctx->gpr[2]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return;
```

**`spu_bi_switch_on(ctx, self)`** goes in `spu_context.h`, next to `SPU_RGOTO`. `self` is the image's own id. The lifter emits `static int s_self_img;` and sets it in the image's register function, from a new one-line getter for `s_reg_image` (`spu_registering_image()`, next to `spu_begin_image` in `spu_channels.c`).

```c
static inline int spu_bi_switch_on(const spu_context* ctx, int self)
{
    if (!g_spu_bi_switch || g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx)
        return 0;                                     /* knob off, or a diagnostic wants every hop */
    for (int s = 0; s < 4; s++)
        if (ctx->resident_code[s].image_id) return 0; /* these slots are searched first */
    return ctx->resident_task == self;                /* the lookup would use this image */
}
```

`g_spu_bi_switch` is the live knob `SPU_BI_SWITCH`: a global refreshed on `ps3_env_gen`, like `g_spu_ch_diag`.

**Why these tests** (read, `spu_indirect_branch` on fork master):
- Image 2 runs as a SPURS task. `spu_taskset_resident_image` matches the task ELF named at LS 0x2794 together with `policy_image_id == resident_ovl`. So while the taskset policy is resident, `resident_ovl` holds the policy's image and is not 0 (inferred; Stage 0 prints it), and a test for `!resident_ovl` would never pass.
- The targets are at or above 0x3000, so the dispatch first calls `spu_taskset_resident_image`. With `resident_task == self`:
  - its longjmp needs `!resident_task`, so it cannot fire;
  - a result of 0 leaves `resident_task` alone, and a result of `self` writes the same value again;
  - any other result would need LS 0x2794 or `resident_ovl` to change while image 2's code runs. Inferred not to happen; Stage 0 logs the fields, and the replay compares them.
- With the four `resident_code` slots empty, the lookup is `spu_lookup(pc, resident_task)`. It finds image 2's own function for every table target.
- `policy_mode` needs no test. Its two special addresses (0x9C0 and 0x9D0) are not table targets, and neither are 0xA70 and 0xA00.
- The job-return test (0x3FF00) cannot match a target. `spu_check_stack_reset` does nothing unless a reset entry is registered, and registering one sets `g_spu_goto_slow`.
- If Stage 0 shows `resident_task` at 0 while the loop runs, the last test becomes `!resident_task && !resident_ovl && image_id == self`. Registration then also checks that no legacy task entry (`spu_taskset_task_image`) equals a table target, because the dispatch would set `resident_task` from it.

**`build_spu.py`** gets a `BI_SWITCH` table shaped like `NATIVE_BLOCKS`:
`"spu_0001_at_0176C200": (0x4FBCAB40231BF72B, ["0x4758=0x475C:37", "0x81A8=0x81AC:16"])`.
- Table 1: 37 distinct targets, `0x47F0-0x5094`: the handlers in section 2.4.
- Table 2: 16 words at `0x81AC-0x81E8`, then an `lnop` at `0x81EC`. They hold 7 distinct targets: `0x7ED4` (10 of the entries), `0x86E4`, `0x8BE8`, `0x8BF0`, `0x8BF8`, `0x8C00` and `0x8C08`. The index is masked to a byte at `0x8030`, and an index over 15 goes to `0x7ED4` (`clgti` at `0x8160`, `brnz` at `0x81A4`). Read.
- The lifter refuses a target that is not a label in the same region body, and a target right after a `brsl` or `bisl`. No target here is either (read).

**Why it is exact.**
- A value from the table reaches the same label the trampoline reaches, for the reasons above.
- `SPU_RGOTO` keeps its slow conditions (a pending interrupt among them) and falls back to the trampoline.
- Any other value takes the old path.
- The drain hop that the switch skips does nothing else here. Its `spu_step_check` (fork master `spu_drain.c`) records the pc in the recent-pc ring, counts `steps`, and halts on a branch to LS 0. The drain loop also returns early when the target equals its return pc. No table target follows a call, so none can be a return pc.
- Image 2 never enables interrupts; its lift has no `int_enable = 1` and no `iret` (read).
- `ctx->steps` still counts one per dispatch, in `SPU_RGOTO` instead of the step check.
- Caveat: print-only traces (`SPU_POLLTRACE`, `SPU_JOBEXEC`, the recent-pc ring in crash reports) and knobs for other games (`SPU_IBCOV` on image 3, `YDKJ_CRI_CHAIN` on image 23) see fewer branches with the switch on.

**Expected (inferred):** the dispatch, the region re-entry and the drain hop, about 10-15 ns per command, plus the `0x81A8` hops on commands 30-34.

**Cost:** about 50 lines of Python, one inline function and the one-line getter. Then a re-lift of the SPU images and a port build. The PPU chunks do not rebuild.

### Stage 2: the end-of-command step in C (a second native block)

`NATIVE_BLOCKS` gains `"0x4808-0x4818=spu_hook_ring_post"`. The block covers four instructions, `andi r5`, `ila r3`, `ila r4` and `brsl 0x3C90`, so it covers the whole call. Sketch (`native_post_on` and `ring_diag` are new; the `spu_*` helpers are the runtime's):

```c
int spu_hook_ring_post(spu_context* ctx)                  /* 0x4808-0x4818 */
{
    if (!native_post_on() || ring_diag()) return 0;       /* g_spu_ch_diag, the LS probe gate */
    u128* r = ctx->gpr;
    const uint32_t sp = r[1]._u32[0];
    const u128 r5 = spu_andi(r[5], 255);
    /* In the lifted order: 0x3C90 stores its 13 quadwords before it reads the
     * descriptor. A return of 0 after the stores is harmless, because the
     * lifted code stores the same bytes again. */
    const u128 r0 = spu_link(0x4818);
    spu_ls_write128(ctx, sp - 0x20, r[81]);  spu_ls_write128(ctx, sp - 0x60, r[85]);
    spu_ls_write128(ctx, sp - 0x10, r[80]);  spu_ls_write128(ctx, sp - 0x30, r[82]);
    spu_ls_write128(ctx, sp - 0x40, r[83]);  spu_ls_write128(ctx, sp - 0x50, r[84]);
    spu_ls_write128(ctx, sp - 0x80, r[87]);  spu_ls_write128(ctx, sp - 0x90, r[88]);
    spu_ls_write128(ctx, sp + 0x10, r0);     spu_ls_write128(ctx, sp - 0x70, r[86]);
    spu_ls_write128(ctx, sp - 0xA0, r[89]);  spu_ls_write128(ctx, sp - 0xB0, r[90]);
    spu_ls_write128(ctx, sp - 0x100, r[1]);
    const u128 d = spu_ls_read128(ctx, 0x13380);          /* {base, limit, cur, 0} */
    const uint32_t limit = d._u32[1], cur = d._u32[2];
    if (!(limit > cur) || r5._u32[0] == 35) return 0;     /* slow path or flush: lifted */
    const u128 old = spu_ls_read128(ctx, 0x13350);
    u128 nw = old; nw._u32[3] = cur;
    spu_ls_write128(ctx, 0x13350, nw);                    /* LS[0x1335C] = cur */
    /* Registers as the lifted code leaves them (cheap, so all of them). */
    r[0] = r0;  r[5] = r5;
    r[2] = spu_rotqbyi(d, 4);                             /* {limit, cur, 0, base} */
    r[3] = spu_rotqbyi(d, 8);                             /* {cur, 0, base, limit} */
    r[4] = spu_clgt(r[2], r[3]);
    r[6] = spu_ila(0x13384);
    r[7] = d;
    r[8] = spu_ceqi(r5, 35);
    r[29] = old;
    r[30] = spu_cwd(spu_ai(r[1], -256), 12);
    /* What the call bracket and the callee's return leave. */
    ctx->pc = 0x4818;  ctx->drain_ret_pc = 0;
    if (ctx->module_img_a00) ctx->image_id = ctx->module_img_a00;
    return 1;
}
```

- The 13 stack slots are distinct, so their order does not matter. Following the lifted order means no check is needed for a frame that overlaps 0x13350 or 0x13380.
- `host_depth` is untouched; the bracket's `++` and `--` cancel out.
- **Expected (inferred):** about 6-9 ns per command (0x3C90's ~5.5% plus the bracket). The native code is about 40 host instructions.

### Stage 3: the native loop (inside the fetch block)

`spu_hook_ring_fetch` keeps today's contract: 1 means go on at 0x4730, 0 means run the lifted code from 0x4618. With `SPU_NATIVE_LOOP=1` it changes what happens before it returns 1:
- It looks at the command it just fetched.
- If the loop handles that command and every precheck passes, it does the rest of the command itself: the state PUT, the handler and the end-of-command step. Then it fetches the next command.
- It returns 1 at the first command it does not handle; the lifted code runs that command from 0x4730.
- It returns 0 when the ring is empty, and the lifted code waits as it does today.

The block now jumps forward along the program's trace, not only over its own range. The state at either exit is the state the lifted program has when it reaches 0x4730 (or 0x4618) for that command. The `--native-block` help text should say so.

```c
int spu_hook_ring_fetch(spu_context* ctx)                 /* 0x4618-0x4730 */
{
    /* today's checks: the knob, vm_base, channel diagnostics, r82 == 0x13480 */
    ring_fetch f;
    if (!fetch_core(ctx, &f)) return 0;                   /* today's body minus its register writes */
    if (!loop_on() || !loop_entry_ok(ctx)) {              /* r84 == 0x475C, r85 == 0x13580, r83 & 255 == 0,
                                                             frame [sp-0x100, sp+0x20) clear of 0x13300-0x135FF */
        fetch_regs(ctx, &f);                              /* today's register writes */
        return 1;
    }
    loop_state L = { .first = 1 };
    for (;;) {
        const ring_cmd c = cmd_at(ctx, 0x13560);
        if (!case_ok(ctx, &c, &L)) break;                 /* not handled here, or a precheck fails */
        state_put(ctx, &f);                               /* 0x4730: latches, spu_wrch(MFC_Cmd, 0x20), tag mask 1,
                                                             spu_wrch(MFC_WrTagUpdate, 1) */
        run_case(ctx, &c, &L);                            /* the handler, LS accesses in the lifted order */
        end_of_command(ctx, &c, &L);                      /* 0x4804-0x4818 and 0x3C90's fast path */
        L.n++; L.first = 0;
        if (loop_yield(ctx)) { loop_exit(ctx, &L); return 0; }   /* lifted from 0x4618 */
        if (!fetch_core(ctx, &f)) { loop_exit(ctx, &L); return 0; }
    }
    loop_exit(ctx, &L);                                   /* r81 = r83 & 255, r0 = link 0x4818, pc = 0x4818, ... */
    fetch_regs(ctx, &f);                                  /* after loop_exit: the fetch leaves r81 = 64 */
    return 1;                                             /* lifted from 0x4730 */
}
```

**The pieces.**
- **`fetch_core`.** Today's fetch, unchanged in its channel calls:
  - the `known` shortcut (87115e8); otherwise the live put read and `spu_llar_noop_commit`;
  - `ctx->event_mask = 1024`;
  - the GET through `spu_wrch` and the tag wait;
  - the copy to 0x13560, and get + 16 into 0x13400.

  It skips the ~30 register writes, which nothing reads unless the loop exits at 0x4730. There `fetch_regs` writes them, as today.
- **`case_ok`.** It runs before any side effect of the command and checks:
  - The command is in the handled set.
  - limit > cur in 0x13380 (for command 37: limit > cur + w1, in 32-bit arithmetic as the SPU does it).
  - Every LS address the handler writes, masked as the lifted code masks it, stays clear of 0x13300-0x135FF and of the frame.
  - Every DMA is in the helper's fast case (size 16..16384, everything 16-byte aligned).
  - For command 37, the copy's two ranges do not wrap the end of LS and do not overlap.

  If any check fails, the lifted code runs the whole command.
- **`end_of_command`** does 0x4804-0x4818 with 0x3C90's fast path. `case_ok` already showed that it takes the fast path.
- **`loop_yield`** is true when any of these holds: `g_spu_goto_slow`, `g_yz_lockstep_on`, `g_pm_flow_ctx`, `ctx->int_enable && (event_status & event_mask)`, a channel or LS diagnostic, or a `ps3_env_gen` change.

**Stack slots, exact but cheaper.**
- In the lifted path, each command writes sp + 0x10 and sp - 0x20 twice: put_wait writes link 0x4734 and r1, then 0x3C90 writes link 0x4818 and r81. The other 11 slots are written once, by 0x3C90.
- After 0x3C90, all 13 slots hold the same values on every command except sp - 0x10 (r80). The reason is that r81 = r83 & 255, and r82-r90, r1 and the link do not change inside the loop.
- So the loop writes all 13 slots on its first command after entry. After that it writes only sp - 0x10, and only when r80 changes. It writes LS[0x1335C] only when cur changes (command 37).
- put_wait's two stores are always overwritten later in the same command, so the loop drops them. The same goes for the helper stores in commands 1, 9, 19, 21 and 37.
- LS is private to the SPU, and nothing reads these bytes in between. At every exit, LS is byte for byte what the lifted code leaves.

**Registers.**
- r1, r82, r84 and r85 are checked at entry and never written.
- r80 is kept in `ctx->gpr[80]` when a handler changes it.
- r81 and r83 are set at the exit.
- Volatile registers: none is live at 0x4618. At the 0x4730 exit, `fetch_regs` writes the fetch's registers, including the put_wait arguments r3, r4 and r5.
- `loop_exit` sets `ctx->pc = 0x4818`, `drain_ret_pc = 0` and the image id rule from `spu_call_done`.
- It also adds 3 per command to `ctx->steps`: the dispatch, the branch to 0x4808 and the branch back to 0x4618. A few handlers take more steps when lifted. `steps` only feeds `SPU_RATE` and a few diagnostic prints, and the LS-0 test in `spu_step_check` only needs it non-zero after the first step.

**Command 7 in full.** It shows the level of exactness needed:

```c
case 7: {                                                 /* 0x4958 */
    uint8_t* ls = ctx->ls;
    const uint32_t state = be32(ls + 0x13390);
    be32_put(ls + ((state + 4 * c.w1 + 0x2850) & 0x3FFFC), c.w2);   /* lqx, cwx, shufb, stqx */
    ctx->gpr[80] = spu_ls_read128(ctx, 0x133A0);          /* the handler leaves r80 = this quadword */
    const uint32_t dirty = ctx->gpr[80]._u32[0];
    const uint8_t* t = ls + ((0x12D60 + (c.w1 & 0xF0)) & 0x3FFF0);
    uint8_t* d = ls + (dirty & 0x3FFF0);
    const uint8_t pick[4] = { c.w1 & 15, c.w2 & 15, c.w3 & 15, c.w0 & 15 };
    uint8_t v[16];
    for (int k = 0; k < 16; k++)                          /* shufb(t, t, andi({w1,w2,w3,w0}, 15)) */
        v[k] = (k & 3) == 3 ? t[pick[k >> 2]] : t[0];
    for (int k = 0; k < 16; k++) d[k] |= v[k];
    break;
}
```

`andi` acts on all four words, so the shuffle control uses every command word, not only w1 (read). Bytes 3, 7, 11 and 15 take t[w1 & 15], t[w2 & 15], t[w3 & 15] and t[cmd & 15]. The other twelve bytes take t[0]. The native code copies this exactly and does not try to "fix" it.

**Command 8:**

```c
case 8: {                                                 /* 0x49C0, then 0x4A08 */
    uint8_t* ls = ctx->ls;
    const uint32_t state = be32(ls + 0x13390);
    be32_put(ls + ((state + 4 * (24 * c.w1 + c.w2) + 0x29B0) & 0x3FFFC), c.w3);
    const uint32_t dirty = be32(ls + 0x133A0);
    be32x4_put(ls + ((dirty + 16 + (c.w1 & 16)) & 0x3FFF0), 1, 1, 1, 1);
    break;
}
```

**Order of cases**, to be confirmed by the histogram:
1. Inline commands without DMA: 5, 6, 7, 8, 12, 16, 20 and 23.
2. Command 1 (the wrap, which makes a second state PUT).
3. The payload commands 9, 19, 21 and 37. Their GET is `spu_wrch` with the same latch values as `0xC4D8`'s fast case.
4. Commands 10, 11, 13, 14, 15, 17 and 18.

Commands 2, 3, 4, 22 and 24-36 stay lifted. They call functions; 2 and 3 flush through 0x4300.

**Expected (inferred).**
- For the commands it handles, the lifted decode, the 0xC358 bracket, the handler body, the fetch's register writes, the stack saves and the per-command `SPU_RGOTO` all go away.
- What is left is mostly the two DMA calls per command.
- On top of Stages 1 and 2, that is about 8-15 ns per handled command.

### Stage 4: out-of-line handlers (only if R0 and R1 ask for it)

- Commands 30-34 go to `0x7D70` and, through `0x5400`, to `0x8CB8`.
- `0x8CB8` is about 1,950 instructions (region 0x8CB8-0xAB30) and was 5.2% of image 2 at ~67 fps (measured).
- A native version of it would be a project of its own. It is listed here so that a high command 30-34 count in R1 has a home.

### Not in this plan

- **Skipping the 128-byte GET when the ring line seems unchanged.** The bytes at 0x13580 differ once the producer has written more of that line, so this is not exact.
- **Publishing get less than once per command.** This changes when HighGraphics sees free space. It could be a separate experiment, but it is not exact.
- **The DMA runtime itself** (`mfc_submit`, `spu_wrch`, ~16% of image 2). That is a separate lever in the MFC code; this plan keeps calling it.

## 6. Keeping the semantics exact

- **Channels.**
  - Every MFC command and tag wait goes through `spu_wrch`, as in the lifted code. That is where the PUT's coherence notify, the reservation breaks and the commit hooks live.
  - The DMA latches are written in place, as `spu_wrch_latch` and today's two hooks write them.
  - The tag update values stay what the lifted code uses: 2 (all) for the ring GET, 1 (any) in the helpers.
  - `ctx->event_mask = 1024` as in the fetch.
  - `wrch SPU_WrEventAck` and the blocking `rdch SPU_RdEventStat` stay in the lifted wait at 0x5120.
- **Atomics and reservations.**
  - No new atomics. The line is handled exactly as today: `known`, or `spu_llar_noop_commit`, which takes the line from one instant (seqlock), clears the reservation, sets `atomic_stat = 0` and runs `spu_taskset_line_committed` and the line commit hook.
  - The freezes in docs/28 came from skipping those hooks and from deciding a PUTLLC without comparing the line. Neither is touched.
- **Memory ordering.**
  - get is published at 0x4730, before the handler runs. Payload commands publish again after their GET (0x486C, or 0x4A68 for command 9).
  - The loop keeps both PUTs in place and in order. Our MFC completes a PUT inside `spu_wrch` (copy to guest memory, then `spu_coh_notify_write`), so the order of the calls is the order HighGraphics sees.
  - The native code touches guest memory only through `spu_wrch`, `spu_llar_noop_commit` and the fetch's existing read of the live put word.
- **LS.**
  - The same bytes as the lifted code, including stack slots and including bytes that look like garbage: command 7's dirty pick, and 0x3C90's saves.
  - Addresses are masked exactly as the lifted code masks them: `& 0x3FFF0` for quadwords, and the word slot from `cwd`/`cwx`.
- **Registers.** At every exit these match the lifted code:
  - r1 and r80-r127;
  - `ctx->pc`;
  - every volatile register live at the exit label (section 2.5).

  Stage 2 writes all the registers its block writes, because it is cheap. The replay checks the liveness claim by poisoning: section 8.
- **Exits.** The native code bails, before any side effect, on:
  - a channel or atomic diagnostic, the LS probe gate, lockstep or flow tracing;
  - `g_spu_goto_slow`;
  - a pending interrupt;
  - an unexpected layout (r82, r84, r85, a frame that overlaps 0x13300-0x135FF);
  - a DMA outside the helpers' fast case;
  - any doubt in a precheck.

  The lifted code then runs as it does today.

## 7. Knobs

All knobs are live (read through `PS3_KNOB` and refreshed on `setenv`). The native ones default to 0 (lifted) until their A/B is in.

| Knob | Values |
|---|---|
| `SPU_RING_HIST=<n>` | Stage 0: print the command histogram, fallbacks, segment switches and command 37 sizes every n commands. 0 = off. |
| `TMP_SLOW_RING_NS=<ns>` | Stage 0, temporary (not to be merged): spin this long per command, for the causal slope. |
| `SPU_BI_SWITCH` | Stage 1: 1 = the switch, 0 = the trampoline as today. The switch also needs the image's `BI_SWITCH` entry at lift time. |
| `SPU_NATIVE_POST` | Stage 2: 1 = native fast path at 0x4808, 0 = lifted. |
| `SPU_NATIVE_LOOP` | Stage 3: 1 = the loop, 0 = today's fetch block. `SPU_NATIVE_RING=0` still turns the whole block off. |
| `SPU_RING_REC=<dir>,<every>,<mode>` | Section 8: record one batch in every `<every>`. Mode `lifted` makes the native code bail for recorded batches. Mode `asis` records whatever runs. |

`SPU_RATE`'s "steps per busy ms" counts lifted gotos. It is not comparable between these builds.

## 8. Equivalence tests

### 8.1 Record and replay (no game)

**Recording** (`SPU_RING_REC`, image 2 only):
- A batch runs from one empty-ring wait to the next: the `rdch SPU_RdEventStat` at 0x5120.
- The ring drains at every `0x9023` round trip, so batches are a few thousand commands.
- At the start of a batch the recorder saves:
  - the 256 KB LS and the 128 registers;
  - the MFC latches, tag mask and status;
  - the event mask and status;
  - the reservation fields (`resv_valid`, `resv_ea`, `resv_line`, `atomic_stat`).
- During the batch it logs, in order:
  - every guest-memory read the image makes, with its EA and bytes (GET data, GETLLAR lines, the fetch's live put read);
  - every write (PUT EA and bytes).
- At the next wait it saves the end state the same way.
- Records are game-derived: they stay in the scratch directory and are never committed.

**Replay.** A test next to the runtime's others, for example `runtime/spu/tests/test_spu_ring_replay.c`. It is built against the local lift, which is not in git:
- **Fake guest memory.** The ring bytes the batch consumed, the control line with put at the batch's end, and each GET source.
  - Within a batch the consumer never sees put == get, so presenting the final put from the start gives the same command sequence.
  - A source EA read twice is served in order.
- **A fake MFC.** It serves GETs and the line from that memory, appends PUTs to a list, and implements `spu_wrch`, `spu_rdch`, the latches, `spu_llar_noop_commit`, `spu_lookup` and a small drain loop.
- **The run.** It starts by returning from the logged `rdch` and stops at the next one (a `longjmp` out).
- **The comparison:**
  - all of LS;
  - r1 and r80-r127, plus the volatile registers live at the stop point;
  - the PUT list (EA, size, bytes, order);
  - the MFC latches and the reservation fields.

  Event status bit 10 (the lock-line event) depends on timing and is masked.
- **Three runs per record.** All lifted (`SPU_NATIVE_RING=0`, `SPU_NATIVE_PUT=0`), which must match the recording exactly and so checks the harness. Today's hooks. Then the new code.
- **Poisoning.** Fill r2-r79 with a pattern at 0x4618 and at the 0x4730 exit, re-run, and expect the same result. That checks the claim that nothing reads them.
- **The reverse check.** Records made in the game with the native code on (`asis`) are replayed through the all-lifted path.

### 8.2 Synthetic cases

The same harness, with generated rings and the lift as the oracle:
- each handled command with random words;
- edge words:
  - w1 large enough to wrap the LS mask;
  - command 7 with w1 = 572 to 575, whose word store lands in the dirty quadword at 0x22740 (0x1F600 + 0x2850 + 4 × 572 = 0x22740). The native code keeps the lifted order, the store and then the OR, so it stays exact; this case tests that;
  - command 8 with large w1 and w2;
- cur at limit - 1, limit and limit + 1;
- command 37 with w1 = 0, 1, 15, 16, 17, 32 KB and unaligned cur;
- payload sizes 0, 16, 16384 and 16400 (chunked: must exit);
- ring wrap (command 1);
- a frame placed over 0x13350;
- a diagnostic knob set in the middle of a batch.

0xC1E8's edge merges must give a byte copy in every one of these before the loop uses a plain copy.

### 8.3 In the game

- `regress.sh` with the knobs on: exit 0.
- `soak.sh`, about 6 minutes each in the Palace and the Nexus at DES_FPS=120. The flip count must keep rising.
- A recording with the knobs on (`asis`), replayed offline through the lifted path.
- Then the A/B (section 9). The game can only show visible breakage and frame rate; the replay is where exactness is checked.

## 9. Run requests

**Common setup.**
- Run from the repository root in Git Bash. From PowerShell, wrap each line as `& "C:\Program Files\Git\bin\bash.exe" -c "<line>"`.
- An agent points `DES_SCRATCH` at its own scratchpad.
- The scripts play from the pinned regression save (`work/tmp/regress/save`) and put the player's save back. For the Nexus runs, set `LC_SAVE` to a copy of a Nexus save, never to the live save folder.
- `RSX_PRESENT`: exeab.sh and soak.sh pick it from the lock state. For the other scripts, pass `RSX_PRESENT=0` while the session is locked, and keep it the same for every run of a batch.
- Measure at `VBLANK_MHZ=240000` with `DES_FPS=60`, as exeab.sh does. One change per A/B. A change has to beat the window spread in every window, or it is dropped (docs/28).
- `port/build_l10s0`, `port/build_l10s12` and `port/build_l10s3` stand for the builds of Stages 0, 1-2 and 3.

**R0. Profile, today's build (no code needed).**
```
PF_PAT=spu_async_thread PF_TO=1 PF_VERB=profrip tools/des/kprof.sh l10_r0_img2 DES_FPS=60 VBLANK_MHZ=240000
PF_PAT=HighGraphics PF_TO=1 PF_VERB=profrip tools/des/kprof.sh l10_r0_hg DES_FPS=60 VBLANK_MHZ=240000
python tools/des/profsym.py port/build/des_boot.map $DES_SCRATCH/l10_r0_img2.out
python tools/des/profsym.py port/build/des_boot.map $DES_SCRATCH/l10_r0_hg.out
```
- Check that the first run profiled image 2: its hot symbol is `spu_region_00004498` or `spu_llar_idle_wait`. If it caught the audio mixer, rerun with `PF_FROM=2 PF_TO=2`.
- Note for image 2: regions 0x4498, 0x3C90, 0xC358 and 0x7D70; `spu_indirect_branch` + `spu_lookup` + `spu_drain_call` + `spu_taskset_resident_image`; the two hooks; `mfc_submit`, `spu_wrch` and `mfc_for`; napping.
- Note for HighGraphics: its usleep poll for image 2 (lr 0x00ACB0A0) and its ring stores.
- Note the `cpu 4` figures for the FIFO thread, HighGraphics and image 2 (in `$DES_SCRATCH/l10_r0_img2.cpu`).

**R1. Histogram and slope (Stage 0 build).** The walk file is `work/tmp/l10_walk.txt`:
```
sleep 20
stick 128 0 8
stick 128 255 8
sleep 20
```
```
DES_EXE=port/build_l10s0/des_boot.exe tools/des/levelcap.sh l10_hist_palace work/tmp/l10_walk.txt SPU_RING_HIST=2000000 DES_FPS=60 VBLANK_MHZ=240000
LC_SAVE=<nexus save copy> DES_EXE=port/build_l10s0/des_boot.exe tools/des/levelcap.sh l10_hist_nexus work/tmp/l10_walk.txt SPU_RING_HIST=2000000 DES_FPS=60 VBLANK_MHZ=240000
DES_EXE=port/build_l10s0/des_boot.exe tools/des/knobab.sh l10_slope10 TMP_SLOW_RING_NS=10 DES_FPS=60 VBLANK_MHZ=240000
DES_EXE=port/build_l10s0/des_boot.exe tools/des/knobab.sh l10_slope50 TMP_SLOW_RING_NS=50 DES_FPS=60 VBLANK_MHZ=240000
```
- Read the `[ring-hist]` lines in `$DES_SCRATCH/lc_<name>/run.txt`, and the image field line printed once.
- From knobab, slope = fps lost per added ns per command. Adding time can only make image 2 more critical, so the slope bounds what removing time can give: the gain of a stage is at most slope × the ns it saves per command. The 10 ns run is the closer estimate; the 50 ns run shows how fast image 2 becomes the limit.
- If the 10 ns slope is inside the window spread, image 2 is off the path. Then Stage 1 is the last stage worth doing now, or none is.

**R2. Stages 1 and 2 (one build, both knobs live).**
```
DES_EXE=port/build_l10s12/des_boot.exe tools/des/knobab.sh l10_bisw SPU_BI_SWITCH=1 DES_FPS=60 VBLANK_MHZ=240000
DES_EXE=port/build_l10s12/des_boot.exe tools/des/knobab.sh l10_post SPU_NATIVE_POST=1 DES_FPS=60 VBLANK_MHZ=240000 SPU_BI_SWITCH=1
tools/des/soak.sh l10_soak_palace port/build_l10s12/des_boot.exe SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
LC_SAVE=<nexus save copy> tools/des/soak.sh l10_soak_nexus port/build_l10s12/des_boot.exe SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
SPU_BI_SWITCH=1 SPU_NATIVE_POST=1 DES_EXE=port/build_l10s12/des_boot.exe tools/des/regress.sh
```
- The second A/B runs on top of the first. Drop `SPU_BI_SWITCH=1` from it if the first A/B fails.
- The build itself against master (does the extra code cost anything with the knobs off?): `tools/des/exeab.sh l10_s12_off port/build/des_boot.exe port/build_l10s12/des_boot.exe 2`.

**R3. Recordings for the replay (Stage 3 build, loop off).**
```
DES_EXE=port/build_l10s3/des_boot.exe tools/des/levelcap.sh l10_rec_palace work/tmp/l10_walk.txt SPU_RING_REC=<scratch>/l10_rec,500,lifted
LC_SAVE=<nexus save copy> DES_EXE=port/build_l10s3/des_boot.exe tools/des/levelcap.sh l10_rec_nexus work/tmp/l10_walk.txt SPU_RING_REC=<scratch>/l10_rec_nexus,500,lifted
```
Then run the replay offline, without the game. All three modes must report 0 differences, and so must the poison runs.

**R4. Stage 3.**
```
DES_EXE=port/build_l10s3/des_boot.exe tools/des/knobab.sh l10_loop SPU_NATIVE_LOOP=1 DES_FPS=60 VBLANK_MHZ=240000 SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
tools/des/soak.sh l10_soak3_palace port/build_l10s3/des_boot.exe SPU_NATIVE_LOOP=1 SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
LC_SAVE=<nexus save copy> tools/des/soak.sh l10_soak3_nexus port/build_l10s3/des_boot.exe SPU_NATIVE_LOOP=1 SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
SPU_NATIVE_LOOP=1 SPU_BI_SWITCH=1 SPU_NATIVE_POST=1 DES_EXE=port/build_l10s3/des_boot.exe tools/des/regress.sh
DES_EXE=port/build_l10s3/des_boot.exe tools/des/levelcap.sh l10_rec_asis work/tmp/l10_walk.txt SPU_RING_REC=<scratch>/l10_rec_asis,500,asis SPU_NATIVE_LOOP=1 SPU_BI_SWITCH=1 SPU_NATIVE_POST=1
```
Then run R0 again with the accepted knobs, to see which thread sets the frame now.

Rough time per batch: R0 6 minutes, R1 13, R2 25, R3 6, R4 30.

A frame-rate A/B may land inside the window spread even when the code is right (section 10). The house rule then drops it. The CPU figure from `cpu 4` is still worth noting for that case.

## 10. Expected gain (inferred)

| Stage | What goes away per command | Saved per command |
|---|---|---|
| 1 | the trampoline bounce: region exit, drain hop, `spu_indirect_branch`, `spu_lookup`, region entry (plus the 0x81A8 hops on commands 30-34) | ~10-15 ns |
| 2 | the 0x3C90 call bracket and its 54 lifted instructions | ~6-9 ns |
| 3 | for handled commands: decode, the 0xC358 bracket, handler body, fetch register writes, stack saves, `SPU_RGOTO` | ~8-15 ns more |

- **Total:** about 20-35 ns of ~152 ns busy per command (13-23% of image 2's busy time), depending on the command mix that R1 measures.
- **CPU:** 20-35 ns × 4.96 million commands a second ≈ 0.10-0.17 of a core in the Palace uncapped. At 30 fps there are a quarter as many commands a second, so the saving is a quarter as large.
- **Frame rate today (Palace uncapped, ~120 fps): 0 to about +2%, likely inside the window spread.**
  - HighGraphics sets the frame. After ps3recomp PR #31 it was at 97% of a core, with the FIFO thread at ~80% and close behind: 1 ms added a frame cost 0.77 ms on HighGraphics and 0.87 ms on the FIFO thread (measured, docs/28).
  - Image 2 reaches the frame mainly through HighGraphics' reply poll: 6.8% of HighGraphics at ~118 fps (measured), about 0.55 ms a frame.
  - The reply waits for image 2 to work through the commands queued before it (inferred). If image 2 does that 13-23% faster, the poll shrinks by about 0.07-0.13 ms a frame. That is +0.9-1.6% if HighGraphics alone set the frame, and less once the FIFO thread takes over.
  - docs/28 calls image 2 "off the margin" because inlining `mfc_for` (2.7% of image 2) gave 116.9 vs 117.0. That A/B ran at the 120 Hz cap, which squashed gains near 117 fps, so it does not show a zero slope.
  - The MFC slot in the SPU context (part of `mfc_for`'s 3.6%) was re-checked at 240 Hz: +1.2%, then +0.6 to +1.1% on top of three other changes. That is inside the spread, but more than the poll estimate predicts for a few ns per command. So the estimate above may be low; R1's 10 ns slope settles it.
- **Where image 2 sets the frame: +8-14%.**
  - That was the case at ~106 fps, where 71% of time added to image 2 showed in the frame (measured).
  - 20-35 ns × 42,000 commands is 0.84-1.47 ms a frame. 71% of that is 0.6-1.04 ms of an 8.3 ms frame at ~120 fps.
  - This can come back in heavier views, or once HighGraphics and the FIFO thread get faster.
- **Precedents (measured, docs/28):**
  - The state-PUT hook, 11% of image 2: +5.1% at ~112 fps, while HighGraphics waited on image 2.
  - The native fetch: +15%, then +8%, while image 2 was the limit.
  - `mfc_for` inline (2.7%): 116.9 vs 117.0, at the 120 Hz cap.
  - The MFC slot: +0.6 to +1.2% at 240 Hz, inside the spread, so dropped.
  - Extending the fetch block over the state PUT: no gain.
- **At DES_FPS=30 or 60:** no frame-rate change, only less CPU time. That helps on PCs with fewer cores.
- **What decides:** R0 (which threads are busy) and R1 (the slope at 10 and 50 ns). Stage 1 is cheap and leaves the semantics alone, so its A/B is worth running either way. Stages 2 and 3 wait for R1.

## 11. Risks

- **Only one path.** The ring is the only route for graphics commands.
  - A wrong published get lets HighGraphics overwrite commands not yet read (corruption or a hang).
  - A wrong state or dirty byte gives subtle render bugs.
  - A wrong cur or boundary breaks the FIFO (an RSX hang).

  Mitigations: prechecks that bail early, the replay, regress, soaks, and knobs that default off.
- **State thought dead that is read after all:** volatile registers after a call, stack slots, `ctx->pc`. Mitigations: the poison runs, the strict register set in Stage 2, and LS kept byte-exact everywhere.
- **Reservation and coherence order.** The lwarx and PUTLLC freezes took minutes to show. Mitigations: no new atomics, the same calls in the same order, 6-minute soaks in the Nexus and the Palace at DES_FPS=120.
- **The gain may not show while another thread sets the frame** (the fetch-block extension, `mfc_for` and MFC slot precedents). Mitigations: R1's slope, measurement at 240 Hz, and the house rule to drop changes inside the spread.
- **Diagnostics.** The native code bails when channel or LS diagnostics are on, so traces still see every access. `SPU_RATE`'s steps figure is not comparable across these builds.
- **A replay mismatch can be a lifter bug,** not a native one. Check the SPU ISA, as with the earlier rotate and shufb fixes, before "fixing" the native side.
- **Tied to one game build** by the fingerprint. Another build lifts without these pieces.
- **Stage 1 touches every image that gets a `BI_SWITCH` entry.** Only image 2 gets one here.

## 12. Addresses

| LS | What |
|---|---|
| `0x30C8` | sets up the FIFO context (called by the loop's set-up) |
| `0x3C90` | end-of-command step; fast path to `0x3D64`, slow path `0x3D68-0x42F8` (JUMP `0x3E50`, put register `0x3F30`, label wait `0x408C-0x40F8`, mode 2 words `0x41D0`, segment PUT `0x4280`, rest of the copy-back `0x42C8`), command 35 at `0x4130` |
| `0x4300` | flush the local buffer (commands 2 and 3); tail-jumps to `0xC358` |
| `0x4498` | ring loop: set-up `0x4498-0x4614`, fetch `0x4618`, publish `0x4730`, dispatch `bi` `0x4758`, table `0x475C`, end of command `0x4804`/`0x4808`, loop test `0x4818`, epilogue `0x481C`, handlers `0x47F0` and `0x4850-0x5094`, empty-ring wait `0x5120` |
| `0x486C` | second state PUT (commands 1, 19, 21 and 37; command 9 has its own at `0x4A68-0x4A88`) |
| `0x4A08` | sets {1, 1, 1, 1} at dirty + 16 + (w1 & 16) (commands 8 and 10) |
| `0x53C8` | calls the loop at `0x53F4` |
| `0x5400` | called by commands 31-34; calls `0x54B8`, `0x59C0`, `0x6B30`, `0x6840`, `0x7140`, `0x7D70` (at `0x547C`, return `0x5480`) and `0x8CB8` (at `0x5488`, return `0x548C`), then tail-jumps to `0x6180` |
| `0x7D70` | region with the second jump table: index masked at `0x8030`, over 15 goes to `0x7ED4` (`0x8160`, `0x81A4`), `bi` at `0x81A8`, 16 words at `0x81AC` |
| `0x8CB8` | large output generator (about 1,950 instructions) |
| `0xC1E8` | LS memcpy (dst r3, src r4, n r5) |
| `0xC358` | put_wait (EA r3, LSA r4, size r5); native hook `spu_hook_put_wait` |
| `0xC4D8` | get_wait (LSA r3, EA r4, size r5) |
| `0x12D60` | command 7's byte table |

| EA | What |
|---|---|
| `0x45A4B280` | the 1 MB ring |
| `0x01B51000` | control block, word 0 = image 2's get (its state PUT) |
| `0x01B51080` | put, stored by HighGraphics |
