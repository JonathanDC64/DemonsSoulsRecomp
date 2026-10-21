// Written for Demon's Souls. Virtual-key codes as RmlUi's keys, the modifier keys held now, the keys held now and
// their names, for the UI's input queue (des_ui.cpp) and the controls window. On Windows these are RmlUi's own Win32
// helpers and Windows' key state; off Windows (docs/76) the same mapping as a table, and SDL's state.
#pragma once

#include <RmlUi/Core/Input.h>

#include <string>

namespace ui::keys {

Rml::Input::KeyIdentifier to_rml(int vk);
int modifiers();  // Rml::Input::KeyModifier bits

// The key (or mouse button) `vk` is held now. Any thread.
bool key_down(int vk);
// The key's name on the keyboard (Windows asks the keyboard layout; elsewhere the US keyboard's names), or empty.
std::string key_name(int vk);

}  // namespace ui::keys
