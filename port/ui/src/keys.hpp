// Written for Demon's Souls. Virtual-key codes as RmlUi's keys, and the modifier keys held now, for the UI's input
// queue (des_ui.cpp). On Windows these are RmlUi's own Win32 helpers; off Windows (docs/76) the same mapping as a
// table, and SDL's modifier state.
#pragma once

#include <RmlUi/Core/Input.h>

namespace ui::keys {

Rml::Input::KeyIdentifier to_rml(int vk);
int modifiers();  // Rml::Input::KeyModifier bits

}  // namespace ui::keys
