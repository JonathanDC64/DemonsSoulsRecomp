// From Dusklight (CC0), src/dusk/ui/nav_types.hpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls (unchanged here).
#pragma once

namespace ui {

enum class NavCommand {
    None,
    Up,
    Down,
    Left,
    Right,
    Next,      // R1
    Previous,  // L1
    Confirm,   // A
    Cancel,    // B
    Menu,      // Back/Minus, or R + Start
};

}  // namespace ui
