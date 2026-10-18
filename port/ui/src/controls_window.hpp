// Adapted from FFCCEOT-Recomp's controls_window.hpp, itself after Dusklight's ControllerConfigWindow (CC0,
// src/dusk/ui/controller_config.cpp): the pages on the left, the bindings of the chosen page on the right, and a
// binding is changed by pressing its row and then the button or key wanted. Adapted for Demon's Souls: the bindings are
// cellPad's PAD_MAP (gamepad) and PAD_KEYS (keyboard), kept in settings.ini.
#pragma once

#include "pane.hpp"
#include "window.hpp"

namespace ui {

class ControlsWindow : public Window {
public:
    ControlsWindow();

    void update() override;
    void hide(bool close) override;

private:
    enum class Page {
        Gamepad,
        Keyboard,
    };

    void build_tab(Rml::Element* content);
    void render_page(Pane& pane, Page page);
    void start_capture(int button, bool pad);
    void cancel_capture();
    bool capture_active() const { return mPendingButton >= 0; }
    Rml::String pending_label() const;

    Page mPage = Page::Gamepad;
    int mPendingButton = -1;  // the game button whose host button or key is being chosen
    bool mPendingPad = false;
    bool mArmed = false;      // nothing is held any more, so the next input is the choice
    unsigned long long mPendingStartedAt = 0;
};

}  // namespace ui
