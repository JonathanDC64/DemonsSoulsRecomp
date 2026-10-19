// Adapted from FFCCEOT-Recomp's controls_window.hpp, itself after Dusklight's ControllerConfigWindow (CC0,
// src/dusk/ui/controller_config.cpp): the pages on the left, the bindings of the chosen page on the right, and a
// binding is changed by pressing its row and then the button or key wanted. Adapted for Demon's Souls: the bindings are
// cellPad's PAD_MAP (gamepad) and PAD_KEYS (keyboard and mouse), kept in settings.ini.
#pragma once

#include "pane.hpp"
#include "window.hpp"

#include <array>

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
    void render_pad_page(Pane& pane);
    void render_key_page(Pane& pane);
    void start_capture(int button, bool pad, int slot);
    void cancel_capture();
    bool capture_active() const { return mPendingButton >= 0; }
    Rml::String pending_label() const;
    void update_pad_capture();
    void update_key_capture();
    bool key_pressed(int code, int mod);
    void bind_key(int in, int mod);

    Page mPage = Page::Gamepad;
    int mPendingButton = -1;  // the game button (gamepad page) or action (keyboard page) whose input is being chosen
    bool mPendingPad = false;
    int mPendingSlot = 0;     // the action's row (keyboard page)
    bool mArmed = false;      // nothing is held any more, so the next input is the choice
    unsigned long long mPendingStartedAt = 0;
    std::array<bool, 256> mDown{};  // the keys and mouse buttons held at the last look (keyboard page)
    int mModDown = 0;               // the Shift, Ctrl or Alt pressed first during the capture (its left or right key)
};

}  // namespace ui
