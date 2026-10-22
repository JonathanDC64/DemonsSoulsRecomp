# Image 2's next cut: docs/48 Stage 2 mapped to edits, and the SPU PUT trim

Date: 2026-10-19. Status: static work when written. Since then everything here has merged: the PUT trim as ps3recomp #74, Stage 0 as #78, the bi-switch as #73 (port #83), FIFO_PUT_WAKE as #79, and Stage 2 as ps3recomp #87 with port #98. The landing plans and draft messages below are kept as written.

Bases: ps3recomp `fork/master` = 4a76fc0, port `origin/master` = 53a047a. Line numbers are on those refs unless marked "lift". "Lift" lines are in today's local lift of image 2 (`work_spu/spu_0001_at_0176C200/spu_recomp.c`, generated and git-ignored), which matches master's lifter without `--bi-switch`.

Labels, as in docs/48:
- **Read**: seen in the source, the lift or a doc.
- **Measured**: a number from a run, quoted with its source.
- **Inferred**: a conclusion that still needs a run.

## Short answer

- **Why image 2 now.** Tonight's stack A/B (stackab2, reported to this lane) put image 2 at 92% busy, HighGraphics at 91% and the FIFO thread at 82%. Turning the bi-switch (docs/48 Stage 1) off cost 4.5% (p 0.005). So image 2 holds the frame, and a cut on it shows in frames until HighGraphics saturates.
- **Stage 2** is a second native block in image 2's ring loop, `0x4808-0x4818=spu_hook_ring_post` (section 2).
  - It replaces the call to `0x3C90`, the end-of-command step, when that step would take its fast path: the command left the 32 KB local buffer under its limit, and the command is not 35.
  - It writes the same 14 local-store (LS) quadwords and the same 10 registers as the lifted call, and leaves `ctx->pc`, `drain_ret_pc` and `image_id` as the call bracket does. It makes no channel calls, because the fast path has none.
  - Everything else (the segment switch and command 35's flush) runs lifted.
  - Knob `SPU_NATIVE_POST` (live): 0 lifted (default until its A/B), 1 native, 2 check mode. Check mode lets the lifted call run, then compares its result with the native prediction at the next fetch, before the next command starts.
  - Edits: ps3recomp `runtime/spu/spu_ring_hook.c` (one new function and a one-line check hook in the fetch block); port `scripts/build_spu.py:55` (one table entry); then a re-lift of image 2. No lifter change.
  - **Expected (inferred):** about 8-11 ns off each of ~38,000-42,000 commands a frame, which is 5-6% of image 2's busy time (region `0x3C90` is 7.5-7.7% of image 2 in docs/56's profiles). At tonight's rate for the bi-switch, that is up to +6-8% while image 2 holds the frame. HighGraphics (91%) probably caps it at +3-5%.
- **The PUT trim** (section 3) merged as ps3recomp #74.
  - Every SPU PUT calls `gcm_note_spu_put`, which scans all 16 GCM IO mapping slots (`libs/video/cellGcmSys.c:1338-1349`). Most PUTs land in no mapping, so the scan runs to the end: 16 iterations a call over 698M calls, 1.4% of image 2 (docs/56, the profile-counts table).
  - The patch adds one bit per 1 MB page that any mapping has covered. A clear bit returns "not mapped" without the scan. A set bit scans as before, so every PUT gets the same answer as before.
  - Knob `GCM_SPU_PUT_FILTER` (live): 1 (default) on, 0 off, 2 check.
  - `git apply --cached --check` against `fork/master` passes.
  - **Expected (inferred):** about 1.1-1.3 points of a core off image 2 (about 2.5 ns a command). That is worth at most +1.5-1.9% while image 2 holds the frame, and likely +0.5-1.5% once HighGraphics caps it. That is at the in-boot live-knob bar (about 1-1.5%), so it needs pooled boots.
- **Main risks.**
  - Stage 2: it sits on the only path graphics commands take, so a wrong byte shows as corruption or a hang. Check mode and the prechecks are the guard.
  - PUT trim: a future writer of the mapping list that forgets to mark its pages would make PUTs into that mapping look unmapped. Check mode (`GCM_SPU_PUT_FILTER=2`) reports it.
  - Both: the frame-rate gain may land inside the noise if another thread saturates first.

## 1. What and why

**The numbers (measured).**
- Tonight's stackab2: image 2 92% busy, the FIFO thread 82%, HighGraphics 91%. The bi-switch off cost 4.5% (p 0.005). It had taken about 3 points of a core off image 2 in the earlier in-boot A/Bs (docs/56, the biswf and crbk sections).
- docs/56, prof2 (the deep Palace save, about 110 fps), image 2's hottest entries: waits 10.3%, region `0x4498` 10.1%, region `0x3C90` 7.7%, region `0x8CB8` 7.2%, `mfc_submit` 6.3%, `spu_wrch` 5.1%, `spu_hook_ring_fetch` 4.3%, `mfc_for` 4.2%.
- docs/56, the CR-fields build (image 2 at 89.6% busy): region `0x4498` 9.8%, `0x8CB8` 8.0%, `0x3C90` 7.5%.
- docs/56, the profile-counts table (the PGO run, deep1): `gcm_note_spu_put`'s hottest block ran 11.2e9 times over 698M calls, 16 a call, 1.4% of image 2.
- docs/56, the causal slopes: `TMP_SLOW_RING_NS=25` (25 ns added to each of 38,456 commands a frame) cost 14.7% of the frame rate, slope 1.51 for image 2.

**What the two changes take away.**
- Stage 2 removes most of region `0x3C90` and its call bracket from every command except command 35 and the rare segment switch.
- The PUT trim removes the mapping scan from every SPU PUT that lands outside the IO-mapped pages. Image 2 makes at least one such PUT per command: the state PUT that publishes get (docs/48 section 2.3).

**How a saving reaches the frame (inferred).** The bi-switch's ~3 points of a core (about 6 ns per command at ~4.6 million commands a second) bought 4.5%, so about 0.75% of frame rate per ns per command at tonight's balance. The 25 ns slope run gives about 0.6% per ns. A cut is worth at most that rate, and less once HighGraphics (91%) or the FIFO thread (82%) saturates.

## 2. Stage 2: 0x3C90's fast path in C

### 2.1 Where it hooks, and how control passes (read)

- **The table entry.** Port `scripts/build_spu.py:51-56` (`NATIVE_BLOCKS`). Image 2's line 55 becomes:

  ```python
  "spu_0001_at_0176C200": (0x4FBCAB40231BF72B, ["0x4618-0x4730=spu_hook_ring_fetch",
                                                "0x4808-0x4818=spu_hook_ring_post"]),
  ```

  The fingerprint check stays (`build_spu.py:125-126`), so another build of the game lifts without the block.
- **What the lifter emits** (`tools/spu_lifter.py`, no change needed):
  - Lines 663-671 make 0x4808 and 0x4818 labels in region 0x4498's body. They stop the lift if 0x4818 were outside that body; it is inside (the loop test, lift line 1782).
  - Lines 691-693 emit, right after `loc_00004808:` and before the first lifted instruction:

    ```c
    if (spu_hook_ring_post(ctx)) goto loc_00004818;
    ```

  - Line 1211 declares `int spu_hook_ring_post(spu_context* ctx);` above the region.
- **The block covers four instructions** (lift lines 1777-1781): `andi r5,r5,255` at 0x4808, `ila r3,0x13340` at 0x480C, `ila r4,0x13380` at 0x4810, and `brsl r0,0x3C90` at 0x4814 with its call bracket.
- **How the native code gets control.** Every command reaches 0x4808:
  - the handlers end with `SPU_RGOTO(ctx, 0x4808u, ...)`, 26 sites in region 0x4498: the inline handlers (for example command 6's at 0x4920) and the out-of-line handlers after their calls (docs/48 section 2.3);
  - command 23 (its table entry is 0x4804), command bytes outside 1-37 (the range test at lift line 1765) and command 36 (0x47F0, which falls through after its call) reach 0x4804 (`r81 = r83 & 255`), which falls through to 0x4808 (lift lines 1773-1777);
  - a trampoline arrival enters the region at its entry case for 0x4808 (lift line 2588), which jumps to the same label.
- **How it gives control back.**
  - It returns 1: the lifted code goes on at 0x4818, the loop test `brz r81, 0x4618` (lift line 1782).
  - It returns 0: the four instructions run as lifted, including the call.
- The block's contract is the one in `runtime/spu/spu_ring_hook.c:5-9`: on 1, every register, LS byte and channel the instructions write is left as they leave it.

### 2.2 Where it meets the bi-switch (read, inferred)

- The bi-switch was not on `fork/master` when this was written; it merged later as ps3recomp #73 and port #83. On master's lift, the dispatch at 0x4758 is the trampoline line at lift line 1766.
- Switched, that `bi` becomes a C `switch` on the target with one `SPU_RGOTO` per handler, ahead of the old trampoline line (docs/48 section 5, Stage 1). Every case lands on a handler label in region 0x4498, and every handler ends on `loc_00004808`, as listed in 2.1.
- So the two meet at `loc_00004808`. With both on, an inline command runs from the fetch block (0x4618) to the loop test (0x4818) inside one call of the region function, with gotos only. The host calls left per inline command are the put_wait hook at 0x4730 and this block.
- Neither depends on the other: `SPU_BI_SWITCH=0` brings back the trampoline hop to the handler, and the handler still reaches the block. Stage 2 needs no switch entry, and the switch needs no change for Stage 2.
- Stage 2's saving adds to the switch's. The switch removes the dispatch hop; Stage 2 removes the end-of-command call.

### 2.3 What the lifted step does on its fast path (read, lift lines 962-1017)

1. **Prologue.** It stores 13 quadwords. Each store writes the register's value at entry, so the slot contents do not depend on the order:

   | slot | value | slot | value |
   |---|---|---|---|
   | sp - 0x20 | r81 | sp - 0x80 | r87 |
   | sp - 0x60 | r85 | sp - 0x90 | r88 |
   | sp - 0x10 | r80 | sp + 0x10 | r0 = link 0x4818 |
   | sp - 0x30 | r82 | sp - 0x70 | r86 |
   | sp - 0x40 | r83 | sp - 0xA0 | r89 |
   | sp - 0x50 | r84 | sp - 0xB0 | r90 |
   | | | sp - 0x100 | r1 |

   Then r1 -= 256. In between, it overwrites r81, r85, r82, r83, r84 and r87 with constants and copies.
2. **Descriptor.** It reads the quadword at 0x13380, {base, limit, cur, 0}, twice (through 0x13388 and 0x13384). It leaves r3 = {cur, 0, base, limit} and r2 = {limit, cur, 0, base}, then r4 = `clgt(r2, r3)`.
3. **Tests.** If word 0 of r4 is 0 (limit <= cur, unsigned), it goes to the slow path at 0x3D68. It sets r8 = `ceqi(r5, 35)`, and if word 0 is set, it goes to 0x4130 (command 35). Neither branch is taken on the fast path, so no `SPU_RGOTO` runs and `ctx->steps` does not change.
4. **0x3D18.** r29 = the quadword at 0x13350, r30 = `cwd(r1, 12)` with r1 = sp - 256 (word slot 3), r1 += 256. It stores r29 with word 3 replaced by cur at 0x13350, so LS[0x1335C] = cur.
5. **Epilogue.** It reloads r0 and r80-r90 from their slots and returns (`SPU_RET`, which sets `ctx->pc = 0x4818`; `spu_context.h:1012-1017`).
6. **The bracket's end** (`spu_call_done`, `spu_context.h:897-905`). No transfer is pending and pc equals the return address, so it only sets `drain_ret_pc = 0`, undoes its `host_depth++`, and sets `image_id = module_img_a00 ? module_img_a00 : <image_id before the call>`.

### 2.4 Guest state read and written

| | What |
|---|---|
| LS read | 0x13380 (the descriptor), 0x13350 (the segment quadword). The epilogue reads the 12 slots back; with the frame check below they hold what the prologue stored. |
| LS written | The 13 slots of 2.3, then 0x13350 with word 3 = cur. Nothing else. |
| Registers read | r1 (sp), r5 (the command quadword from 0x13560), r80-r90 (for the slots) |
| Registers written | r0 = link 0x4818; r2 = {limit, cur, 0, base}; r3 = {cur, 0, base, limit}; r4 = `clgt(r2, r3)`; r5 = `andi(r5, 255)` (every word); r6 = 0x13384 in every word; r7 = the descriptor; r8 = `ceqi(r5, 35)`; r29 = the old 0x13350 quadword; r30 = the `cwd` mask for word 3 |
| Unchanged | r1, r80-r127, `host_depth`, `steps`, the event mask and status, the MFC state, the reservation |
| `ctx` fields written | `pc = 0x4818`, `drain_ret_pc = 0`, `image_id` by the rule above |
| Channel operations | none (the fast path has no `rdch`/`wrch`; the slow path's PUTs, eieio, put register write and label wait stay lifted) |

### 2.5 Which commands run natively, and the fallbacks

**Natively:** the end-of-command step of every command except command 35, while the local buffer is under its limit. That is commands 1-34, 36 and 37, and out-of-range command bytes (through 0x4804). Stage 0's semaphore delta gives the share of commands that close a segment (2.11, item 2). It is expected to be about one in a few thousand.

**Fallbacks.** The block returns 0, with nothing written, when any of these holds:
1. `SPU_NATIVE_POST` is 0. In check mode (2) it also returns 0, after it records its prediction. It also returns 0 after this thread saw a check mismatch.
2. `g_spu_trampoline_fn` is set. A pending transfer is not expected at 0x4808.
3. `g_spu_ls_dbg` is not 0: an LS watch or probe is on (`spu_context.h:524`; the LS accessors test it at 616), and the probes should see the lifted order.
4. The frame does not fit:
   - sp is not 16-byte aligned;
   - sp < 0x100, or sp + 0x20 > 256 KB;
   - [sp - 0x100, sp + 0x20) overlaps 0x13350-0x1338F, the span that holds both quadwords the step touches (0x13350 and 0x13380).

   With the frame clear of both quadwords, the native order (read, test, write) gives the same bytes as the lifted order (write, read, write).
5. limit <= cur (unsigned): the slow path, the segment switch.
6. Word 0 of `andi(r5, 255)` is 35: command 35's flush at 0x4130.

**Cases that need no fallback (read):**
- `g_spu_goto_slow`, lockstep, PM flow tracing and interrupts: the fast path takes no branch, so the lifted step does no per-step work either. Image 2 never enables interrupts (docs/48 section 5).
- Channel diagnostics: the fast path makes no channel call.

### 2.6 The code (append to `runtime/spu/spu_ring_hook.c`)

A header paragraph goes after master line 33, in the style of the `SPU_NATIVE_RING` one: what 0x3C90 does on its fast path, and the knob's three values.

```c
#define RING_SEG_LS   0x13350u   /* w0 semaphore, w1 count, w2 index, w3 output start */
#define RING_DESC_LS  0x13380u   /* the local buffer {base, limit, cur, 0} */
#define RING_POST_END 0x4818u    /* the loop test after the call to 0x3C90 */

static int native_post_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_POST", 0));
    return on;
}

/* 0x3C90's prologue stores, in its order: the slot's offset from sp, and the
 * register stored there (0 = the link the brsl at 0x4814 sets). */
static const int16_t k_post_slot[13] = { -0x20, -0x60, -0x10, -0x30, -0x40, -0x50,
                                         -0x80, -0x90, 0x10, -0x70, -0xA0, -0xB0, -0x100 };
static const uint8_t k_post_reg[13]  = { 81, 85, 80, 82, 83, 84, 87, 88, 0, 86, 89, 90, 1 };

typedef struct ring_post {
    uint32_t sp;
    u128 slot[13];               /* what each prologue store writes */
    u128 seg;                    /* 0x13350 after the store of cur */
    u128 r0, r2, r3, r4, r5, r6, r7, r8, r29, r30;
    u128 keep[11];               /* r80-r90: they come back unchanged */
    int32_t image_id;            /* spu_call_done's rule */
} ring_post;

/* What 0x4808-0x4818 leaves when 0x3C90 takes its fast path. 0 when it would
 * not, or when the layout is not the one this was written for. Writes nothing. */
static int ring_post_predict(const spu_context* ctx, ring_post* p)
{
    const u128* r = ctx->gpr;
    const uint32_t sp = r[1]._u32[0];
    if ((sp & 15) || sp < 0x100u || sp > SPU_LS_SIZE - 0x20u) return 0;
    /* The frame must miss both quadwords the step reads and writes, so the
     * order of its accesses cannot matter. */
    if (sp - 0x100u < RING_DESC_LS + 16u && sp + 0x20u > RING_SEG_LS) return 0;
    const u128 d  = spu_ls_read128(ctx, RING_DESC_LS);
    const u128 r5 = spu_andi(r[5], 255);                          /* 0x4808 */
    if (!(d._u32[1] > d._u32[2]) || r5._u32[0] == 35u) return 0;  /* switch, or 35 */
    p->sp = sp;
    p->r0 = spu_link(RING_POST_END);
    for (int k = 0; k < 13; k++)
        p->slot[k] = k_post_reg[k] ? r[k_post_reg[k]] : p->r0;
    p->r5  = r5;
    p->r2  = spu_rotqbyi(d, 4);                                   /* {limit, cur, 0, base} */
    p->r3  = spu_rotqbyi(d, 8);                                   /* {cur, 0, base, limit} */
    p->r4  = spu_clgt(p->r2, p->r3);
    p->r6  = spu_ila(RING_DESC_LS + 4u);
    p->r7  = d;
    p->r8  = spu_ceqi(r5, 35);
    p->r29 = spu_ls_read128(ctx, RING_SEG_LS);
    p->r30 = spu_cwd(spu_ai(r[1], -256), 12);
    p->seg = p->r29;
    p->seg._u32[3] = d._u32[2];                                   /* LS[0x1335C] = cur */
    for (int k = 0; k < 11; k++) p->keep[k] = r[80 + k];
    p->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : ctx->image_id;
    return 1;
}

static void ring_post_apply(spu_context* ctx, const ring_post* p)
{
    u128* r = ctx->gpr;
    for (int k = 0; k < 13; k++)
        spu_ls_write128(ctx, p->sp + (uint32_t)(int32_t)k_post_slot[k], p->slot[k]);
    spu_ls_write128(ctx, RING_SEG_LS, p->seg);
    r[0] = p->r0;  r[2] = p->r2;  r[3] = p->r3;  r[4] = p->r4;  r[5] = p->r5;
    r[6] = p->r6;  r[7] = p->r7;  r[8] = p->r8;  r[29] = p->r29; r[30] = p->r30;
    /* What the call bracket leaves after a plain return (spu_call_done). */
    ctx->pc = RING_POST_END;
    ctx->drain_ret_pc = 0;
    ctx->image_id = p->image_id;
}

int spu_hook_ring_post(spu_context* ctx)
{
    const int mode = native_post_mode();
    if (!mode || g_spu_trampoline_fn || g_spu_ls_dbg) return 0;
    ring_post p;
    if (!ring_post_predict(ctx, &p)) return 0;
    if (mode == 2) {                       /* check: the lifted call runs */
        ring_post_record(ctx, &p);
        return 0;
    }
    if (s_post_bad) return 0;              /* this thread saw a mismatch */
    ring_post_apply(ctx, &p);
    return 1;
}
```

Notes on the code:
- `spu_andi`, `spu_rotqbyi`, `spu_clgt`, `spu_ceqi`, `spu_ila`, `spu_cwd` and `spu_link` are the runtime's helpers (`spu_helpers.h:149, 297/300, 164, 167, 722, 666, 84`), the same ones the lift calls, so the register values match bit for bit.
- `_u32[3]` is word 3, as in the fetch block's `old_line._u32[3]` (`spu_ring_hook.c:76`).
- The struct is about 420 bytes on the host stack. If the profile shows the copy, mode 1 can write straight from the registers and skip the struct; the prediction is only needed for mode 2.
- `g_spu_ls_dbg` is -1 until something resolves it. The block bails on -1, and the lifted code's first LS access resolves it.

### 2.7 Check mode: `SPU_NATIVE_POST=2`

The point is to run both and compare, with the lifted result kept, so a mismatch is caught before it costs a frame.

- **At 0x4808** the block predicts as above. If `r81` word 0 is 0, the loop will go on to 0x4618 (the step leaves r81 unchanged). It then saves the prediction in a thread-local record (`SPU_THREAD_LOCAL`, `spu_context.h:844-846`, as Stage 0's counters do), sets a global `g_post_checking = 1`, and returns 0. The lifted call runs and its result stands. When r81 is not 0 (command 3: the loop ends), nothing is recorded.
- **At the next 0x4618** nothing has run since 0x4818 except the `brz` (a goto, or one trampoline hop with nothing on this context in between). There, the first line of `spu_hook_ring_fetch` compares:

  ```c
  if (g_post_checking) ring_post_check_pending(ctx);   /* SPU_NATIVE_POST=2 */
  ```

  The line goes before the knob test at master line 66, so it runs with `SPU_NATIVE_RING=0` too. `ring_post_check_pending` returns at once when its thread has no record. It is declared near the top of the file, with `g_post_checking`, and defined with the Stage 2 code.
- **The pieces** (near the top of the file: `g_post_checking` and the prototype; the rest with the Stage 2 code):

  ```c
  static volatile int g_post_checking;                  /* set once mode 2 records */
  static void ring_post_check_pending(spu_context* ctx);

  SPU_THREAD_LOCAL static ring_post s_post;             /* the prediction to check */
  SPU_THREAD_LOCAL static int s_post_pending, s_post_bad;
  SPU_THREAD_LOCAL static unsigned long long s_post_n, s_post_miss;

  static void ring_post_record(const spu_context* ctx, const ring_post* p)
  {
      if (ctx->gpr[81]._u32[0] != 0) return;            /* the loop ends: no next fetch */
      s_post = *p;
      s_post_pending = 1;
      g_post_checking = 1;
  }

  static void ring_post_check_pending(spu_context* ctx) /* at 0x4618 */
  {
      if (!s_post_pending) return;
      s_post_pending = 0;
      /* Compare the fields listed below; on the first difference print one
       * [ring-post] line (8 at most per thread), set s_post_bad and stop. */
  }
  ```

- **What it compares:**
  - the 13 slots at the recorded sp;
  - the 0x13350 quadword;
  - r0, r2, r3, r4, r5, r6, r7, r8, r29 and r30;
  - r1 against the recorded sp;
  - r80-r90 against `keep`;
  - `image_id` against the rule;
  - `drain_ret_pc == 0`.

  The comparison uses `ctx->ls` directly, so the LS probes do not see it.
- **On a mismatch** it prints up to 8 lines: `[ring-post] spu=<id> mismatch: <field> lifted <hex> native <hex> (cmd <n>, sp 0x<sp>)`. Then it sets the thread's `s_post_bad`, so mode 1 stays lifted on that thread for the rest of the session.
- **Every 2^20 checks** it prints `[ring-post] spu=<id> <n> checked, <m> mismatches, <b> bails (frame <f>, switch <s>, cmd35 <c>)`, which counts how often each fallback fires. For that, `ring_post_predict` returns its reason (a small enum instead of 0) and mode 2 counts it in thread-locals next to `s_post_n`.
- **Order in the file:** the pieces above go before `spu_hook_ring_post`, which uses `ring_post_record` and `s_post_bad`.
- **Cost:** mode 2 is for correctness runs, not frame rate. Modes 0 and 1 pay one load of `g_post_checking` per fetch.
- **Not covered by check mode:** bytes outside the write set. The fast path's write set is read from the lift (lines 962-1017); the native code writes nothing else. A full-LS compare every n-th command could be a mode 3 if it is wanted.
- **Conflict with Stage 0.** Stage 0 (`SPU_RING_HIST`, since merged as ps3recomp #78) renames master's `spu_hook_ring_fetch` to `ring_fetch` and adds a wrapper. If Stage 0 lands first, the check line goes at the top of its wrapper. Either way the two PRs touch the same lines, so land one, then rebase the other on the new master (still not stacked).

### 2.8 The knob

`SPU_NATIVE_POST` (live, `PS3_KNOB` + `ps3_env_int`, refreshed on `ps3_env_gen` like `SPU_NATIVE_RING` at `spu_ring_hook.c:52-57`):
- 0 (default until its A/B is in): the lifted call;
- 1: the native fast path;
- 2: check mode (2.7).

`SPU_NATIVE_RING=0` does not turn Stage 2 off; they are separate blocks.

### 2.9 The steps, in order

1. ps3recomp, branch `perf/spu-ring-post` from `fork/master` (or from the new master once Stage 0 is in): `runtime/spu/spu_ring_hook.c`, with the header paragraph, the code of 2.6, the check of 2.7 and the one-line hook in the fetch block. The file is already in the runtime's build, so there is no reconfigure.
2. Port, branch `perf/spu-ring-post`: `scripts/build_spu.py:55` as in 2.1. It needs the ps3recomp change, or the link fails on `spu_hook_ring_post`. Merge the ps3recomp PR first.
3. Re-lift the SPU images (`scripts/build_spu.py`, about 17 s per docs/56), rebuild the runtime and link the port. The PPU chunks do not rebuild.
4. **First run, check mode:** `SPU_NATIVE_POST=2` in the Palace and the Nexus. Expect `0 mismatches`, and see the bail counts.
5. Then `regress.sh` and the soaks with `SPU_NATIVE_POST=1`, then the in-boot A/B (section 5).

### 2.10 Expected gain (inferred, from the profile)

- **Per command.** Region `0x3C90` is 7.5-7.7% of image 2's samples (docs/56). At about 89% busy and about 4.6 million commands a second (38,456 a frame at about 120 fps), that is about 14-15 ns a command. The call bracket in region 0x4498 adds 1-2 ns.
  - The native step is about 60-80 host instructions: 13 byte-swapped quadword stores, two loads, a few vector helpers. That is about 4-6 ns.
  - Saving: about 8-11 ns a command, 5-6% of image 2's busy time, 4.5-5.5 points of a core.
- **Frame rate.**
  - At tonight's bi-switch rate (about 0.75% a ns a command), up to +6-8%.
  - At the 25 ns slope's rate (about 0.6%), up to +5-6.5%.
  - HighGraphics at 91% busy, with part of that in its `usleep` reply poll, and the FIFO thread at 82% cap it below that. Likely +3-5%.
- **Against the bars:** the in-boot live-knob A/B resolves about 1-1.5%, so this should clear it. A cross-build A/B (2.8% profile fps, 4.5% gap fps) might clear the profile bar.
- **CPU:** `knobcpu.py` should show image 2 down by 4-5 points with the knob on.

### 2.11 Open questions

1. **The frame check.** docs/48 says r1 is set before the loop and never changes, and the stack is expected near the top of LS (inferred). Check mode's bail counts will show whether the frame check ever fires.
2. **How often the slow path runs.** Stage 0's semaphore delta per taken command answers it (its `[ring-hist]` interval line prints the segment switches and the semaphore delta). If segment switches were frequent, the gain would shrink by their share.
3. **The cost of the 13 stores.** docs/48 Stage 3 notes that 12 of the slots hold the same values on every command. A later version could write only the slot for r80 and LS[0x1335C] when they change, but only inside a loop that owns all the exits (Stage 3). For a single block, the stores stay.
4. **Whether a mode 3 (a full-LS compare every n-th command) is wanted.** It is not needed for the fast path's write set, which is read from the lift.
5. **The split with the bi-switch and the PUT trim.** All three cut image 2. Measure them one at a time, with the others on, as tonight's stack did.

## 3. The SPU PUT trim (ready patch)

### 3.1 What and why (read)

- **The PUT path.** Each SPU PUT, after its copy and its coherence notify, calls `g_spu_put_hook(ea, size)` (`runtime/spu/spu_dma.h:818-819`, and `1145-1146` for the second PUT path; the pointer is defined at `runtime/spu/spu_channels.c:157`).
- `cellGcmInit` installs `gcm_note_spu_put` as the hook (`libs/video/cellGcmSys.c:506-514`). It records SPU-built pushbuffers for titles whose SPUs write the RSX stream (comment at `cellGcmSys.c:1317-1333`).
- `gcm_note_spu_put` (`cellGcmSys.c:1357-1374`) calls `gcm_ea2io_range` (`1338-1349`). That function scans the 16 slots of `s_io_mappings` (`188`) in order: it skips inactive or empty slots and returns the first slot that holds ea.
- **Measured (docs/56, the profile-counts table):** 16 iterations a call over 698M calls, 1.4% of image 2. Nearly every call scans all 16 slots, so nearly every PUT is outside every mapping. Image 2's state PUT to the ring's control block (EA 0x01B51000) is one of them, once per command.

### 3.2 The exact edits (`libs/video/cellGcmSys.c`, master lines)

- **After 189** (the mapping list):
  - the comment block;
  - `s_io_pages`: 64 words of 64 bits, one bit per 1 MB page of the 4 GiB space (512 bytes), as C11 atomics;
  - `gcm_io_pages_mark(ea, size)`: a release OR for each page in [ea, ea + size);
  - `gcm_io_pages_clear()`;
  - `gcm_io_page_maybe(ea)`: an acquire load and a bit test;
  - `gcm_put_filter_mode()`: the knob;
  - `gcm_put_filter_miss()`: the check mode's report, capped at 8 lines.

  C11 atomics already reach this file on every platform: it includes `runtime/ppu/ppu_memory.h` (line 16), which includes `<stdatomic.h>` unconditionally (its line 19) and uses `_Atomic(uint64_t)` with `atomic_load_explicit` itself (350-351). This file calls `atomic_thread_fence` outside any `#ifdef` (1694).
- **1337-1342** (`gcm_ea2io_range`): with the knob at 1 and the page bit clear, return `0xFFFFFFFF` before the scan. With the knob at 2, scan anyway and report a mapped PUT whose bit is clear.
- **Writers that add a mapping mark its pages after the slot's stores:**
  - `cellGcmInit`'s slot 0 (`494-498`);
  - the `AddressToOffset` auto-map (`2973-2974`);
  - `cellGcmMapMainMemory` (`3045-3050`);
  - `cellGcmMapEaIoAddress` (`3084-3089`);
  - `cellGcm_syscall_iomap` (`3880-3884`);
  - `cellGcm_replay_load`, after its loop restores the list (`4158`, `4171`).
- **Writers that clear the list clear the bits after it:** `cellGcmInit` (`445`) and `cellGcmTerminate` (`3536`).
- **Unmaps** (`3106`, `3122`) leave the bits set; each gets a short comment. `cellGcm_syscall_iounmap` (`3889-3895`) never touches the list, so nothing changes there.
- These are all the writers of `s_io_mappings`: it is file-static, and a search for every slot write (`s_io_mappings[`, `->active`, `find_free_mapping`) finds only these sites.

### 3.3 The patch

`spu_put_io.patch`. `git apply --cached --check` on a temporary index read from `fork/master` passes. The blob and the patch are LF.

```diff
diff --git a/libs/video/cellGcmSys.c b/libs/video/cellGcmSys.c
index 7fffef6..a6f3f5d 100644
--- a/libs/video/cellGcmSys.c
+++ b/libs/video/cellGcmSys.c
@@ -188,6 +188,69 @@ typedef struct IoMapping {
 static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
 static int s_io_mapping_count = 0;
 
+/* Every SPU PUT asks the mapping list whether it landed in RSX-visible memory
+ * (gcm_note_spu_put), and most land in no mapping at all, so the scan ran all
+ * 16 slots for nothing: 1.4% of Demon's Souls' busiest SPU thread. One bit per
+ * 1MB page answers those at once. A mapping's pages are marked after its slot
+ * is written, and stay marked when it is unmapped, so the bits are always a
+ * superset of what the list maps: a clear bit means the scan would find
+ * nothing. Only cellGcmInit and cellGcmTerminate clear them, after the list.
+ * Every writer of s_io_mappings marks the pages it adds (gcm_io_pages_mark).
+ *
+ * Threads: the mapping calls run on PPU threads while SPU threads read. The
+ * mark is a release OR after the slot's stores and the test an acquire load,
+ * so a reader that sees a bit sees the slot behind it. A PUT that races a map
+ * call may miss the new mapping, as the scan alone could; a PUT ordered after
+ * the call cannot, because a bit only goes from 0 to 1 until the list itself
+ * is cleared.
+ *
+ * GCM_SPU_PUT_FILTER (live): 1 (default) as above; 0 scans every mapping for
+ * every PUT, as before; 2 scans every PUT and reports a mapped PUT whose page
+ * bit is clear (a writer that does not mark; a PUT racing a map call can
+ * also show once). */
+#define GCM_IO_PAGE_WORDS (4096u / 64u)      /* 4 GiB of 1MB pages */
+static _Atomic(u64) s_io_pages[GCM_IO_PAGE_WORDS];
+
+static void gcm_io_pages_mark(u32 ea, u32 size)
+{
+    if (!size) return;                       /* the scan skips these */
+    /* A range past 4 GiB wraps in the scan's u32 sum and never matches there;
+     * marking up to the last page is only generous. */
+    const u64 end  = (u64)ea + size;
+    const u32 last = end > 0x100000000ull ? 4095u : (u32)((end - 1) >> 20);
+    for (u32 p = ea >> 20; p <= last; p++)
+        atomic_fetch_or_explicit(&s_io_pages[p >> 6], 1ull << (p & 63), memory_order_release);
+}
+
+static void gcm_io_pages_clear(void)
+{
+    for (u32 w = 0; w < GCM_IO_PAGE_WORDS; w++)
+        atomic_store_explicit(&s_io_pages[w], 0, memory_order_relaxed);
+}
+
+static int gcm_io_page_maybe(u32 ea)
+{
+    const u32 p = ea >> 20;
+    return (int)((atomic_load_explicit(&s_io_pages[p >> 6], memory_order_acquire) >> (p & 63)) & 1u);
+}
+
+static int gcm_put_filter_mode(void)
+{
+    static int mode = -1;
+    PS3_KNOB(mode, ps3_env_int("GCM_SPU_PUT_FILTER", 1));
+    return mode;
+}
+
+static void gcm_put_filter_miss(u32 ea, int slot)
+{
+    static int n = 0;
+    if (n >= 8) return;
+    n++;
+    printf("[cellGcmSys] GCM_SPU_PUT_FILTER: PUT to ea 0x%08X is in mapping %d "
+           "(ea 0x%08X, size 0x%X) but its page bit is clear\n",
+           ea, slot, s_io_mappings[slot].ea, s_io_mappings[slot].size);
+}
+
 /* Callback handlers — these are GUEST OPD addresses passed by the
  * recompiled game, not host function pointers. Stored as uint32_t and
  * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */
@@ -443,6 +506,7 @@ s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
     memset(&s_config, 0, sizeof(s_config));
     memset(&s_control, 0, sizeof(s_control));
     memset(s_io_mappings, 0, sizeof(s_io_mappings));
+    gcm_io_pages_clear();                    /* after the list (s_io_pages) */
     memset(s_tiles, 0, sizeof(s_tiles));
     memset(s_zcull, 0, sizeof(s_zcull));
     memset(s_report_data, 0, sizeof(s_report_data));
@@ -496,6 +560,7 @@ s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
         s_io_mappings[0].size   = ioSize;
         s_io_mappings[0].active = 1;
         s_io_mapping_count = 1;
+        gcm_io_pages_mark(ioAddress, ioSize);
     }
 
     /* Zero the guest-visible control register (put/get/ref). */
@@ -1334,13 +1399,18 @@ static void gcm_ref_push_at(u32 v, u32 getoff);
 static struct { u32 io_start, io_end; } s_spu_blk[GCM_SPU_BLK_N];
 static int s_spu_blk_w = 0;
 
-/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact). */
+/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact).
+ * A page no mapping has touched answers at once (s_io_pages). */
 static u32 gcm_ea2io_range(u32 ea, u32* room)
 {
+    const int filter = gcm_put_filter_mode();
+    const int maybe  = filter ? gcm_io_page_maybe(ea) : 1;
+    if (!maybe && filter != 2) return 0xFFFFFFFFu;
     for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
         IoMapping* m = &s_io_mappings[i];
         if (!m->active || !m->size) continue;
         if (ea >= m->ea && ea < m->ea + m->size) {
+            if (!maybe) gcm_put_filter_miss(ea, i);
             if (room) *room = m->ea + m->size - ea;
             return m->io + (ea - m->ea);
         }
@@ -2972,6 +3042,7 @@ s32 cellGcmAddressToOffset(u32 address, u32* offset)
         if (m) {
             m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
             s_io_mapping_count++;
+            gcm_io_pages_mark(ea_page, 0x100000u);
         } else {
             printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                    "the FIFO will not be able to walk io 0x%08X\n", io);
@@ -3048,6 +3119,7 @@ s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
     mapping->size   = size;
     mapping->active = 1;
     s_io_mapping_count++;
+    gcm_io_pages_mark(ea, size);
 
     populate_offset_table(ea, io_offset, size);
 
@@ -3087,6 +3159,7 @@ s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
     mapping->size   = size;
     mapping->active = 1;
     s_io_mapping_count++;
+    gcm_io_pages_mark(ea, size);
 
     populate_offset_table(ea, io, size);
 
@@ -3103,7 +3176,7 @@ s32 cellGcmUnmapEaIoAddress(u32 ea)
     printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);
 
     clear_offset_table(mapping->ea, mapping->io, mapping->size);
-    mapping->active = 0;
+    mapping->active = 0;                     /* its page bits stay set (s_io_pages) */
     s_io_mapping_count--;
 
     return CELL_OK;
@@ -3119,7 +3192,7 @@ s32 cellGcmUnmapIoAddress(u32 io)
     printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);
 
     clear_offset_table(mapping->ea, mapping->io, mapping->size);
-    mapping->active = 0;
+    mapping->active = 0;                     /* its page bits stay set (s_io_pages) */
     s_io_mapping_count--;
 
     return CELL_OK;
@@ -3534,6 +3607,7 @@ void cellGcmTerminate(void)
     memset(&s_config, 0, sizeof(s_config));
     memset(&s_control, 0, sizeof(s_control));
     memset(s_io_mappings, 0, sizeof(s_io_mappings));
+    gcm_io_pages_clear();                    /* after the list (s_io_pages) */
     memset(s_tiles, 0, sizeof(s_tiles));
     memset(s_zcull, 0, sizeof(s_zcull));
     memset(s_report_data, 0, sizeof(s_report_data));
@@ -3882,6 +3956,7 @@ void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
         s_io_mappings[s_io_mapping_count].size   = size;
         s_io_mappings[s_io_mapping_count].active = 1;
         s_io_mapping_count++;
+        gcm_io_pages_mark(ea, size);
     }
     if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
 }
@@ -4169,6 +4244,8 @@ int cellGcm_replay_load(const char* file, u32** methods)
         free(b);
     }
     fclose(f);
+    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++)   /* the restored list (s_io_pages) */
+        if (s_io_mappings[i].active) gcm_io_pages_mark(s_io_mappings[i].ea, s_io_mappings[i].size);
     s_rcap_state0 = s_state;
     s_inited = 1;
     s_gcm_initialized = 1;
```

### 3.4 Why the answer is the same for every PUT

- **The bits are a superset.** A slot's pages are marked after it becomes active, and no bit is cleared while the list holds anything. The only clears follow a full clear of the list. So for any ea that the scan maps, ea's page bit is set once the mapping call has finished.
- **A clear bit gives the scan's answer.** A clear bit means no mapping covers ea's page, so the scan would return `0xFFFFFFFF`. A set bit runs the unchanged scan, so the answer is the scan's.
- **Edge cases follow the scan:**
  - Size 0 is skipped by both.
  - A range that passes 4 GiB never matches in the scan, because its u32 sum wraps. Marking it to page 4095 only makes the filter scan more often.
  - Partial pages are marked whole, so a filter that errs can only err towards scanning.

### 3.5 Memory ordering (mappings that change while SPU threads run)

- **Writers.** The mapping calls run on PPU threads (HLE calls, the sys_rsx syscalls) or at load (the replay). Each writes the slot's fields with plain stores, as today, then marks with `atomic_fetch_or_explicit(..., memory_order_release)`.
- **Readers.** SPU host threads, in `gcm_note_spu_put`, load the word with `memory_order_acquire`.
- **A PUT that sees the bit set** synchronizes with the release that set it, so its scan sees the slot's stores. It does not see a half-written slot, which the plain scan alone could see in theory.
- **A PUT ordered after the map call**, through any chain of happens-before (the guest's own signal to its SPU), must see the bit set. A load that happens after a store reads that store or a later one, and each bit only goes from 0 to 1. So that PUT is never turned away.
- **A PUT that races the map call**, with no order between them, may see the bit clear and get "not mapped". That is the answer it would get had it run just before the call, which the scan alone could also give. The `[cellGcmSys]` check line can show once in that case.
- **Unmaps** leave the bits set, so a PUT that races an unmap scans, exactly as today.
- **Clears** (`cellGcmInit`, `cellGcmTerminate`) happen after the list's memset, with relaxed stores. A reader that sees a cleared bit gets "not mapped", which the cleared list would give too. A reader that sees a stale bit scans the list, as today. The marks that follow in `cellGcmInit` come after the clears in program order, so they stay set.
- **Two writers at once.** Concurrent mapping calls already race on the list itself (`find_free_mapping` has no lock). The bits add no new race: ORs commute, and a slot's bits are always set by the thread that wrote the slot.
- **Cost.** A miss costs the knob check (one load of `ps3_env_gen` and a compare), one load and a bit test. A hit costs the same, then the unchanged scan.

### 3.6 Independence from the FIFO-walker wake in `gcm_note_spu_put`

- The wake that became FIFO_PUT_WAKE (ps3recomp #79; then branch `perf/fifo-put-wake`, cc0ee92). In `gcm_note_spu_put` it adds a forward declaration and, before the `gcm_ea2io_range` call, a test for a PUT that covers the put register at `GCM_CONTROL_GUEST_ADDR` (`VM_HLE_INJECT_BASE + 0x2000`, master line 321).
- This patch does not touch `gcm_note_spu_put` (1357-1374) or `gcm_forget_spu_blocks` (1351-1355). Its nearest hunk ends at master line 1346, inside `gcm_ea2io_range`. The wake's first hunk starts at line 1354.
- Checked statically: this patch applies to cc0ee92's tree (`git apply --cached --check`), and cc0ee92's diff applies to the patched file. So the two merge in either order.
- Neither changes what the other does. The wake tests the put register before the mapping lookup, so it sees that PUT whatever the lookup answers. That address is normally not IO-mapped, so the lookup then answers "not mapped", before and after this patch alike. With the trim, that answer usually comes from one bit test instead of the 16-slot scan.

## 4. Risks and how each is handled

| Risk | Change | How it is handled |
|---|---|---|
| A new writer of `s_io_mappings` that does not mark its pages: PUTs into its range look unmapped, and an SPU-built pushbuffer there is not recorded | PUT trim | The list is file-static, and every current writer marks (3.2). The comment at the list says so. `GCM_SPU_PUT_FILTER=2` reports any mapped PUT with a clear bit, and `=0` turns the filter off live. |
| A PUT racing a map call gets "not mapped" | PUT trim | The same answer the scan can give for a racing PUT (3.5). Demon's Souls maps at boot, before the SPU images run (inferred). |
| The knob test costs more than it saves when PUTs mostly hit | PUT trim | A hit pays one load and one bit test more than today. The profile says nearly every call is a miss. |
| Stage 2 writes a wrong byte or register on the only graphics path: corruption or an RSX hang | Stage 2 | Prechecks before any write (2.5), check mode before the first mode-1 run (2.7), `regress.sh` and the soaks, and a default of 0. |
| A register thought dead is read after 0x4818 | Stage 2 | The block writes every register the lifted step writes (2.4), so no liveness claim is needed. |
| The frame overlaps the two quadwords and the order of accesses matters | Stage 2 | The frame check bails to the lifted code (2.5, item 4). |
| `image_id` or `drain_ret_pc` left differently from the bracket | Stage 2 | It copies `spu_call_done`'s plain-return case (`spu_context.h:897-905`), and check mode compares both. |
| An LS diagnostic sees accesses in another order | Stage 2 | It bails while `g_spu_ls_dbg` is set. |
| Stage 0 and Stage 2 both edit `spu_ring_hook.c` | Stage 2 | Land one, then rebase the other on master (2.7); not stacked. |
| The gain hides inside the noise because HighGraphics saturates first | both | In-boot live-knob A/Bs with pooled boots, and the CPU figure from `knobcpu.py` beside the frame rate. A gain inside the bar is dropped under the house rule; keeping it for the CPU saving is the maintainer's call. |

## 5. How to measure it tomorrow

Common setup, as in docs/48 section 9:
- Run from the port root in Git Bash, or from PowerShell as `& "C:\Program Files\Git\bin\bash.exe" -c "<line>"`.
- Set `DES_SCRATCH=<scratchpad>/...` and `PS3_NO_USER_INPUT=1`.
- Pass `RSX_PRESENT=0` while the Windows session is locked, and keep it the same for every run of a batch.
- Measure at `VBLANK_MHZ=240000` and `DES_FPS=60`, as the scripts do. Never overlap two runs.

**The PUT trim** (one ps3recomp change, so one build holds both sides):
1. Correctness: `GCM_SPU_PUT_FILTER=2 tools/des/regress.sh` with the new exe. Exit 0, and no `GCM_SPU_PUT_FILTER:` line in the log.
2. Frame rate, an in-boot live-knob A/B. The default is on, so the knob's B side turns it off:

   ```
   DES_EXE=<new exe> tools/des/knobab.sh put_filter_off GCM_SPU_PUT_FILTER=0 DES_FPS=60 VBLANK_MHZ=240000 PS3_NO_USER_INPUT=1
   ```

   Run 3 or more boots and pool them (`knobpool.py`), as biswf did. A gain shows as B slower than A.
3. **Expected:** A faster than B by +0.5 to +1.5%, at most +1.9% (inferred: about 1.1-1.3 points of a core off image 2, about 2.5 ns a command, at 0.6-0.75% per ns per command). That is at the in-boot bar (about 1-1.5%) and below the cross-build bars (2.8% profile fps, 4.5% gap fps). `knobcpu.py` should show image 2 about 1 point less busy with the filter on, whatever the frame rate does.

**Stage 2** (after it is written, 2.9):
1. Check mode first: `SPU_NATIVE_POST=2` through a levelcap walk in the Palace and the Nexus. Expect `0 mismatches` in the `[ring-post]` lines.
2. Then `SPU_NATIVE_POST=1 tools/des/regress.sh`, and `tools/des/soak.sh` about 6 minutes each in the Palace and the Nexus at `DES_FPS=120`. The flip count must keep rising.
3. Then the in-boot A/B, with B = native:

   ```
   DES_EXE=<stage 2 exe> tools/des/knobab.sh post SPU_NATIVE_POST=1 DES_FPS=60 VBLANK_MHZ=240000 PS3_NO_USER_INPUT=1
   ```

4. **Expected:** +3-5% (inferred, 2.10). That should clear the in-boot bar, and could clear the cross-build profile bar of 2.8%.

**The normal play line**, to look at the PUT trim with check mode on:

```
& "C:\Program Files\Git\bin\bash.exe" -c "DES_EXE=<new exe> tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 GCM_SPU_PUT_FILTER=2"
```

What to check in `work/tmp/play.txt`: no `GCM_SPU_PUT_FILTER:` line, and the game draws and plays as usual.

## 6. Branches and commit messages

**PUT trim:** ps3recomp branch `perf/spu-put-io` from `fork/master`, one commit with `spu_put_io.patch`. Tooling and fix commits stay separate; this is a single runtime change. After the A/B, the line "Tested on Demon's Souls only. Not re-run: Virtua Fighter 5, The Simpsons Game, Tornado Outbreak, Tokyo Jungle, Midway Arcade Origins." replaces the "Not built" line, followed by a results paragraph.

```
spu: a PUT that lands in no IO mapping skips the mapping scan

Every SPU PUT calls gcm_note_spu_put, which looks its EA up in the GCM
IO mapping list to find SPU-built pushbuffers. Most PUTs land in no
mapping, so gcm_ea2io_range walked all 16 slots for nothing. In the
Palace profile it ran 16 iterations a call over 698M calls, 1.4% of
Demon's Souls' ring consumer (SPU image 2), the thread that now holds
the frame.

- cellGcmSys.c: one bit per 1MB page (512 bytes in all). A mapping's
  pages are marked after its slot is written and stay marked when it is
  unmapped, so the bits are a superset of the list. A PUT whose page bit
  is clear gets "not mapped" at once; every other PUT scans the list as
  before, so each PUT gets the same answer as before.
- Every writer of the list marks the pages it adds: cellGcmInit, the
  AddressToOffset auto-map, MapMainMemory, MapEaIoAddress, the sys_rsx
  iomap and the replay loader. cellGcmInit and cellGcmTerminate clear
  the bits after the list.
- Threads: the mark is a release OR after the slot's stores, and the
  test an acquire load, so a PUT that sees a bit also sees the slot. A
  PUT ordered after a map call always sees its bits. One that races the
  call may miss the new mapping, as the scan alone could.
- GCM_SPU_PUT_FILTER (live): 1 (default) as above; 0 scans every PUT
  as before; 2 scans every PUT and prints up to 8 mapped PUTs whose
  page bit is clear.

Not built or run yet: written statically on 2026-10-19.
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

**Stage 2** (once written): ps3recomp `perf/spu-ring-post` (the hook) and port `perf/spu-ring-post` (the table entry), each based on its master. The ps3recomp PR is merged first. Suggested subjects:
- ps3recomp: `spu: image 2's end-of-command step in C when it only records where output starts (SPU_NATIVE_POST)`
- port: `spu: lift image 2 with the native end-of-command block`
