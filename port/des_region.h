/* des_region.h: which disc the game was lifted from, and its addresses.
 *
 * The port supports two discs: BLUS30443 (the US disc) and BLES00932 (the
 * European disc, 01.00). Their EBOOTs are the same program with most
 * functions moved a little, so every guest address in the port is written
 * as the US disc's, and DES_AT gives the running disc's:
 *
 *     vm_read32(DES_AT(kChrMan))
 *     ctx->lr = DES_AT(0x00724E40u);
 *
 * port/regions/BLES00932.inc has the European address of each. An address
 * missing there is a compile error at its DES_AT, and tools/region/region_table.py
 * (CI) checks that every address in the port's sources is in the table.
 *
 * What stays a US address:
 * - instruction hook keys (insn_hooks.txt, the handlers' `addr ==` and
 *   `case`): setup lifts the European EBOOT with each hook at its European
 *   address but called with its US key (ppu_lifter.py's sym@key);
 * - the lifted functions the port calls by name (func_XXXXXXXX): the split
 *   build resolves them at the European address when the game DLL loads
 *   (des_game_host.cpp).
 *
 * The region is set once, when the game DLL loads, before any guest code or
 * port hook runs. A single-exe build (not DES_SPLIT) links the lift's
 * functions by their US names, so it is always the US disc. */
#pragma once
#include <cstdint>

enum class DesRegion : uint8_t { US, EU };

extern "C" uint8_t g_des_region;   /* a DesRegion; des_region.cpp */

inline DesRegion des_region() { return static_cast<DesRegion>(g_des_region); }
/* "BLUS30443" or "BLES00932". */
const char* des_region_title_id(DesRegion r);

namespace des_region_detail {
struct Pair { uint32_t us, eu; };
inline constexpr Pair kEu[] = {
#define DES_REGION_ADDR(us, eu) {us, eu},
#include "regions/BLES00932.inc"
#undef DES_REGION_ADDR
};
inline constexpr uint32_t kEuCount = sizeof kEu / sizeof kEu[0];

/* The European address of a US address in the table; 0 if it isn't there. */
constexpr uint32_t eu_of(uint32_t us)
{
    uint32_t lo = 0, hi = kEuCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (kEu[mid].us < us) lo = mid + 1;
        else hi = mid;
    }
    return lo < kEuCount && kEu[lo].us == us ? kEu[lo].eu : 0;
}

constexpr bool sorted()
{
    for (uint32_t i = 1; i < kEuCount; i++)
        if (!(kEu[i - 1].us < kEu[i].us)) return false;
    return true;
}
static_assert(sorted(), "port/regions/BLES00932.inc must be sorted by the US address, one line each");
}

/* The running disc's address for the US address US (a constant). */
template <uint32_t US>
inline uint32_t des_at()
{
    constexpr uint32_t eu = des_region_detail::eu_of(US);
    static_assert(eu != 0, "this US address is not in port/regions/BLES00932.inc (tools/region/region_table.py find)");
    return g_des_region == static_cast<uint8_t>(DesRegion::EU) ? eu : US;
}
#define DES_AT(us) (des_at<(us)>())

/* ppu_register_function at the running disc's address of a US address. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(struct ppu_context*));
template <uint32_t US>
inline void des_register_function(void (*fn)(struct ppu_context*))
{
    ppu_register_function(des_at<US>(), fn);
}
#define DES_REGISTER_FUNCTION(us, fn) des_register_function<(us)>(fn)
