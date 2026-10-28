/* des_game_title.h: the port's side of the split build's boundary (DES_SPLIT, docs/57).
 *
 * DES_GAME_TITLE_FUNCS (des_game_title_funcs.h, which port/CMakeLists.txt
 * writes from scripts/lift_ppu.json's overrides and insn_hooks.txt): the port
 * functions the lifted code calls by name. The game DLL defines a jump thunk
 * for each (des_game_module.cpp) and asks the exe for them by name at the
 * load (des_game_title.c). A lift made with another hooks file fails the DLL's
 * link with the missing name; one in the list that the lift does not use
 * costs a thunk.
 *
 * DES_GAME_CALLS: the lifted functions the port's own code calls by name, by
 * guest address. In the exe each name is a forwarder through a pointer that
 * des_game_host.cpp resolves from the lift's function table at the load
 * (des_game_calls.h). A single exe declares the same names as the lift's own
 * functions there, since the port's sources include only ps3emu/ppu_lift.h. */
#pragma once

#define DES_GAME_CALLS(X)                                                        \
    X(00023750) X(000B14C8) X(000B15C8) X(0015AE10) X(001D17A0) X(001D35D8)      \
    X(00273BE0) X(002986B8) X(002D8978) X(002DD368) X(002EC2B8) X(004418B8)      \
    X(00595B80) X(0059E3A8) X(005A5920) X(0071A6B8) X(0071B5A0) X(0071B728)      \
    X(00720F50) X(00724AD0) X(0072D970) X(007731F0) X(00789BC0) X(0078E2E8)      \
    X(0078EB60) X(0078EDC8) X(0078F020) X(00798CD8) X(007A8C60) X(0081E110)      \
    X(008219E0) X(008CD078) X(008EF430) X(008F2A58) X(008FB4F0) X(008FD2A8)      \
    X(009E0148) X(00A8CA48) X(00A8CBF0) X(00B495B8) X(00B5CFA0) X(00B5D2F8)      \
    X(00B5D548) X(00F60BD0) X(00F62E68)
