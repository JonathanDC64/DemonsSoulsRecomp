/* des_game_module.cpp: the port's part of the game DLL (DES_SPLIT, docs/57).
 *
 * The DLL's one export, des_game_init. It runs the runtime's half
 * (ps3_game_module_init, runtime/ppu/game_module.c), fills the jump slots of
 * the port functions the lifted code calls by name (DES_GAME_TITLE_FUNCS)
 * from the exe, and hands back the startup work a single exe did on its own:
 * the SPU workload registration (a static constructor there) and the lifted
 * libsre's glue (ps3_load_prx_modules). */
#ifndef PS3_GAME_MODULE
#define PS3_GAME_MODULE 1
#endif
#include <cstdint>
#include <cstdio>
#include "ps3emu/game_module.h"
#include "des_game_title_funcs.h"   /* DES_GAME_TITLE_FUNCS, from the build directory */

extern "C" {
#define DES_GM_SLOT_(n) void* ps3_gt_##n;
DES_GAME_TITLE_FUNCS(DES_GM_SLOT_)
#undef DES_GM_SLOT_
}
#define DES_GM_THUNK_(n) PS3_GAME_THUNK(n)
DES_GAME_TITLE_FUNCS(DES_GM_THUNK_)
#undef DES_GM_THUNK_

extern "C" void des_spu_register_all(void);   /* spu_workloads.cpp, without its constructor */
#ifdef DES_GAME_HAS_LIBSRE
extern "C" void ps3_load_prx_modules(void);   /* port/gen/libsre_glue.c */
#endif

extern "C" __declspec(dllexport) int des_game_init(const ps3_host_api* host, ps3_game_api* game)
{
    const int rc = ps3_game_module_init(host, game);
    if (rc != 0) return rc;
    int missing = 0;
#define DES_GM_FILL_(n)                                                          \
    if (!(ps3_gt_##n = host->title_fn(#n))) {                                     \
        std::fprintf(stderr, "[des_game] the exe has no %s%c", #n, 10);           \
        missing++;                                                               \
    }
    DES_GAME_TITLE_FUNCS(DES_GM_FILL_)
#undef DES_GM_FILL_
    if (missing) return 4;
    game->register_spu = des_spu_register_all;
#ifdef DES_GAME_HAS_LIBSRE
    game->load_prx_modules = ps3_load_prx_modules;
#endif
    return 0;
}
