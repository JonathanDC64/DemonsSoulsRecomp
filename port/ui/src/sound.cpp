// The UI's sounds (see sound.hpp): routed to g_des_ui_sound (des_ui.h), which the port points at the game's own menu
// sounds. Silent until it is set. Adapted for Demon's Souls from FFCCEOT-Recomp's sound.cpp.
#include "sound.hpp"

#include "des_ui.h"

extern "C" void (*g_des_ui_sound)(int id) = nullptr;

namespace ui {

void play_sound(Sound sound) noexcept {
    if (auto* fn = g_des_ui_sound) fn(static_cast<int>(sound));
}

}  // namespace ui
