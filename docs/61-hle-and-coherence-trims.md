# 61. Two small trims: HLE dispatch and the lock-line notify (2026-10-19)

Status: written statically on 2026-10-19, during an A/B. Both patches have since merged: the HLE dispatch table as ps3recomp #75 and the notify hint as #76. The draft commit messages below are the drafts as written. Both patches are against
ps3recomp `fork/master` (4a76fc0), and each one passes `git apply --check` on its own against that commit. File:line
references are to `fork/master` unless they say otherwise.

Where the numbers come from: docs/56, "What the profile's counts show" (the deep1 PGO profile counts and prof3's sampled
shares). Both trims are under 1.5% of one thread, so neither will move the frame rate on its own (section 3).

## Short answer

| | Trim 1: HLE dispatch | Trim 2: lock-line notify |
|---|---|---|
| Today | `ps3_hle_call` scans the context-handler list, then the NID table, both linearly | `spu_coh_notify_write` reads all 8 SPU context slots on every store into a reserved line |
| Cost (docs/56) | ~103 loop passes a call, 1.1% of HighGraphics, 0.4% of main | 8 passes a call, 738M calls in 5 min, 3.3% of HighGraphics |
| Change | a hash index beside each table; same answers, first entry wins | a per-slot "line hint"; the notify skips slots whose hint is another line |
| Thread safety | lock-free lookups; slots written once with release; registrations serialized | hint written by its own context before the reservation is published; same ordering argument as `resv_valid` |
| Live knob | `PS3_HLE_HASH=0` brings back the scans | `SPU_COH_HINT=0` checks every slot, as before |
| Patch | `hle_dispatch.patch`, 1 file, +100 -26 (20 of them the re-indented dispatch body) | `coh_notify.patch`, 1 file, +50 -4 |
| Branch | `perf/hle-dispatch-table` | `perf/coh-notify-mask` |
| Main risk | low: a missed registration would show as an unresolved import | a missed notify can freeze the game; the argument in 2.4 says none is missed, and the backstops in 2.6 limit the damage if it were wrong |

## 1. HLE dispatch: a hash index for each handler table

### 1.1 What the code does today

- The lifted import stubs call `ps3_hle_call(nid, ctx)` (`runtime/ppu/ppu_hle.cpp:306`). After the diagnostics it asks
  `prx_resolve_export` whether a loaded PRX exports the NID (`ppu_hle.cpp:462`). That lookup is already a hash
  (`runtime/prx/prx_loader.c:19-80`, commit 99d1342).
- Then it scans the context-handler list `g_ctx` from index 0 (`ppu_hle.cpp:786-805`). The first entry whose NID matches
  wins.
- If none matches, it calls `ps3_nid_table_find` (`ppu_hle.cpp:807`). That is a linear scan too
  (`include/ps3emu/nid.h:200-207`), over a table of up to 4096 entries (`ppu_hle.cpp:43`). It also returns the first
  match.
- `ps3_hle_has` (`ppu_hle.cpp:77-84`) does the same two scans.
- Registration appends. `ps3_hle_register_ctx` (`ppu_hle.cpp:64-71`) writes the entry, then `g_ctx_count++`, with no
  duplicate check and no lock. `ps3_hle_register` (`ppu_hle.cpp:48-52`) calls `ps3_nid_table_add`
  (`include/ps3emu/nid.h:190-198`), which is the same. So a NID registered twice resolves to its first registration, and
  a NID in both tables resolves to its context handler.
- Who registers: `runtime/ppu/ppu_sysprx.cpp` (about 45 calls), `runtime/ppu/ppu_fs.cpp` (about 22), `runtime/spu/spu_raw.c`,
  `libs/network/sysNet.c`, `libs/network/sceNpScore.c`, `libs/system/cellSaveData.c`, and the generated registration
  unit through `ppu_hle_register_all` (`ppu_hle.cpp:907-912`). Today these run at init, but nothing prevents a later
  call from any thread.

Every HLE call that goes to the NID table first compares its NID with every context handler, and a context handler
registered late is found only after the ones before it. The profile reads ~103 loop passes a call (30.4e9 / 295.6M),
which docs/56 puts on the context-list loop (`ppu_hle.cpp:786`). `ps3_nid_table_find` is `static inline`, so its loop is
inside `ps3_hle_call` as well, and some of those passes may be the NID-table scan. The static reading cannot tell which
(the registered counts depend on the port's generated unit). The patch indexes both tables, so it covers either
reading.

### 1.2 The edits (all in `runtime/ppu/ppu_hle.cpp`)

1. Two hash indexes, `g_hle_nid_hash` (2^13 slots for the 4096-entry NID table) and `g_ctx_hash` (2^9 slots for the
   256-entry context list). Each is twice its table's capacity, as `prx_loader.c` does. Open addressing with linear
   probing and the same multiplicative hash (`nid * 0x9E3779B1`, top bits). A slot holds
   `(entry index + 1) << 32 | NID`, and 0 means empty, so a probe compares the NID without touching the entry.
2. `hle_hash_add` walks the probe run. If the NID is already there it returns, so the index keeps the first entry,
   which is the one the scan stops at. Otherwise it publishes the slot with a release store, after the entry is
   written.
3. `hle_hash_find` probes with acquire loads until it finds the NID or an empty slot.
4. Both register functions take a small spinlock, `g_hle_reg_lock`, around the append and the index insert.
5. `hle_ctx_find` and `hle_nid_find` give the index of the context handler and the NID-table entry. With
   `PS3_HLE_HASH=0` they run the old scans, line for line.
6. `ps3_hle_call` and `ps3_hle_has` use them. The dispatch body is unchanged, only re-indented from a loop into an
   `if`.

The registration functions keep both indexes up to date whatever the knob says, so the knob can be switched in a
running boot.

### 1.3 Why the lookup gives the scans' answers, from any number of threads

- **Duplicates.** The scan returns the lowest index with a matching NID. Entries are only appended, and the index takes
  a NID only once, at its first registration. So both return the same entry. A first entry with a null handler stays the
  answer, as before (`ps3_hle_call` then reports the NID unresolved).
- **Order between tables.** Unchanged: the context list first, then the NID table.
- **Full tables.** An append that does not fit is dropped, as before, and is not indexed. The indexes are at most half
  full, so a probe always ends.
- **Concurrent lookups.** Lookups take no lock. A slot changes once, from 0 to its final value, and never again: there is
  no delete and no rehash. The entry is written before its slot is published (release store), and the lookup reads the
  slot with an acquire load, so a lookup that sees the slot sees the whole entry. A probe run only grows, so a lookup that
  starts after a registration finished will find it.
- **Registration at run time.** A lookup racing a registration either sees nothing yet and falls through (the same as a
  scan that read the old `g_ctx_count`), or sees the complete entry. The scan was weaker here: the plain
  `g_ctx_count++` could become visible before the entry's fields.
- **Concurrent registrations.** Two registrations at once could lose an entry on master (two `g_ctx_count++`). With the
  index they could also claim one slot, which would make the answers differ. So registrations are serialized by
  `g_hle_reg_lock`. Registration is rare and short, so a spin with no back-off is enough, and nothing inside it can
  register again.

### 1.4 The patch (`hle_dispatch.patch`)

```diff
diff --git a/runtime/ppu/ppu_hle.cpp b/runtime/ppu/ppu_hle.cpp
index fd512ba..84bc683 100644
--- a/runtime/ppu/ppu_hle.cpp
+++ b/runtime/ppu/ppu_hle.cpp
@@ -45,10 +45,63 @@ static ps3_nid_entry  g_hle_storage[HLE_NID_CAP];
 static ps3_nid_table  g_hle_nids;
 static int            g_hle_inited = 0;
 
+/* NID -> entry index, one index per handler table. ps3_hle_call looks the NID
+ * up on every HLE call, in the context-handler list and then in the NID
+ * table, and both were linear scans: a call that goes to the NID table paid a
+ * scan of the whole context list first (~100 compares a call, 1.1% of
+ * Demon's Souls' HighGraphics). A hash index answers in a probe or two, as
+ * prx_resolve_export's does. Open addressing, linear probing; a slot holds
+ * (entry index + 1) << 32 | NID, 0 when empty; twice the table's capacity.
+ *
+ * Same answers as the scans: a NID registered twice keeps its first entry in
+ * the index, which is the one the scan stops at. Lookups take no lock: a slot
+ * goes from 0 to its value once and never changes again, and it is published
+ * (release) after its entry is written, so a lookup racing a registration
+ * sees no entry or a whole one, and a probe run only grows. Registrations take
+ * g_hle_reg_lock, so two of them cannot claim one slot. PS3_HLE_HASH=0 (live)
+ * brings back the scans; both indexes are kept either way. */
+#define HLE_NID_HASH_BITS 13
+static uint64_t g_hle_nid_hash[1u << HLE_NID_HASH_BITS];
+static volatile long g_hle_reg_lock = 0;
+
+static void hle_reg_lock(void)   { while (__atomic_exchange_n(&g_hle_reg_lock, 1, __ATOMIC_ACQUIRE)) { } }
+static void hle_reg_unlock(void) { __atomic_store_n(&g_hle_reg_lock, 0, __ATOMIC_RELEASE); }
+
+/* The entry index for nid, or -1. */
+static int hle_hash_find(const uint64_t* tab, unsigned bits, uint32_t nid)
+{
+    const uint32_t mask = (1u << bits) - 1;
+    for (uint32_t h = (nid * 0x9E3779B1u) >> (32 - bits);; h = (h + 1) & mask) {
+        const uint64_t v = __atomic_load_n(&tab[h], __ATOMIC_ACQUIRE);
+        if (!v) return -1;
+        if ((uint32_t)v == nid) return (int)(v >> 32) - 1;
+    }
+}
+
+/* Under g_hle_reg_lock, after entry i is written. */
+static void hle_hash_add(uint64_t* tab, unsigned bits, uint32_t nid, uint32_t i)
+{
+    const uint32_t mask = (1u << bits) - 1;
+    uint32_t h = (nid * 0x9E3779B1u) >> (32 - bits);
+    for (uint64_t v; (v = tab[h]) != 0; h = (h + 1) & mask)
+        if ((uint32_t)v == nid) return;      /* the first entry wins, as in the scan */
+    __atomic_store_n(&tab[h], ((uint64_t)(i + 1) << 32) | nid, __ATOMIC_RELEASE);
+}
+
+static int hle_hash_on(void)
+{
+    static int s_on = -1;
+    PS3_KNOB(s_on, ps3_env_int("PS3_HLE_HASH", 1));
+    return s_on;
+}
+
 extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler)
 {
+    hle_reg_lock();
     if (!g_hle_inited) { ps3_nid_table_init(&g_hle_nids, g_hle_storage, HLE_NID_CAP); g_hle_inited = 1; }
-    ps3_nid_table_add(&g_hle_nids, nid, name, handler);
+    if (ps3_nid_table_add(&g_hle_nids, nid, name, handler) == 0)
+        hle_hash_add(g_hle_nid_hash, HLE_NID_HASH_BITS, nid, g_hle_nids.count - 1);
+    hle_reg_unlock();
 }
 
 extern "C" uint32_t ps3_hle_count(void) { return g_hle_inited ? g_hle_nids.count : 0; }
@@ -60,14 +113,36 @@ typedef void (*hle_ctx_fn)(ppu_context*);
 #define HLE_CTX_CAP 256
 static struct { uint32_t nid; hle_ctx_fn fn; const char* name; } g_ctx[HLE_CTX_CAP];
 static uint32_t g_ctx_count = 0;
+#define HLE_CTX_HASH_BITS 9
+static uint64_t g_ctx_hash[1u << HLE_CTX_HASH_BITS];   /* see g_hle_nid_hash */
 
 extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn)
 {
+    hle_reg_lock();
     if (g_ctx_count < HLE_CTX_CAP) {
         g_ctx[g_ctx_count].nid = nid; g_ctx[g_ctx_count].fn = fn;
         g_ctx[g_ctx_count].name = name ? name : "?";   /* named in the milestone log */
+        hle_hash_add(g_ctx_hash, HLE_CTX_HASH_BITS, nid, g_ctx_count);
         g_ctx_count++;
     }
+    hle_reg_unlock();
+}
+
+/* nid's context handler (its index in g_ctx), or -1. */
+static int hle_ctx_find(uint32_t nid)
+{
+    if (hle_hash_on()) return hle_hash_find(g_ctx_hash, HLE_CTX_HASH_BITS, nid);
+    for (uint32_t i = 0; i < g_ctx_count; i++)
+        if (g_ctx[i].nid == nid) return (int)i;
+    return -1;
+}
+
+/* nid's entry in the NID table (the first one registered), or null. */
+static ps3_nid_entry* hle_nid_find(uint32_t nid)
+{
+    if (!hle_hash_on()) return g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
+    const int k = hle_hash_find(g_hle_nid_hash, HLE_NID_HASH_BITS, nid);
+    return k >= 0 ? &g_hle_storage[k] : nullptr;
 }
 
 /* Is this NID implemented here? Lets a host boot harness that owns its own
@@ -76,10 +151,8 @@ extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn
  * "unresolved NID" error it cannot distinguish from a real failure. */
 extern "C" int ps3_hle_has(uint32_t nid)
 {
-    for (uint32_t i = 0; i < g_ctx_count; i++)
-        if (g_ctx[i].nid == nid) return 1;
-    if (!g_hle_inited) return 0;
-    ps3_nid_entry* e = ps3_nid_table_find(&g_hle_nids, nid);
+    if (hle_ctx_find(nid) >= 0) return 1;
+    ps3_nid_entry* e = hle_nid_find(nid);
     return (e && e->handler) ? 1 : 0;
 }
 
@@ -783,28 +856,29 @@ extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
         }
     }
 
-    for (uint32_t i = 0; i < g_ctx_count; i++)
-        if (g_ctx[i].nid == nid) {
-            ps3_msf("hle:%s", g_ctx[i].name);
-            /* Record the name here too. This loop dispatches every
-             * ps3_hle_register_ctx handler -- which is the whole sysPrxForUser
-             * and CRT surface: spinlocks, lwmutex, thread and fs ops -- and it
-             * returned without touching g_last_hle_name or the in-flight slot.
-             * So for a title wedged anywhere in that surface the watchdog named
-             * whichever TABLE handler ran last instead, with total confidence.
-             *
-             * Guitar Hero III wedges in sys_spinlock_unlock and was reported as
-             * "cellPadInit", which for a game that needs a guitar controller is
-             * a very convincing wrong answer to chase. */
-            g_last_hle_name = g_ctx[i].name;
-            hle_first_seen(nid, g_ctx[i].name, ctx); hle_lr_seen(nid, g_ctx[i].name, ctx);
-            { unsigned t = (unsigned)ctx->thread_id;
-              if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = g_ctx[i].name; }
-            g_ctx[i].fn(ctx);
-            return;   /* the guard clears the in-flight slot (after `waits` reads it) */
-        }
+    const int ci = hle_ctx_find(nid);
+    if (ci >= 0) {
+        const uint32_t i = (uint32_t)ci;
+        ps3_msf("hle:%s", g_ctx[i].name);
+        /* Record the name here too. This branch dispatches every
+         * ps3_hle_register_ctx handler -- which is the whole sysPrxForUser
+         * and CRT surface: spinlocks, lwmutex, thread and fs ops -- and it
+         * returned without touching g_last_hle_name or the in-flight slot.
+         * So for a title wedged anywhere in that surface the watchdog named
+         * whichever TABLE handler ran last instead, with total confidence.
+         *
+         * Guitar Hero III wedges in sys_spinlock_unlock and was reported as
+         * "cellPadInit", which for a game that needs a guitar controller is
+         * a very convincing wrong answer to chase. */
+        g_last_hle_name = g_ctx[i].name;
+        hle_first_seen(nid, g_ctx[i].name, ctx); hle_lr_seen(nid, g_ctx[i].name, ctx);
+        { unsigned t = (unsigned)ctx->thread_id;
+          if (t < PS3_HLE_INFLIGHT_MAX) g_hle_inflight[t] = g_ctx[i].name; }
+        g_ctx[i].fn(ctx);
+        return;   /* the guard clears the in-flight slot (after `waits` reads it) */
+    }
 
-    ps3_nid_entry* e = g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
+    ps3_nid_entry* e = hle_nid_find(nid);
     if (!e || !e->handler) {
         /* Recorded before the diagnostic paths below, several of which return
          * early. An import moving between resolved and unresolved is exactly the
```

### 1.5 Risks and how each is handled

- **A different answer from the scan.** Handled by keeping the first entry, the same table order, and the same "full
  table drops the entry" rule. If it were wrong anyway, the import would land on the wrong handler or read as
  unresolved: the `hle:unresolved:` milestone line and `PS3_HLE_UNRESOLVED` show that at once, and `PS3_HLE_HASH=0`
  confirms it in the same boot.
- **Build.** Not compiled yet. It uses the same builtins (`__atomic_*`) as `prx_loader.c` and the rest of this file, and
  the `PS3_KNOB` pattern already used in `ps3_hle_call` (`ppu_hle.cpp:314-315`). Run the HLE unit tests
  (`runtime/ppu/tests/test_hle.cpp`, `test_hle_integration.cpp`) after the build.
- **Memory.** +68 KiB of zeroed BSS (64 KiB for the NID index, 4 KiB for the context index).
- **The spinlock.** Only registrations take it, and nothing inside it registers again, so it cannot deadlock. It spins
  with no pause; a registration holds it for well under a microsecond.
- **The knob cost.** `hle_hash_on` is a `PS3_KNOB`: one load and compare, twice a call.

### 1.6 How to measure it

- Knob: `PS3_HLE_HASH=0` is the old scans, live through the console's `setenv`.
- In-boot live-knob A/B: `perfknob.sh` (docs/56 section 1) with a knob line `PS3_HLE_HASH=0` and the reference line
  `PERF_NULL=1`, at the deep Palace save. Read `knobcpu.py`'s per-thread CPU for HighGraphics and main.
- The direct check: a `profrip` of HighGraphics with the knob on and off. `ps3_hle_call`'s exclusive share should fall
  from ~1.1% to ~0.1-0.2%.
- Expected size: about 1 point of HighGraphics' CPU and 0.3-0.4 of main's. That is well under the 2.8% Palace bar for
  frame rate, so it cannot be seen in frames alone. It shows in the profile share, and probably in `knobcpu.py` over
  enough windows. Stacked with trim 2 (and the `gcm_ea2io_range` trim of docs/56), it is about 3 points of HighGraphics:
  still under the bar on its own, and it only reaches frames where HighGraphics holds the frame (the base build, not the
  CR-fields stack, where the FIFO thread holds it).

### 1.7 Branch and commit message

Branch (ps3recomp): `perf/hle-dispatch-table`.

```
hle: find a NID's handler through hash indexes, not two scans

ps3_hle_call looks the NID up on every HLE call: in the context-handler
list (ps3_hle_register_ctx), then in the NID table (ps3_hle_register).
Both were linear scans, so a call that goes to the NID table first
compared its NID with every context handler (~100 compares a call, 1.1%
of Demon's Souls' HighGraphics in the Palace profile). Each table now has
a hash index beside it, as prx_resolve_export has: open addressing, a
slot holding the entry's index and its NID, twice the table's capacity.

The answers are the scans' answers. A NID registered twice keeps its
first entry in the index, which is the entry the scan stopped at, and a
NID in both tables still goes to its context handler. Lookups take no
lock: a slot is written once, after its entry, with a release store, so
a lookup racing a registration sees no entry or a whole one.
Registrations now take a small spinlock, so two of them cannot claim
one slot (g_ctx_count++ from two threads could lose an entry before).
ps3_hle_has uses the same lookups.

The live knob PS3_HLE_HASH=0 brings back the scans; both indexes are
kept either way, so the knob can be switched in a running boot.

Not built or run yet: written statically on 2026-10-19.
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

## 2. Lock-line notify: a line hint per context slot

### 2.1 The reservation code, end to end

Read in full before the change. The pieces that matter:

- **The lock.** One spinlock per line stripe (64 stripes, `runtime/spu/spu_coherency.c:61-62`). Each stripe is a
  sequence word: odd while held, +2 per release. A writer takes it with `lock bts`, a locked read-modify-write
  (`spu_ll_acquire`, `spu_coherency.c:126-153`), and releases it with `_InterlockedIncrement`
  (`spu_coherency.c:170-174`). Both are full barriers.
- **The lock-free read.** `spu_lockline_read_begin` reads the stripe word (`spu_coherency.c:175-180`).
  `spu_lockline_read_ok` runs a sequentially consistent fence, then reads it again (`spu_coherency.c:181-187`). Its
  comment states the rule this whole file depends on: "the caller's reservation stores must be visible before this read,
  so a writer that takes the stripe after it finds and drops them".
- **The bitmap and the slots.** A bit per 128-byte line, set by the first reservation and never cleared
  (`spu_coherency.c:228-234`). Eight context slots, `s_coh_ctxs` (`spu_coherency.c:246-247`). `spu_coh_reserve`
  (`spu_coherency.c:251-296`) sets the bit and puts the context in a slot. Only the context's own thread adds or removes
  it. `spu_coh_unregister` and `spu_coh_forget_range` clear a slot under all 64 stripes (`spu_coherency.c:308-349`).
- **GETLLAR** (`spu_getllar_read`, `runtime/spu/spu_channels.c:583-643`). On a line already marked, the lock-free path
  calls `spu_coh_reserve`, copies the line, sets `resv_ea` and `resv_valid = 1`, then `spu_lockline_read_ok`
  (`spu_channels.c:594-600`). If a writer took the stripe meanwhile, it redoes all of it under the stripe
  (`spu_channels.c:601-641`). Both paths end with `resv_valid = 1` and no early exit.
- **PUTLLC** (`spu_channels.c:995-1160`). A no-op PUTLLC (it writes back the same bytes) drops the reservation and is
  decided by the sequence word or under the stripe (`spu_channels.c:1011-1048`). A storing PUTLLC commits under the
  stripe and calls `spu_coh_notify_write` for its peers (`spu_channels.c:1051-1090`). PUTLLUC does the same
  unconditionally (`spu_channels.c:1164-1182`). The lean path (`spu_atomic_lean`, `spu_channels.c:1296-1365`) drops the
  reservation with an atomic exchange, then compares the line with the snapshot, which catches raw stores that notify
  nobody (commit c116abf, the PUTLLC freeze).
- **The PPU side.** Every lifted guest store goes through `VM_WRITE_COH` (`runtime/ppu/ppu_loader.cpp:1657-1668`): if
  the bitmap marks the line (`spu_coh_reserved_fast`, `ppu_loader.cpp:789-794`), it takes the stripe, stores, notifies
  and releases. `stwcx.`/`stdcx.` do the same around their CAS (`ppu_stwcx32`, `ppu_loader.cpp:994-1030`;
  `ppu_stdcx64`, `ppu_loader.cpp:1086-1104`). The inline HLE stores and block copies do it too
  (`runtime/ppu/ppu_memory.h:144-154`, `247-279`, `306-345`, `360-390`). The PPU's own lwarx reservations are a separate
  mechanism (`ppu_resv_break`), fixed for order in a72bac3; this change does not touch them.
- **The libraries.** `cellSync` (`libs/sync/cellSync.c:75-95`, `418-441`) and the SPURS event flag
  (`libs/spurs/cellSpurs.c:285-291`) notify under the line's stripe as well.

So every caller of `spu_coh_notify_write` holds the stripe of the line it notifies, taken before the store. The header
says so too (`runtime/spu/spu_coherency.h:73-79`). The patch depends on that, exactly as the existing `resv_valid` check
does.

### 2.2 What costs the time

`spu_coh_notify_write` (`spu_coherency.c:409-434`) walks all 8 slots. For each live one it reads the context pointer,
`resv_valid` and `resv_ea`. Those two fields sit on a cache line of their own per context
(`runtime/spu/spu_context.h:270-280`, commit 680f1fd), which that context's SPU writes on every GETLLAR and PUTLLC. The
profile: 738M calls in 5 minutes (about 30,000 a frame), all 8 passes every call, 3.3% of HighGraphics. Nearly every
store is to a line that one context or none holds, so most of those reads find nothing.

### 2.3 The change (all in `runtime/spu/spu_coherency.c`)

1. `s_coh_hint`: one 32-bit hint per slot, the line (`ea & ~127`) that slot's context reserved last. All 8 hints sit on
   one cache line of their own.
2. `spu_coh_reserve` writes the hint of the slot it finds or claims, and only when it changes. It runs before the
   GETLLAR sets `resv_valid`, on both paths.
3. `spu_coh_notify_write` skips a slot whose hint is not this line. Slots whose hint matches get exactly the old check.
4. Hints are never cleared. A stale hint only means the full check runs for that slot, and the full check rejects it.
5. `SPU_COH_HINT=0` (live) checks every slot, as before. The hints are kept up to date whatever the knob says, so it can
   be switched in a running boot.

Why not the ideas in the task and in docs/56:

- **A count of valid reservations.** In this game some SPURS kernel nearly always holds a reservation, so the count is
  rarely 0 and would skip almost nothing. Keeping it would also mean a locked add on every GETLLAR and PUTLLC, on one
  line shared by all SPU threads. Image 2 is ~89% busy.
- **A bitmask of contexts with a live reservation.** It needs clearing to be useful. A clear by a notifier races the
  lock-free GETLLAR: the notifier sees `resv_valid == 0`, the GETLLAR then sets it and passes its stripe check (the
  notifier holds another stripe), and the bit stays clear on a live reservation. That is a missed notify and a freeze. A
  clear by the owner is safe, but costs two locked operations on a shared line per GETLLAR/PUTLLC pair (the ring
  consumer does ~1.8M GETLLARs a second).
- The hint needs neither: only the owner writes it, only when its line changes, and nobody clears it.

### 2.4 Why no notify is missed

Let N be a notifier for line L. It holds L's stripe, taken with a locked RMW before it reads anything. Let R be a
reservation of context C (slot i) on L that N has to break: R was taken before N's store.

1. **R taken under the stripe.** C wrote `hint[i] = L` while holding the stripe, then set `resv_valid`, then released
   the stripe with a full barrier. N took the stripe after that release, so N sees everything C wrote before it,
   including the hint. (If N took the stripe first, C's copy of the line already holds N's store, and nothing needs
   breaking.)
2. **R taken without the lock.** C wrote `hint[i] = L` (in `spu_coh_reserve`), copied the line, set `resv_valid`, ran
   the fence in `spu_lockline_read_ok` and found the stripe word unchanged. So N's locked RMW on that word comes after
   C's fence. A sequentially consistent fence before a load, against a locked RMW before a load, is the store-buffer
   pattern: at least one side sees the other. C did not see N, so N sees C's stores before the fence, including the
   hint. (If C had seen N, its read fails and it redoes it under the stripe, which is case 1.) This is the same argument
   that makes N see `resv_valid` today. The hint is one more store in the same window, before the same fence.
3. **The one case the hint skips.** `hint[i]` can only change in C's own `spu_coh_reserve`, which only
   `spu_getllar_read` calls, and that always ends with `resv_ea` = the new line and `resv_valid = 1`. So if N reads a
   hint for another line L2 while C's `resv_valid` still shows R on L, C is in the middle of a GETLLAR of L2, which
   replaces R. Skipping it loses only the LR event bit for R, which C has abandoned. That is the outcome of N's store
   landing just after C's GETLLAR of L2, an order nothing rules out: the two are concurrent. On hardware, a GETLLAR also
   replaces the old reservation.
4. **Stale hints.** An empty slot, or a context with no reservation on L, may still show L. N then runs the old check,
   which rejects it. A slot claimed by a new context gets its hint in the same call that claims it, before that context
   sets `resv_valid`.
5. **The PUTLLC "put back" paths** (`spu_channels.c:1029` and `1358`) set `resv_valid = 1` again without calling
   `spu_coh_reserve`. They keep `resv_ea`, and only C's own thread runs them, between its GETLLAR and its PUTLLC, so the
   hint still names that line.

One behaviour changes, in the direction of hardware. A store that overlaps a lock-free GETLLAR whose stripe check is
about to fail may have set a spurious LR bit on master (only when it happened to see the early `resv_valid = 1`). With
the hint it may not. The GETLLAR redoes its read under the stripe and sees the store either way. Real hardware raises no
LR for a reservation taken after the store.

The two freezes the task names are the same kind of ordering. In a72bac3 the lifted lwarx loaded before it published its
reservation. The hint is published before the fence, in the same order as `resv_valid`. In c116abf a no-op PUTLLC was
decided from the reservation alone, and missed raw stores that notify nobody. This patch changes neither which stores
notify nor how a PUTLLC decides.

### 2.5 The patch (`coh_notify.patch`)

```diff
diff --git a/runtime/spu/spu_coherency.c b/runtime/spu/spu_coherency.c
index aa2e29d..fb2d236 100644
--- a/runtime/spu/spu_coherency.c
+++ b/runtime/spu/spu_coherency.c
@@ -246,6 +246,30 @@ SPU_COH_LINE(int) g_spu_coh_armed_line;   /* own line, as g_lockline; exported a
 #define SPU_COH_MAX_CTX 8
 static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];
 
+/* The line (ea & ~127) each slot's context reserved last, so a notify passes
+ * over the contexts whose reservation is on another line without reading
+ * them. spu_coh_notify_write looked at all eight on every store into a
+ * reserved line -- Demon's Souls' HighGraphics makes ~30,000 a frame, and the
+ * scan was 3.3% of that thread -- although nearly every store is to a line
+ * one context or none holds.
+ *
+ * Written only by the slot's own context, in spu_coh_reserve, before the
+ * GETLLAR sets resv_valid: under the line's stripe on the locked path, and
+ * before the StoreLoad fence of spu_lockline_read_ok on the lock-free one. A
+ * notifier holds the line's stripe, so it sees the hint of every reservation
+ * it has to break, by the argument that makes it see resv_valid (see
+ * spu_coh_notify_write). A hint is never cleared: a stale one names a line
+ * whose context then fails the full check, as before. On a cache line of its
+ * own, as it changes only when a context moves to another line. */
+typedef uint32_t spu_coh_hint_t[SPU_COH_MAX_CTX];
+static SPU_COH_LINE(spu_coh_hint_t) s_coh_hint;
+
+static void spu_coh_hint_set(int i, uint32_t line_ea)
+{
+    if (__atomic_load_n(&s_coh_hint.v[i], __ATOMIC_RELAXED) != line_ea)
+        __atomic_store_n(&s_coh_hint.v[i], line_ea, __ATOMIC_RELAXED);
+}
+
 unsigned long g_spu_lr_raise = 0;
 
 void spu_coh_reserve(spu_context* ctx, uint32_t ea)
@@ -272,17 +296,25 @@ void spu_coh_reserve(spu_context* ctx, uint32_t ea)
      * runs during other threads' unregisters), and stopping at the first empty
      * slot registered a context a second time below its own entry. Unregister
      * cleared one; the other dangled once the context's stack was gone, and
-     * spu_coh_notify_write faulted on it. */
+     * spu_coh_notify_write faulted on it. The slot found or claimed takes
+     * the line's hint before the caller sets resv_valid. */
+    const uint32_t line_ea = ea & ~127u;
     for (int i = 0; i < SPU_COH_MAX_CTX; i++)
-        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE) == ctx) return;
+        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE) == ctx) {
+            spu_coh_hint_set(i, line_ea);
+            return;
+        }
     for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
         spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
         if (c == NULL) {
             /* Claim the slot: another stripe's holder may be claiming it too. */
             spu_context* expect = NULL;
             if (__atomic_compare_exchange_n(&s_coh_ctxs[i], &expect, ctx, 0,
-                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
-            if (expect == ctx) return;
+                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ||
+                expect == ctx) {
+                spu_coh_hint_set(i, line_ea);
+                return;
+            }
         }
     }
     /* More live SPU contexts than the registry holds. The ones already in it
@@ -409,7 +441,21 @@ uint32_t spu_coh_rmw32(uint32_t ea, uint32_t set, uint32_t clr)
 void spu_coh_notify_write(uint32_t ea)
 {
     uint32_t line = ea & ~127u;
+    /* Only the slots whose hint is this line (s_coh_hint). Every caller holds
+     * the line's stripe, taken with a locked RMW before this runs. A GETLLAR
+     * that took its reservation under the stripe released it after writing
+     * the hint; one that took it without the lock wrote the hint, fenced, and
+     * found the stripe word unchanged, so its fence came before our RMW and
+     * the hint is visible here. Either way a reservation this store has to
+     * break shows its line in the hint, unless its context has since begun a
+     * GETLLAR of another line: that GETLLAR replaces the reservation, and the
+     * store reads as one that landed after it. A reservation whose GETLLAR
+     * this store overlaps redoes its read under the stripe and sees the
+     * store. SPU_COH_HINT=0 (live) checks every context, as before. */
+    static int s_hint = -1;
+    PS3_KNOB(s_hint, ps3_env_int("SPU_COH_HINT", 1));
     for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
+        if (s_hint && __atomic_load_n(&s_coh_hint.v[i], __ATOMIC_RELAXED) != line) continue;
         spu_context* c = s_coh_ctxs[i];
         if (!c) continue;
         if (c->resv_valid && (c->resv_ea & ~127u) == line) {
```

### 2.6 Risks and how each is handled

- **A missed notify (freeze).** The argument in 2.4 covers every reservation path on master: both GETLLAR paths, the
  two PUTLLC put-back paths, `spu_llar_noop_commit` (which sets `resv_ea` but leaves `resv_valid` at 0,
  `spu_channels.c:1413-1416`), and the slot claim and release paths. `git grep` over `runtime`, `libs`, `include` and
  `tools` finds no other writer of `resv_valid` or `resv_ea` (the only other one, `runtime/spu/spu_ring_hook.c:98`,
  clears it), so the lifted code goes through these paths too. It depends on every notifier holding the line's
  stripe, which section 2.1 checked caller by caller. A future caller that notifies without the stripe would be wrong
  with or without this patch. Two backstops already exist if it were wrong: the event-channel check compares the
  reserved line with its snapshot on every `SPU_RdEventStat` (`spu_resv_lost_poll`, `spu_channels.c:1610-1618`), and a
  GETLLAR nap wakes after 2 ms at the latest (`spu_llar_idle_wait`, `spu_channels.c:1647-1665`). So a miss would most
  likely read as latency, not a hang, except for a write that restores the same bytes. The knob `SPU_COH_HINT=0` brings
  back the full check in the same boot.
- **Cost moved to the SPU side.** Every GETLLAR now reads the hint line (one relaxed load), and writes it when its line
  changes. The ring consumer mostly re-reserves the ring header (the comment at `spu_channels.c:1005-1009`: ~89% of its
  PUTLLCs re-commit that unchanged line), so its hint should rarely change. A context that hops between lines on every GETLLAR would write the hint line each
  time, and each write costs every notifier a miss on it. Measure image 2's CPU as well as HighGraphics'.
- **Other notifiers.** `spu_coh_notify_write_except` (the SPU DMA put path, `spu_coherency.c:368-381`, called from
  `runtime/spu/spu_dma.h:779-804`, `1116-1139`) is left as it is. It could use the same hint, but its callers' locking
  was not checked here.
- **Build and tests.** Not compiled yet. It uses the file's own `SPU_COH_LINE` macro (with a typedef for the array)
  and the `PS3_KNOB` pattern already in `spu_coh_notify_write`. `runtime/spu/tests/test_spu_coherency.c` includes
  `spu_coherency.c` directly; its GETLLAR helper calls `spu_coh_reserve` before it sets `resv_valid` (lines 96-104), and
  its one direct `resv_valid = 1` (line 266) keeps the same line, so it should pass unchanged, as should
  `test_spu_lockline_peer.c`. Run both with `SPU_COH_HINT` unset and at 0.

### 2.7 How to measure it

- Knob: `SPU_COH_HINT=0` is the full check, live through the console's `setenv`.
- In-boot live-knob A/B: `perfknob.sh` with a knob line `SPU_COH_HINT=0` and the reference `PERF_NULL=1`, at the deep
  Palace save. Read `knobcpu.py` for HighGraphics (expected lower with hints on) and image 2 (expected unchanged).
- The direct check: a `profrip` of HighGraphics with the knob on and off. `spu_coh_notify_write` should fall from ~3.3%
  to about 1%. Also check image 2's profile for `spu_coh_reserve`.
- Expected size: about 2 points of HighGraphics' CPU at best. Like trim 1 it is under the 2.8% bar for frame rate alone.
  It shows in the profile share and probably in `knobcpu.py`; stacked with trim 1 it is ~3 points of HighGraphics.
- Also run the regress check (`regress.sh`) and a soak with the knob on (default), since a coherence miss could take
  minutes to show. A freeze with the knob on and none with it off, on the same save, is the signal to drop the patch.

### 2.8 Branch and commit message

Branch (ps3recomp): `perf/coh-notify-mask`.

```
spu: a lock-line notify skips contexts reserving other lines

spu_coh_notify_write looked at all eight registered SPU contexts on
every store into a reserved line: their pointer, resv_valid and
resv_ea, each context's on a cache line of its own that its SPU writes
on every GETLLAR and PUTLLC. Demon's Souls' HighGraphics makes ~30,000
such stores a frame, and the loop was 3.3% of that thread in the
Palace profile, although nearly every store is to a line that one
context or none holds.

Each slot now keeps a hint: the line its context reserved last.
spu_coh_reserve writes it (only when it changes) before the GETLLAR
sets resv_valid, under the line's stripe or before the StoreLoad fence
of the lock-free read. Every caller of the notify holds the line's
stripe, taken with a locked RMW, so it sees the hint of every
reservation it has to break, by the same argument that makes it see
resv_valid. The notify skips the slots whose hint is another line and
runs the full check on the rest. A hint is never cleared; a stale one
only costs the full check. The one case it skips that the old loop
might not is a context already reading another line, whose GETLLAR
replaces the reservation: the store then reads as one that landed
after that GETLLAR.

The live knob SPU_COH_HINT=0 checks every context, as before. The
hints are kept either way, so the knob can be switched in a running
boot.

Not built or run yet: written statically on 2026-10-19.
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

## 3. Open items

- Neither patch is built. After the build, run the runtime tests, then the live-knob A/Bs in 1.6 and 2.7.
- Each patch applies to `fork/master` on its own (checked with `git apply --check` in a scratch index). They touch
  different files, so they also stack.
- Once tested, replace the commit messages' last-but-one line with the house line ("Tested on Demon's Souls only. Not
  re-run: ...") and add the measured numbers.
- Unit tests worth adding with trim 1: register a NID twice and in both tables, then check `ps3_hle_has` and the
  dispatch with `PS3_HLE_HASH` at 1 and at 0.
- Not done: the hint for `spu_coh_notify_write_except`; the `gcm_ea2io_range` bound from docs/56 (the third scan in that
  table).
