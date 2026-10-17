// The startup work's window (prework_modal.hpp). FFCCEOT-Recomp's BuildModal, adapted for Demon's Souls.
#include "prework_modal.hpp"

#include "ui.hpp"

#include <algorithm>
#include <cstdio>

// des_prework.cpp (the executable): 1 while the startup work runs, with its progress and a line on what it is doing;
// skip stops it.
extern "C" int des_prework_status(float* fraction, char* text, int cap);
extern "C" void des_prework_skip(void);

namespace ui {

bool prework_running() { return des_prework_status(nullptr, nullptr, 0) != 0; }

PreworkModal::PreworkModal() : WindowSmall("modal") {
    auto* header = append(mDialog, "modal-header");
    append_text(append(header, "modal-title"), "Preparing the shaders");
    append(header, "icon")->SetClass("verifying", true);

    auto* body = append(mDialog, "modal-body");
    append_text(body,
        "Compiling the game's shaders for your graphics card, so the game does not stutter the first time it draws "
        "something. This takes under a minute and happens again only after an update.");
    auto* content = append(body, "verification-progress");
    mProgress = append(content, "progress");
    mProgress->SetClass("info", true);
    mProgress->SetAttribute("value", 0.f);
    mDetail = append(content, "small");

    auto* actions = append(mDialog, "modal-actions");
    mSkipButton = std::make_unique<Button>(actions, "Skip");
    mSkipButton->root()->SetClass("modal-btn", true);
    mSkipButton->on_pressed([this] { skip(); });
    refresh();
}

void PreworkModal::update() {
    if (!mFinished) {
        if (!prework_running()) {
            mFinished = true;
            pop();
        } else {
            refresh();
        }
    }
    WindowSmall::update();
}

bool PreworkModal::focus() { return mSkipButton != nullptr && mSkipButton->focus(); }

bool PreworkModal::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        skip();
        event.StopPropagation();
        return true;
    }
    if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
        return true;
    }
    return false;
}

void PreworkModal::skip() {
    if (mSkipRequested) return;
    mSkipRequested = true;
    des_prework_skip();
    mSkipButton->set_text("Skipping...");
    mSkipButton->set_disabled(true);
}

void PreworkModal::refresh() {
    float fraction = 0.0f;
    char text[160] = {};
    if (!des_prework_status(&fraction, text, int(sizeof text))) return;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    mProgress->SetAttribute("value", fraction);
    char line[200];
    std::snprintf(line, sizeof line, "%s (%.0f%%)", text, fraction * 100.0f);
    set_text_content(mDetail, line);
}

}  // namespace ui
