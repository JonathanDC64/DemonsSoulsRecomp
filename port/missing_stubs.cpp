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
