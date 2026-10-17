// The startup work's window: while the game prepares (the shader cache, des_prework.cpp), before it runs, a bar and
// what is being done, with a Skip button. FFCCEOT-Recomp's BuildModal (game/src/ui/rom_setup.cpp), adapted for
// Demon's Souls.
#pragma once

#include "button.hpp"
#include "window.hpp"

#include <memory>

namespace ui {

class PreworkModal : public WindowSmall {
public:
    PreworkModal();

    void update() override;
    bool focus() override;
    bool obscures_game() const override { return true; }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void skip();
    void refresh();

    Rml::Element* mProgress = nullptr;
    Rml::Element* mDetail = nullptr;
    std::unique_ptr<Button> mSkipButton;
    bool mSkipRequested = false;
    bool mFinished = false;
};

// The startup work is running (des_prework_status).
bool prework_running();

}  // namespace ui
