// The setup screens after the build (see setup_options_window.hpp). The texts are the screens'; the values are the
// settings' (settings.hpp) and the presets' (setup_options.hpp).
#include "setup_options_window.hpp"

#include "osk_window.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include "setup_pc.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ui {
namespace {

// The settings "A few choices" sets, in its order (the online name last: it shows only while online play is on).
const char* const kChoiceKeys[] = {"DES_FULLSCREEN", "DES_FPS",   "RSX_SCALE",   "DES_ASPECT",
                                   "DES_MOUSE_LOOK", "DES_ONLINE", "DES_NET_NAME"};

// The frame rate as a number for the hints: the game's own 30 for "" and "30", far above any display for "unlocked".
int fps_number(const std::string& v) {
    if (v.empty()) return 30;
    if (v == "unlocked") return 100000;
    return std::atoi(v.c_str());
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

}  // namespace

std::string setup_options_summary() {
    setup::Preset preset = setup::Preset::Custom;
    setup::preset_from_key(settings::get("setup_preset"), preset);
    return setup::summary(preset, [](std::string_view key) { return settings::get(key); });
}

// ---- 1. How do you want to play? ----

SetupPresetWindow::SetupPresetWindow() : SetupPage("setup-preset"), mHeight(setup::display().height) {
    using S = RailStep::State;
    setup::Preset picked;
    if (setup::preset_from_key(settings::get("setup_preset"), picked)) mPick = picked;
    set_rail({{"Your game", "", S::Done}, {"Build", "Done", S::Done}, {"Options", "", S::Now}, {"Play", "", S::Todo}});
    set_heading("How do you want to play?",
                "A starting point. Every option can be changed later in Settings (F1 in the game).");

    const int matched = setup::matched_scale(mHeight);
    struct Card {
        setup::Preset preset;
        const char* title;
        const char* tag;  // or null
        const char* text;
        std::vector<std::string> lines;
    };
    const Card kCards[] = {
        {setup::Preset::Original, "Original", nullptr,
         "The game exactly as it was on the PS3. No enhancements at all.",
         {"30 fps", "720p, the game's own filtering",
          "Motion blur and depth of field as on the PS3", "16:9, in a window"}},
        {setup::Preset::Enhanced, "Enhanced", "Recommended", "Sharper and smoother, the same game.",
         {"60 fps, with the fixes for high frame rates",
          mHeight > 0 ? "Sharper picture, set for your display: " + setup::scale_label(matched)
                      : std::string("The picture at 720p (the display's size is not known)"),
          "Sharp distant textures and sharp scaling", "Fullscreen"}},
        {setup::Preset::Custom, "Custom", nullptr, "Starts from the settings you have now.",
         {"Pick the frame rate, resolution and more on the next page"}},
    };
    auto* cards = append(content(), "setup-choices");
    cards->SetClass("pick", true);
    for (const Card& c : kCards) {
        auto button = std::make_unique<ControlledButton>(
            cards, ControlledButton::Props{.text = "", .isSelected = [this, p = c.preset] { return mPick == p; }});
        Rml::Element* root = button->root();
        root->SetClass("setup-choice", true);
        text(root, "setup-choice-title", c.title);
        if (c.tag) text(root, "setup-tag", c.tag)->SetClass("info", true);
        text(root, "setup-choice-text", c.text);
        auto* list = append(root, "setup-choice-list");
        for (const std::string& line : c.lines) {
            auto* item = append(list, "setup-choice-item");
            append(item, "setup-dot");
            text(item, "setup-choice-item-text", line);
        }
        button->on_pressed([this, p = c.preset] {
            if (mPick == p) return;
            play_sound(kSoundItemChange);
            mPick = p;
        });
        mCards.push_back(std::move(button));
    }

    text(content(), "setup-heading", "What changes");
    auto* table = append(content(), "setup-cmp");
    const auto row = [&](const std::string& name, const std::string& original, const std::string& enhanced,
                         bool head) {
        auto* r = append(table, "setup-cmp-row");
        if (head) r->SetClass("head", true);
        text(r, "setup-cmp-cell", name)->SetClass("name", !head);
        text(r, "setup-cmp-cell", original);
        text(r, "setup-cmp-cell", enhanced)->SetClass("on", !head && original != enhanced);
    };
    row("Option", "Original", "Enhanced", true);
    for (const setup::Change& c : setup::preset_changes(mHeight))  // (only what differs, so the page fits 720 dp)
        if (c.original != c.enhanced) row(c.name, c.original, c.enhanced, false);

    set_buttons(mActions, footer_row(),
                {{"Continue", [this] { open_choices(); }, Look::Primary}, {"Back", [this] { back(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
}

bool SetupPresetWindow::focus() {
    if (!asking())
        for (const auto& card : mCards)
            if (card->selected() && card->focus()) return true;
    return SetupPage::focus();
}

std::vector<std::vector<Button*>> SetupPresetWindow::nav_lines() const {
    return {line(mCards), line(mActions)};
}

void SetupPresetWindow::back() {
    pop();
}

// Down from a card goes to Continue, not to the button below it (Back, under Enhanced); Up comes back to the picked
// card.
bool SetupPresetWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (!asking() && (cmd == NavCommand::Down || cmd == NavCommand::Up) && !mActions.empty()) {
        Rml::Element* target = event.GetTargetElement();
        const auto on = [target](const Buttons& buttons) {
            return std::any_of(buttons.begin(), buttons.end(), [target](const auto& b) { return b->contains(target); });
        };
        if (cmd == NavCommand::Down && on(mCards) && mActions.front()->focus()) {
            play_sound(kSoundItemFocus);
            return true;
        }
        if (cmd == NavCommand::Up && on(mActions)) {
            for (const auto& card : mCards) {
                if (!card->selected() || !card->focus()) continue;
                play_sound(kSoundItemFocus);
                return true;
            }
        }
    }
    return SetupPage::handle_nav_command(event, cmd);
}

// "A few choices", set from the preset picked: its values over the settings in use (Custom: those alone). Back there
// and Continue again with the same preset finds the choices as they were left.
void SetupPresetWindow::open_choices() {
    setup::Settings values;
    if (mHaveKept && mKeptFor == mPick) {
        values = mKept;
    } else {
        const setup::Settings preset = setup::preset_settings(mPick, mHeight);
        for (const char* key : kChoiceKeys) {
            std::string v = settings::get(key);
            for (const auto& [k, pv] : preset)
                if (!std::strcmp(k, key)) v = pv;
            values.emplace_back(key, std::move(v));
        }
    }
    push(std::make_unique<SetupChoicesWindow>(SetupChoicesWindow::Props{
        .preset = mPick,
        .values = std::move(values),
        .displayHeight = mHeight,
        .onBack =
            [this, pick = mPick](const setup::Settings& v) {
                mKept = v;
                mHaveKept = true;
                mKeptFor = pick;
            },
        .onFinish = [this] { force_hide(true); },  // (covered by A few choices: it closes without coming back)
    }));
}

// ---- 2. A few choices ----

SetupChoicesWindow::SetupChoicesWindow(Props props)
    : SetupPage("setup-few"), mProps(std::move(props)), mDisplay(setup::display()) {
    using S = RailStep::State;
    set_rail({{"Your game", "", S::Done}, {"Build", "Done", S::Done}, {"Options", "", S::Now}, {"Play", "", S::Todo}});
    const char* from = mProps.preset == setup::Preset::Original   ? "Set from Original."
                       : mProps.preset == setup::Preset::Enhanced ? "Set from Enhanced."
                                                                  : "Set from your settings.";
    set_heading("A few choices", std::string(from) + " Change any of them, or keep them as they are.");

    // The online name is never empty here: one is picked when there is none (or it is not valid), and said so.
    if (!settings::net_name_ok(value("DES_NET_NAME"))) {
        set_value("DES_NET_NAME", settings::suggest_net_name());
        mNameNote = "Picked for you; change it if you like.";
    }

    const setup::Display& d = mDisplay;
    auto* opts = append(content(), "setup-opts");
    add_opt(opts, "Display mode", "DES_FULLSCREEN", {{"0", "Window"}, {"1", "Fullscreen"}});

    std::vector<int> rates = {60, 120};
    if (d.hz > 60 && d.hz <= 500 && d.hz != 120) rates.push_back(d.hz);
    std::sort(rates.begin(), rates.end());
    std::vector<Seg> fps = {{"", "30"}};
    for (const int r : rates) fps.push_back({std::to_string(r), std::to_string(r)});
    const std::string fpsNow = value("DES_FPS");
    if (fpsNow != "30" && std::none_of(fps.begin(), fps.end(), [&](const Seg& s) { return s.value == fpsNow; }))
        fps.push_back({fpsNow, fpsNow == "unlocked" ? "Unlocked" : fpsNow});
    add_opt(opts, "Frame rate", "DES_FPS", std::move(fps));

    std::vector<Seg> scales;
    for (const int s : {100, 150, 200, 300}) scales.push_back({std::to_string(s), setup::scale_label(s)});
    const std::string scaleNow = value("RSX_SCALE");
    if (std::none_of(scales.begin(), scales.end(), [&](const Seg& s) { return s.value == scaleNow; }))
        scales.push_back({scaleNow, setup::scale_label(std::atoi(scaleNow.c_str()))});
    add_opt(opts, "Render resolution", "RSX_SCALE", std::move(scales));

    std::vector<Seg> aspects = {{"", "16:9"}, {"auto", "Match the display"}};
    const std::string aspectNow = value("DES_ASPECT");
    if (aspectNow != "" && aspectNow != "auto") {
        static const struct {
            const char* value;
            const char* label;
        } kAspects[] = {{"2560:1080", "21:9 (2560x1080)"},
                        {"3440:1440", "21:9 (3440x1440)"},
                        {"3840:1600", "21:9 (3840x1600)"},
                        {"32:9", "32:9"}};  // (the Aspect ratio row's, settings_window.cpp)
        std::string label = aspectNow;
        for (const auto& a : kAspects)
            if (aspectNow == a.value) label = a.label;
        aspects.push_back({aspectNow, label});
    }
    add_opt(opts, "Aspect ratio", "DES_ASPECT", std::move(aspects));

    add_opt(opts, "Controls", "DES_MOUSE_LOOK", {{"0", "Gamepad"}, {"1", "Keyboard and mouse"}});
    add_opt(opts, "Online play", "DES_ONLINE", {{"1", "On"}, {"0", "Off"}});

    // Under Online play: the name, with Change name (the on-screen keyboard; a keyboard types into it too).
    Rml::Element* online = mOpts.back().hint->GetParentNode();
    mNameBlock = append(online, "setup-opt-more");
    text(mNameBlock, "setup-opt-label", "Online name")->SetClass("sub", true);
    auto* nameRow = append(mNameBlock, "setup-name");
    mNameText = text(nameRow, "setup-name-text", value("DES_NET_NAME"));
    auto* nameButtonRow = append(nameRow, "setup-name-button");
    set_buttons(mNameButton, nameButtonRow, {{"Change name", [this] { change_name(); }, Look::Plain}});
    for (const auto& b : mNameButton) b->root()->SetClass("small", true);
    mNameHint = text(mNameBlock, "setup-opt-hint", "");

    set_buttons(mActions, footer_row(),
                {{"Finish", [this] { finish(); }, Look::Primary}, {"Back", [this] { back(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
    draw_hints();
}

std::string SetupChoicesWindow::value(const char* key) const {
    for (const auto& [k, v] : mProps.values)
        if (!std::strcmp(k, key)) return v;
    return settings::get(key);
}

void SetupChoicesWindow::set_value(const char* key, const std::string& v) {
    for (auto& [k, old] : mProps.values) {
        if (std::strcmp(k, key)) continue;
        old = v;
        return;
    }
    mProps.values.emplace_back(key, v);
}

// An option's block: its name, its choices side by side (one picked), and a line about it (draw_hints).
void SetupChoicesWindow::add_opt(Rml::Element* parent, const char* label, const char* key, std::vector<Seg> segs) {
    auto* block = append(parent, "setup-opt");
    text(block, "setup-opt-label", label);
    auto* seg = append(block, "setup-seg");
    Opt opt{key};
    for (Seg& s : segs) {
        // DES_FPS "30" (30 with the real frame time, from Settings) shows as 30 too.
        const auto picked = [this, key, v = s.value] {
            const std::string now = value(key);
            return now == v || (v.empty() && !std::strcmp(key, "DES_FPS") && now == "30");
        };
        auto button = std::make_unique<ControlledButton>(
            seg, ControlledButton::Props{.text = s.label, .isSelected = picked});
        button->root()->SetClass("setup-seg-btn", true);
        button->on_pressed([this, key, v = s.value, picked] {
            if (picked()) return;
            play_sound(kSoundItemChange);
            set_value(key, v);
            draw_hints();
        });
        opt.segs.push_back(std::move(button));
    }
    opt.hint = text(block, "setup-opt-hint", "");
    mOpts.push_back(std::move(opt));
}

// The lines under the options, which follow their values and this PC's display; and the name, shown while online
// play is on.
void SetupChoicesWindow::draw_hints() {
    const setup::Display& d = mDisplay;
    for (Opt& o : mOpts) {
        const std::string v = value(o.key);
        std::string hint;
        if (!std::strcmp(o.key, "DES_FULLSCREEN")) {
            hint = "Alt+Enter switches at any time.";
        } else if (!std::strcmp(o.key, "DES_FPS")) {
            if (d.hz <= 0) hint = "30 is the game's own.";
            else if (fps_number(v) > d.hz)
                hint = "Your display runs at " + std::to_string(d.hz) + " Hz, so it shows at most " +
                       std::to_string(d.hz) + " frames a second.";
            else hint = "Your display runs at " + std::to_string(d.hz) + " Hz.";
        } else if (!std::strcmp(o.key, "RSX_SCALE")) {
            if (d.width <= 0 || d.height <= 0) hint = "Higher is sharper and costs more GPU time.";
            else if (720 * std::atoi(v.c_str()) / 100 > d.height)
                hint = "Your display is " + std::to_string(d.width) + "x" + std::to_string(d.height) +
                       ". Higher than that costs GPU time for little gain.";
            else hint = "Your display is " + std::to_string(d.width) + "x" + std::to_string(d.height) + ".";
        } else if (!std::strcmp(o.key, "DES_ASPECT")) {
            const float shape = d.height > 0 ? float(d.width) / float(d.height) : 0.f;
            if (shape <= 0.f) hint = "Match the display fills a wide display.";
            else if (std::fabs(shape - 16.f / 9.f) < 0.02f) hint = "Your display is 16:9, so both look the same.";
            else if (shape > 16.f / 9.f) hint = "Your display is wider: Match the display shows more at the sides.";
            else hint = "Your display is narrower, so the picture stays 16:9.";
        } else if (!std::strcmp(o.key, "DES_MOUSE_LOOK")) {
            hint = v == "1" ? "Mouse look, and the keys of the Souls games on PC. Change any key in Settings."
                            : "The keyboard works as well. Buttons show as PS3 buttons.";
        } else if (!std::strcmp(o.key, "DES_ONLINE")) {
            hint = v == "0" ? "No messages, bloodstains or phantoms."
                            : "Messages, bloodstains and phantoms from other players.";
        }
        set_text_content(o.hint, hint);
    }
    const bool online = value("DES_ONLINE") != "0";
    set_display(mNameBlock, online ? Rml::Style::Display::Flex : Rml::Style::Display::None);
    set_text_content(mNameText, value("DES_NET_NAME"));
    set_text_content(mNameHint,
                     (mNameNote.empty() ? "" : mNameNote + " ") + "3 to 16 letters, digits, - or _. Not your PSN name.");
}

void SetupChoicesWindow::change_name() {
    const std::string name = value("DES_NET_NAME");
    push_document(std::make_unique<OskWindow>(OskWindow::Props{
        .prompt = "Your name online",
        .text = std::u16string(name.begin(), name.end()),  // (letters, digits, - and _ only)
        .limit = 16,
        .onDone =
            [this](bool ok, const std::u16string& typed) {
                if (ok) name_typed(utf16_to_utf8(typed));
                mRefocusName = true;  // (the keyboard gives the focus to the page's first button as it closes)
            },
    }));
}

// A name that is not valid keeps the one there, and says so: online play always has a name.
void SetupChoicesWindow::name_typed(const std::string& typed) {
    const std::string name = trim(typed);
    if (settings::net_name_ok(name)) {
        set_value("DES_NET_NAME", name);
        mNameNote.clear();
    } else {
        mNameNote = std::string(name.empty() ? "Online play needs a name"
                                             : "Names are 3 to 16 letters, digits, - or _") +
                    ", so " + value("DES_NET_NAME") + " was kept.";
    }
    draw_hints();
}

bool SetupChoicesWindow::focus() {
    if (!asking() && !mOpts.empty())
        for (const auto& seg : mOpts.front().segs)
            if (seg->selected() && seg->focus()) return true;
    return SetupPage::focus();
}

std::vector<std::vector<Button*>> SetupChoicesWindow::nav_lines() const {
    std::vector<std::vector<Button*>> lines;
    for (const Opt& o : mOpts) {
        lines.push_back(line(o.segs));
        if (!std::strcmp(o.key, "DES_ONLINE") && value("DES_ONLINE") != "0") lines.push_back(line(mNameButton));
    }
    lines.push_back(line(mActions));
    return lines;
}

void SetupChoicesWindow::tick() {
    if (std::exchange(mRefocusName, false) && !mNameButton.empty()) mNameButton[0]->focus();
}

void SetupChoicesWindow::back() {
    if (mProps.onBack) mProps.onBack(mProps.values);
    pop();
}

// The preset's settings, then the choices over them, then the preset: the game reads them all when it starts.
void SetupChoicesWindow::finish() {
    for (const auto& [key, v] : setup::preset_settings(mProps.preset, mProps.displayHeight)) settings::store(key, v);
    for (const auto& [key, v] : mProps.values) settings::store(key, v);
    settings::store("setup_preset", setup::preset_key(mProps.preset));
    std::fprintf(stderr, "[setup] options: %s\n", setup_options_summary().c_str());
    if (mProps.onFinish) mProps.onFinish();
    pop();
}

}  // namespace ui
