// Written for Demon's Souls. The setup screens after the build (docs/91), on the setup shell (setup_page.hpp), as the
// setup design draws them:
//  1. "How do you want to play?": three presets as cards (Original, Enhanced, Custom) and what Enhanced changes;
//  2. "A few choices": display mode, frame rate, render resolution, aspect ratio, controls and online play with the
//     online name, each set from the preset and changed in place.
// Nothing is written until Finish, which stores the preset's settings, the choices over them and the preset itself
// (setup_preset) in settings.ini (settings::store: the game reads them when it starts) and closes both pages. The page
// that opened them (the progress screen's end, or the start screen) then draws Ready to play.
#pragma once

#include "button.hpp"
#include "setup_options.hpp"
#include "setup_page.hpp"
#include "setup_pc.hpp"

#include <functional>
#include <string>

namespace ui {

// Ready to play's Options line, from the settings: "Enhanced: 60 fps, 1440p, fullscreen, gamepad, online on."
std::string setup_options_summary();

class SetupPresetWindow : public SetupPage {
public:
    SetupPresetWindow();

    bool focus() override;  // the picked card

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void back() override;
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void open_choices();

    setup::Preset mPick = setup::Preset::Enhanced;
    int mHeight = 0;           // the display's, for Enhanced's render resolution
    setup::Settings mKept;     // "A few choices" as its Back left them, for mKeptFor
    bool mHaveKept = false;
    setup::Preset mKeptFor = setup::Preset::Enhanced;
    Buttons mCards, mActions;
};

class SetupChoicesWindow : public SetupPage {
public:
    struct Props {
        setup::Preset preset;
        setup::Settings values;  // the choices to begin with (kChoiceKeys, setup_options_window.cpp)
        int displayHeight = 0;
        std::function<void(const setup::Settings&)> onBack;  // Back: the choices as they are now
        std::function<void()> onFinish;                      // Finish: the options are written
    };

    explicit SetupChoicesWindow(Props props);

    bool focus() override;  // the first option's picked choice

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    // An option: its setting and its choices, as segments.
    struct Seg {
        std::string value;
        std::string label;
    };
    struct Opt {
        const char* key;
        Rml::Element* hint = nullptr;
        Buttons segs;
    };

    void add_opt(Rml::Element* parent, const char* label, const char* key, std::vector<Seg> segs);
    void draw_hints();
    void change_name();
    void name_typed(const std::string& typed);
    void finish();
    std::string value(const char* key) const;
    void set_value(const char* key, const std::string& v);

    Props mProps;
    setup::Display mDisplay;  // this PC's, for the hints
    std::vector<Opt> mOpts;
    Rml::Element* mNameBlock = nullptr;
    Rml::Element* mNameText = nullptr;
    Rml::Element* mNameHint = nullptr;
    std::string mNameNote;  // why the name is what it is ("Picked for you."), or none
    bool mRefocusName = false;
    Buttons mNameButton, mActions;
};

}  // namespace ui
