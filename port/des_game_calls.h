/* des_game_calls.h: in the split build's exe (DES_SPLIT, docs/57), the lifted
 * functions the port calls by name are in the game DLL. Each name becomes a
 * forwarder in the exe that calls the DLL's function through a pointer,
 * resolved by guest address when the DLL loads (des_game_host.cpp). Calls,
 * function pointers, template arguments (gauge_fill_rate<func_...>) and
 * trampoline values all work through it. Include after ppu_recomp.h. A
 * single-exe build compiles none of this. */
#pragma once
#ifdef PS3_GAME_SPLIT
#include "des_game_title.h"

namespace des_game_calls {
#define DES_GAME_CALL_DECL_(a) void f_##a(ppu_context* ctx);
DES_GAME_CALLS(DES_GAME_CALL_DECL_)
#undef DES_GAME_CALL_DECL_
}

#define func_00023750 des_game_calls::f_00023750
#define func_0015AE10 des_game_calls::f_0015AE10
#define func_001D17A0 des_game_calls::f_001D17A0
#define func_001D35D8 des_game_calls::f_001D35D8
#define func_00273BE0 des_game_calls::f_00273BE0
#define func_002986B8 des_game_calls::f_002986B8
#define func_002EC2B8 des_game_calls::f_002EC2B8
#define func_004418B8 des_game_calls::f_004418B8
#define func_0071A6B8 des_game_calls::f_0071A6B8
#define func_00724AD0 des_game_calls::f_00724AD0
#define func_0072D970 des_game_calls::f_0072D970
#define func_00789BC0 des_game_calls::f_00789BC0
#define func_0078E2E8 des_game_calls::f_0078E2E8
#define func_0078EB60 des_game_calls::f_0078EB60
#define func_0078EDC8 des_game_calls::f_0078EDC8
#define func_0078F020 des_game_calls::f_0078F020
#define func_00798CD8 des_game_calls::f_00798CD8
#define func_007A8C60 des_game_calls::f_007A8C60
#define func_008CD078 des_game_calls::f_008CD078
#define func_008EF430 des_game_calls::f_008EF430
#define func_008F2A58 des_game_calls::f_008F2A58
#define func_008FB4F0 des_game_calls::f_008FB4F0
#define func_009E0148 des_game_calls::f_009E0148
#define func_00A8CA48 des_game_calls::f_00A8CA48
#define func_00A8CBF0 des_game_calls::f_00A8CBF0
#define func_00B495B8 des_game_calls::f_00B495B8
#define func_00B5CFA0 des_game_calls::f_00B5CFA0
#define func_00B5D2F8 des_game_calls::f_00B5D2F8
#define func_00B5D548 des_game_calls::f_00B5D548
#endif
