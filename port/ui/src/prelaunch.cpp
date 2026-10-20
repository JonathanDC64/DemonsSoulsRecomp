// Adapted from Dusklight (CC0), src/dusk/ui/prelaunch.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: no ROM
// or disc to set up (the executable is built from the game already), so the screen is the title and three buttons over
// the game booting behind it.
#include "prelaunch.hpp"

#include "app.hpp"
#include "host.hpp"
#include "input.hpp"
#include "modal.hpp"
#include "settings.hpp"
#include "settings_window.hpp"
#include "trophies_window.hpp"
#include "ui.hpp"
#include "version.hpp"

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/prelaunch.rcss" />
</head>
<body>
    <prelaunch-gradient />
    <prelaunch-vignette />
    <content id="root" open>
        <menu>
            <hero class="intro-item delay-0">
                <title-text>Demon's Souls</title-text>
                <title-rule />
                <eyebrow>PC port</eyebrow>
            </hero>
            <menu-list id="menu-list" />
        </menu>
        <hint-info class="intro-item delay-5">
            <hint-label id="hint-text" />
        </hint-info>
        <version-info class="intro-item delay-6">
            <version-label>Version <version-number id="version-text"></version-number></version-label>
        </version-info>
    </content>
</body>
</rml>
)RML";

void apply_intro_animation(Rml::Element* element, const char* delay_class) {
    if (element == nullptr || delay_class == nullptr) {
        return;
    }
    element->SetClass("intro-item", true);
    element->SetClass(delay_class, true);
}

}  // namespace

Prelaunch::Prelaunch() : Document(kDocumentSource, false, DocumentScope::Prelaunch) {
    mRoot = mDocument->GetElementById("root");

    build_menu_buttons();

    mVersion = mDocument->GetElementById("version-text");
    mHint = mDocument->GetElementById("hint-text");

    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        auto* target = event.GetTargetElement();
        if (target == nullptr) {
            return;
        }
        if (target == mDocument && !mDocument->HasAttribute("open")) {
            Document::hide(true);
        } else if (target->GetTagName() == "button" && !target->IsClassSet("anim-done")) {
            target->SetClass("anim-done", true);
        }
    });
}

void Prelaunch::build_menu_buttons() {
    auto* menuList = mDocument->GetElementById("menu-list");
    if (menuList == nullptr) {
        return;
    }

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Play"));
    mMenuButtons.back()->on_pressed([this] { play(); });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-1");

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Settings"));
    mMenuButtons.back()->on_pressed([this] {
        play_sound(kSoundWindowOpen);
        push(std::make_unique<SettingsWindow>(true));
    });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-2");

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Trophies"));
    mMenuButtons.back()->on_pressed([this] {
        play_sound(kSoundWindowOpen);
        push(std::make_unique<TrophiesWindow>());
    });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-3");

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Quit"));
    mMenuButtons.back()->on_pressed([] { request_quit_confirm(); });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-4");
}

// Play: the game is already running behind this screen; hide it and hand over the input.
void Prelaunch::play() {
    play_sound(kSoundPlay);
    if (settings::get_int("menu_hint") != 0) {
        show_menu_notification();
    }
    hide(true);
}

void Prelaunch::show() {
    Document::show();
    mDocument->SetAttribute("open", "");
    mRoot->SetAttribute("open", "");
}

void Prelaunch::hide(bool close) {
    if (close) {
        if (!mEntranceAnimationStarted || !host::animations_enabled()) {
            // Close document immediately
            Document::hide(true);
        } else {
            mPendingClose = true;
        }
        mDocument->RemoveAttribute("open");
    } else {
        mRoot->RemoveAttribute("open");
    }
}

void Prelaunch::update() {
    if (!mEntranceAnimationStarted && mDocument != nullptr) {
        mDocument->SetClass("animate-in", true);
        mEntranceAnimationStarted = true;
    }
    for (const auto& button : mMenuButtons) {
        button->update();
    }
    if (mVersion != nullptr) {
        set_text_content(mVersion, kVersion);
    }
    if (mHint != nullptr) {
        set_text_content(mHint, Rml::String("In the game, F1 or ") +
                                    input::chord_label(settings::get_int("pad_menu_chord")) + " opens the menu.");
    }

    Document::update();
}

bool Prelaunch::focus() {
    if (mMenuButtons.empty()) {
        return false;
    }
    return mMenuButtons.front()->focus();
}

bool Prelaunch::visible() const {
    return mDocument->HasAttribute("open") && mRoot->HasAttribute("open");
}

bool Prelaunch::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    int direction = 0;
    if (cmd == NavCommand::Down) {
        direction = 1;
    } else if (cmd == NavCommand::Up) {
        direction = -1;
    } else if (cmd == NavCommand::Cancel) {
        // Circle / Escape on the start screen asks to quit, as a desktop program's start screen would.
        request_quit_confirm();
        return true;
    } else {
        return false;
    }
    auto* target = event.GetTargetElement();
    int focusedButton = -1;
    for (int i = 0; i < int(mMenuButtons.size()); ++i) {
        if (mMenuButtons[i]->contains(target)) {
            focusedButton = i;
            break;
        }
    }
    const auto n = static_cast<int>(mMenuButtons.size());
    int i = ((focusedButton + direction) % n + n) % n;
    while (i >= 0 && i < int(mMenuButtons.size())) {
        if (mMenuButtons[i]->focus()) {
            play_sound(kSoundItemFocus);
            event.StopPropagation();
            return true;
        }
        i += direction;
    }
    return false;
}

}  // namespace ui
