/* des_game_host.cpp: the port's part of the split build's exe (DES_SPLIT, docs/57).
 *
 * The lifted functions the port calls by name (DES_GAME_CALLS) are in the game
 * DLL. des_game_calls.h turns each name into a forwarder defined here, which
 * calls through a pointer that ps3_game_title_loaded resolves from the lift's
 * own function table when the DLL loads: the lifted function itself, not
 * whatever the address registry maps that address to (the port wraps some of
 * these addresses with its own functions, which call these). */
#ifdef PS3_GAME_SPLIT
#include "ppu_recomp.h"
#include "des_game_calls.h"
#include <cstdint>
#include <cstdio>

namespace des_game_calls {
#define DES_GAME_CALL_DEF_(a)                                                    \
    static void (*p_##a)(ppu_context*);                                          \
    void f_##a(ppu_context* ctx) { p_##a(ctx); }
DES_GAME_CALLS(DES_GAME_CALL_DEF_)
#undef DES_GAME_CALL_DEF_
}

namespace {
struct lift_entry { uint64_t addr; void (*func)(ppu_context*); const char* name; };

void (*find_lifted(const lift_entry* t, uint64_t n, uint64_t addr))(ppu_context*)
{
    for (uint64_t i = 0; i < n; i++)
        if (t[i].addr == addr) return t[i].func;
    return nullptr;
}
}

extern "C" int ps3_game_title_loaded(const void* func_table, uint64_t count)
{
    const lift_entry* t = static_cast<const lift_entry*>(func_table);
    int missing = 0;
#define DES_GAME_CALL_RESOLVE_(a)                                                \
    if (!(des_game_calls::p_##a = find_lifted(t, count, 0x##a##ull))) {           \
        std::fprintf(stderr, "[des_game] func_%s is not in the game DLL's function table%c", #a, 10); \
        missing++;                                                               \
    }
    DES_GAME_CALLS(DES_GAME_CALL_RESOLVE_)
#undef DES_GAME_CALL_RESOLVE_
    return missing;
}
#endif
