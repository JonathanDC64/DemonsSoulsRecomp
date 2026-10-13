/* Stubs for degenerate "external" function targets the lifter referenced but
 * never defined.
 *
 * In a few misdecoded regions (data interpreted as code -- see the nearby
 * `/* TODO: .word *\/` and `bdnzgela` artifacts in the generated chunks) the
 * lifter emits a branch/trampoline to a bogus address such as 0xFFFFFFE8
 * (= -24) and forward-declares `func_FFFFFFE8` as `/* external *\/`, but no
 * definition is ever produced -> a single unresolved symbol at link time.
 *
 * We satisfy it by routing to the runtime's existing unlifted-function logger.
 * These addresses are not real code; if such a path is ever executed it means
 * control reached misdecoded data, and the log will say so rather than the
 * build failing to link. (Lifter robustness fix candidate: route out-of-range
 * branch targets to ppu_unlifted_stub at generation time.)
 */
#include "ppu_recomp.h"
#include <cstdint>

extern "C" void ppu_unlifted_stub(uint64_t addr, ppu_context* ctx);

void func_FFFFFFE8(ppu_context* ctx) { ppu_unlifted_stub(0xFFFFFFE8ull, ctx); }

/* ---------------------------------------------------------------------------
 * Shadow call stack backing store (fork 4b770dd / 4c35a07).
 *
 * The PPU lift is produced with `--shadow-stack`, so every lifted function emits
 * a DS_FRAME guard that pushes its guest address onto these thread-local arrays
 * (declared `extern DS_TLS` in work/recomp/ppu_recomp.h). That gives a reliable
 * guest backtrace where the host back-chain is useless -- which is how the
 * NULL-vtable use-before-construction bug was localized.
 *
 * These lived in ps3recomp's ppu_loader.cpp before the v0.10.0 upstream merge;
 * upstream rewrote that file and has no shadow-stack support. Keeping the
 * definitions port-side means ps3recomp stays closer to upstream and this
 * survives the next merge.
 * -----------------------------------------------------------------------*/
#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
  #define DS_TLS __declspec(thread)
#else
  #define DS_TLS __thread
#endif

extern "C" {
    DS_TLS unsigned g_ds_sp = 0; DS_TLS unsigned g_ds_stk[8192];
    /* Per-entry ring (addr,this), never popped: the temporal call sequence,
     * including tail-callers the stack pops -- reveals where `this` goes 0. */
    DS_TLS unsigned g_ds_ring_pos = 0; DS_TLS unsigned g_ds_ring_a[1024];
    DS_TLS unsigned g_ds_ring_r3[1024];
}

/* One-shot diagnostic: confirm SPURS handler B (func_00C5D6D8) executes on our
 * posted SPU USER event. Gated by SPU_MBOX_LOG. */
void ds_note_handlerB(unsigned r3, unsigned r4)
{
    static int n = 0;
    if (getenv("SPU_MBOX_LOG") && n < 40) { n++;
        fprintf(stderr, "[handlerB] func_00C5D6D8 ENTER r3=0x%08X r4=0x%08X\n", r3, r4);
    }
}

extern "C" void ds_dump_shadow(void);
extern "C" void (*g_shadow_stack_dump_hook)(void);
static struct DsShadowHookInit {
    DsShadowHookInit() { g_shadow_stack_dump_hook = ds_dump_shadow; }
} s_ds_shadow_hook_init;

extern "C" void ds_dump_shadow(void)
{
    unsigned n = g_ds_sp; if (n > 8192u) n = 8192u;
    fprintf(stderr, "[shadow] guest call stack depth=%u (innermost first):\n", g_ds_sp);
    unsigned shown = n < 32u ? n : 32u;
    for (unsigned i = 0; i < shown; i++)
        fprintf(stderr, "   #%u 0x%08X\n", i, g_ds_stk[n - 1 - i]);
    if (n == 0) { fprintf(stderr, "   (empty -- lift without --shadow-stack?)\n"); fflush(stderr); return; }
    /* Temporal ring with `this`: find where this transitions valid->0. */
    unsigned win = g_ds_ring_pos < 64u ? g_ds_ring_pos : 64u;
    fprintf(stderr, "[shadow] last %u entries (addr this) -- watch this:valid->0:\n", win);
    for (unsigned i = 0; i < win; i++) {
        unsigned idx = (g_ds_ring_pos - win + i) & 1023u;
        fprintf(stderr, "   0x%08X this=0x%08X\n", g_ds_ring_a[idx], g_ds_ring_r3[idx]);
    }
    fflush(stderr);
}
