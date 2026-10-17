// What the UI needs from the port (des_ui.cpp fills it in): what its buttons do to the game. From FFCCEOT-Recomp's
// app.hpp, cut down for Demon's Souls (the settings themselves are in settings.hpp).
#pragma once

#include <functional>

namespace ui {

struct App {
    std::function<void()> quit;  // close the game (the window's close message)
};

App& app();

}  // namespace ui
