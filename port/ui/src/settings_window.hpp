// Adapted from Dusklight (CC0), src/dusk/ui/settings.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: the
// port's run options (settings.hpp), in tabs Display, Game, Controls, Interface and About.
#pragma once

#include "window.hpp"

namespace ui {

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false);

    void update() override;
    void hide(bool close) override;

protected:
    bool mPrelaunch;
};

}  // namespace ui
