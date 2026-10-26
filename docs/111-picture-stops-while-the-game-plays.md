# 111. The picture stops while the game plays on (issue #205)

Date: 2026-10-26. Status: researched without the reporter's logs, which were asked for.
- Two ways the D3D12 backend can stop presenting for good while the game keeps flipping were found in the code.
  One of them reproduces the issue's symptom on demand.
- Fixes for both are in JonathanDC64/ps3recomp#132. This port's `ps3recomp.pin` moves to it in a later pull
  request.
- Which way, if either, hit the reporter waits for their `des.log`.

## Short answer

- **Issue #205:** about five minutes into play, the picture froze. The game went on: sound, input and the F1 menu
  worked, and the report filed from the menu showed the frozen frame in its screenshot.
- **The report's frame-rate list** ends `228 222 224 196 0 2 0 142 240 240 242`: a 3 s stall, then exactly 240,
  the vblank cap.
  - "Game frames" counts the game's flip requests, not pictures shown.
  - So 240 with a frozen picture means the game flips at its cap while nothing is drawn. Drawing nothing is also
    why the rate rose from about 225 to the cap.
- **How a frame ends since present-at-flip** (`c01c7f7`, docs/28): the backend shows a frame only when a flip
  in the command stream reaches the FIFO walker (`d3d12_fifo_flip`). From the first such flip on, display clears
  and the ticker no longer present.
- **Two ways out of that never come back:**
  1. **Flips outside the command stream.** A flip the PPU makes directly never reaches `d3d12_fifo_flip`:
     - an immediate `cellGcmSetFlipCommand`;
     - `_cellGcmSetFlipCommandWithWaitLabel`, which the fork always makes directly (Demon's Souls imports it;
       upstream 0650a590 queues it in the stream);
     - a flip with no room in its buffer.

     If the game's flips all go that way, nothing presents again. Forced here, the picture dropped to 0.4-2
     pictures a second while the game flipped at 160-420 a second (section 2).
  2. **A full batch at a flip with no display draw.** Such a flip keeps the batch for the composite that ends it.
     Once the batch is full (16384 records), that composite is refused, and nothing empties the batch again. In
     Demon's Souls this is close to unreachable: every Palace frame has about 460 records that count as display
     draws, from its second record on.
- **Why the F1 menu still works:** in both cases the overlay repaints the last frame shown once the game stops
  presenting (`rsx_d3d12_overlay_tick`). That is also why the report's screenshot shows the frozen frame.
- **The fix** (ps3recomp#132): after three flips outside the stream, display clears end the frame again. A full
  batch is drawn at its flip without a present. Neither path runs in normal play, and the frame-rate A/Bs show no
  change (section 4).

## 1. What the report shows

- **Setup:** GTX 1070 (Pascal), Windows 10 Pro N 22H2. Borderless fullscreen on a 1080p 180 Hz display, with a
  60 Hz second display. Build 0.1.0-dev.915, ps3recomp f1ee21ab.
- **At the report** (4 min 56 s in):
  - game frames 44036, overlay frames 29699;
  - process memory 2084 MB;
  - GPU memory 475 MB of a 7299 MB budget.
- **The screenshot:** Boletarian Palace, soul form, an item pickup dialog on screen.
- **What it rules out:**
  - The device was not lost: the F1 menu presented through the same device and swap chain, and the screenshot
    was read back from it.
  - The port's own hang notice did not fire. It keys on flips stopping, and they did not stop.
- **Not in the report yet:** the zip (`des.log`, `game.txt`, `settings.ini`).

## 2. Runs on this PC

**Flips outside the stream**, reproduced:
- **The trigger:** `GCM_FLIP_CALLBACK=0`, set live from the debug console. Demon's Souls' 32 KB ring runs past
  `end`, and the rule behind that setting is what puts its flips into the stream there. With it off, all but one
  to three flips a second take the immediate path. Held for 20 s at the deep Palace save (`m02`), with
  `GCM_RATES=1`.
- **Before the fix:**
  - 0.4-2.2 presents a second for the whole window (the window title read 0.56 fps);
  - meanwhile 160-420 flips a second, and `label255` kept counting, so the game ran on;
  - presents came back when the setting was restored.
- **After the fix:** 117-129 presents a second, against about 130 before the window.
  - `[D3D12] N flips outside the command stream ... presenting at display clears` and `... in the command stream
    again` alternate. The game still puts one or two flips a second into the stream, and each of those ends the
    fallback for three flips.
- **The wait-label flip:** `PS3_HLE_ARGS=D8F88E1A` saw no `_cellGcmSetFlipCommandWithWaitLabel` call in these
  runs. What could send the reporter's flips outside the stream is open (section 6).

**Full batch**, tried and not reproduced with the old code:
- **The knobs** (scratch, not committed):
  - one treated n flips as having no display draw (n = 40, 200);
  - one dropped every draw into a display buffer for n frames (n = 6, 12);
  - one cut the draw table to 3000 records.
- **Result:** in the Tutorial and the Palace, the old code rendered again at the first honest flip every time.
  The batch always still held a record that counts as a display draw:
  - In a Palace frame, 462 of about 2,250 records count as display draws, and the first is record 1. That held
    even with draws into the display buffers refused, so those records are classified some other way (not looked
    into).
  - In the Tutorial, a full vertex buffer stops recording vertex-program draws, but the display copy still got
    in, and the next flip rendered and grew the buffer.
- **With the fix:** each full batch was drawn without a present
  (`a batch with no display draw is full (8807 draws, 2682517 of 1572864 vertices)`).

**SPU decrementer wrap** (upstream 7e257f75, also in the fork):
- **The bug:** `SPU_RdDec` computes `elapsed_ns * 79800000`, which overflows 231 s after a context's base stamp.
  The count then jumps by about 15.9 s.
- **Who reads it unwritten:** only the SPURS kernel (image 12) and short-lived image 7 and 10 contexts.
- **Runs:** one with the wrap brought forward (`SPU_DEC_SKEW_S=150`), and one with a jump every 15 s while
  walking. Neither had a stall, a warning or a frozen picture.

## 3. The fixes (ps3recomp#132, `libs/video/rsx_d3d12_backend.c`)

- **`d3d12_fifo_flip` notes the flip request count.**
  - In `d3d12_clear_display`, from three requests past it on, display clears end the frame again, as before
    present-at-flip, until a flip arrives in the stream.
  - In normal play every Demon's Souls flip is in the stream (`flips` equals `flipwords` in `GCM_RATES`), so
    nothing changes.
- **`flip_without_display()`:** at a flip with no display draw, a batch that is full (the draw table, the vertex
  buffer or the legacy buffer) is drawn with `s_present_this_frame = 0`, the ticker's offscreen-only path. The
  next frame then starts empty. Batches that are not full keep accumulating as before (demosaic flips once per
  effect pass).
- **New log lines:**
  - `[D3D12] N flips outside the command stream since the last one in it: presenting at display clears`
  - `[D3D12] flips are in the command stream again`
  - `[D3D12] a batch with no display draw is full (...): drawn without a present`
  - `[D3D12] 300 flips in a row with no display draw (...)`

## 4. Performance

- **Method:** `exeab.sh` at the deep Palace save, plain windows, against fork master 480610f1.
- **Noise:** the PC was not quiet (background CPU 7-21%, noise bar 4.5%), and the base alone moved from 157.0 to
  164.1 fps between the two batches.
- **First commit**, 4 rounds: median +0.06% (157.0 against 156.7 fps), p=0.95.
- **Both commits**, 3 rounds: median -1.15% (164.1 against 162.9 fps), p=0.48, faster in 1 of 3 rounds.
- **Per frame**, the change adds one counter read at each flip and one at each display clear.
- **Cutscene:** the Tutorial's opening cutscene at `DES_FPS=60`, 3 boots a side, first commit against base.
  - no frame over 100 ms on either side;
  - one over 40 ms in a base boot;
  - p99 33.8-33.9 ms on both.
- **Earlier hitches:** the 95-195 ms cutscene hitches in earlier runs came from scratch builds (decrementer
  bookkeeping on every SPU timer read) and a busy PC. They appeared with and without the fix.

## 5. Ruled out or unlikely

- **Lost GPU device (TDR):** the F1 menu presents through the same device and swap chain. On a removed device it
  could not appear, and the screenshot could not be read back.
- **Online:** the reporter's game was offline (the "project" server preset has no address yet). The game's 300 s
  rating poll only runs online.
- **Saves:** they are synchronous on the save thread. A slow write blocks that thread, not the picture.
- **Sysutil dialogs:** no sysutil drawing event is ever queued, and the pickup dialog is part of the game's own
  frame.
- **A full vertex buffer:** see section 2.
- **Counters wrapping:** none wraps within minutes at 240 flips a second.

## 6. If it comes back: what to look for

- **`des.log`:**
  - `[D3D12] frame has more than 16384 draws: the rest are dropped`. It is printed once, since the frame count
    stops: the batch filled without a present, either way above.
  - With ps3recomp#132: `flips outside the command stream`, `a batch with no display draw is full`,
    `300 flips in a row with no display draw`.
  - Around the stall: `FIFO resync`, `FIFO walker circling`, `title-ring recycle`, `[gcm-wedge]`,
    `wait_for_gpu STUCK`, `FENCE STUCK`, `pipeline library saved`, `[RT] all 16`.
- **A run with `GCM_RATES=1`** separates the cases:
  - `[gcm-rates]` comes from `render_frame` and stops when nothing is drawn;
  - `flips=` above `flipwords=` means flips outside the stream.
- **`PS3_HLE_ARGS=D8F88E1A`** prints the first 6 calls of `_cellGcmSetFlipCommandWithWaitLabel`.
- **Open:** what could move Demon's Souls' flips out of the stream for good.
  - Candidates: the wait-label flip, or `gcm_flip_into_fifo` returning 0. That happens when the context's current
    pointer has no IO mapping, or for a second context near its end, since only one context is remembered as
    running past `end`.
  - The 3 s stall right before the freeze may be what changes the game's flip path.

## 7. Upstream ps3recomp since the split (a2c20108)

The fork never merged upstream after the split. Agents compared upstream's non-merge commits since a2c20108
with fork master 480610f1, reading the fork's code where each upstream diff changes it. `git cherry` finds no
upstream patch in the fork.

- **PPU lifter** (12158c45, 65806f3f, and the other lifter commits): no gap affects the current Demon's Souls
  lift.
  - Every instruction in it has a lowering.
  - The ops the fork lacks or gets wrong (about 45 VMX ops, wrong lane order in 13 handlers, the stvlx/stvrx
    stores, LK=1 forms, wrapping rlwinm masks) occur 0 times in the EBOOT.
  - They would matter for a re-lift or another title. Port them one op at a time in the fork's style, not as a
    merge: upstream's CR-fusion pass and inline loads conflict with the fork's CR layout and its own inline loads.
- **SPU and SPURS:** the best lead for frame rate and stability.
  - The 8-slot MFC engine table and the 8-slot lock-line table overflow in nearly every Demon's Souls log:
    `[SPU] more than 8 contexts hold an MFC engine at once` and `[spu-coh] more than 8 reserving SPU contexts`.
    The SPURS task, job and policy paths never call `spu_mfc_release`.
  - Effect on the MFC engine table: every context past slot 8 shares one unlocked engine, and every one of its
    MFC channel accesses takes an exclusive lock.
  - Effect on the lock-line table: contexts past slot 8 get no lock-line wake, so a GETLLAR wait naps up to its
    2 ms safety net.
  - Upstream d0d67442 and 89cc4c5a fix this. Port the release calls and the larger tables, not 89cc4c5a's locked
    lookup (the fork's lock-free one is tuned, docs/28, docs/48).
  - Smaller items:
    - `_cellSpursAttributeInitialize` reads the revision as nSpus (37ea6328);
    - CreateTaskset clears about 152 bytes of a 0x2900-byte Taskset2 (194bb185);
    - the 256-identical-transfers halt has no wall-time guard (39cfbf14).
- **PPU, OS and libraries:**
  - **Worth porting:**
    - `cellFsAioRead` seeks and reads a shared FILE* without a lock (7208d22b);
    - the TLS window holds 31 blocks because blocks are memsz + 0x7000 (088befac: about 248);
    - on Linux, `sys_spinlock_*` does an 8-byte exchange on a 4-byte guest word (67e18b9e).
  - **Fork bugs no upstream commit fixes:**
    - `cellFsFtruncate` always fails with EBADF, because `ppu_fs` owns every fd;
    - `cellFsAioWrite` is not registered;
    - `cellFsUnlink` and `cellFsRmdir` do nothing.
  - **Do not port:** the memory-map change (340a8b4e, fb5354fe, 807e8e14). It would move Demon's Souls'
    allocations next to the fork's TLS window.
- **cellGcm and D3D12:** 0650a590 (the wait-label flip, above) and 7c1d6f12 (a wrapped ring overwriting
  commands the RSX hasn't read) are not in the fork. The rest of this area is still being compared.
