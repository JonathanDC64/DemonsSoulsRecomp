// Written for Demon's Souls. See privacy_window.hpp.
#include "privacy_window.hpp"

#include "privacy.hpp"
#include "settings.hpp"
#include "ui.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace ui {
namespace {

constexpr auto kOfflineToast = std::chrono::seconds(15);

// Up and Down: a few lines; L1 and R1: most of a page.
bool scroll_by(Rml::Element* page, float fraction, int steps) {
    const float step = std::max(24.f, page->GetClientHeight() * fraction);
    page->SetScrollTop(std::max(0.f, page->GetScrollTop() + step * float(steps)));
    return true;
}

}  // namespace

PrivacyWindow::PrivacyWindow(Mode mode) : SetupPage("setup-privacy"), mMode(mode) {
    using S = RailStep::State;
    const std::string v = "Version " + std::to_string(privacy::version()) + ".";
    if (mMode == Mode::Ask) {
        set_rail({{"Privacy note", "", S::Now}, {"Play", "", S::Todo}});
        set_heading("Privacy note", "Before the game goes online, here is what this project's server keeps about you "
                                    "and for how long. " + v + " Decline plays offline this time.");
    } else {
        set_rail({{"Privacy note", "", S::Now}});
        const bool accepted = settings::get_int("privacy_accepted") >= privacy::version();
        set_heading("Privacy note", "What this project's online server keeps about you, and for how long. " + v +
                                        (accepted ? " You accepted it." : ""));
    }
    auto* box = panel(append(content(), "setup-body"), true);
    box->SetClass("update-notes", true);
    for (const privacy::Part& part : privacy::parts()) {
        switch (part.kind) {
        case privacy::Part::Kind::Heading:
            text(box, "update-note-head", part.text);
            break;
        case privacy::Part::Kind::Item:
            text(box, "update-note-item", "\xE2\x80\xA2 " + part.text);
            break;
        case privacy::Part::Kind::Text:
            text(box, "update-note-text", part.text);
            break;
        }
    }
    if (mMode == Mode::Ask) {
        set_buttons(mButtons, footer_row(),
                    {{"Accept", [this] { answer(true); }, Look::Primary},
                     {"Decline", [this] { answer(false); }, Look::Quiet}});
        set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Decline"}, {nullptr, "L1 R1", "Scroll"}});
    } else {
        set_buttons(mButtons, footer_row(), {{"Close", [this] { pop(); }, Look::Primary}});
        set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Close"}, {nullptr, "L1 R1", "Scroll"}});
    }
}

void PrivacyWindow::answer(bool accept) {
    play_sound(accept ? kSoundPlay : kSoundClick);
    privacy::answer(accept);
    if (!accept) {
        push_toast({
            .type = "warning",
            .title = "Playing offline",
            .content = "You declined the privacy note, so this session is offline. The game asks again at its next "
                       "start; to stop that, turn Online off in Settings > Network.",
            .duration = kOfflineToast,
        });
    }
    pop();
}

std::vector<std::vector<Button*>> PrivacyWindow::nav_lines() const {
    return {line(mButtons)};
}

// Circle: Read closes; Ask makes sure first, since Decline means no online play this time.
void PrivacyWindow::back() {
    if (mMode == Mode::Read) {
        pop();
        return;
    }
    ask("Play offline?", "The game goes on offline this time, and asks again at its next start.",
        {{"Play offline", [this] { answer(false); }, Look::Plain}, {"Back to the note", [] {}, Look::Primary}}, 1);
}

bool PrivacyWindow::scroll(int pages) {
    return scroll_by(content(), 0.8f, pages);
}

bool PrivacyWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    // One line of buttons: Up and Down have nowhere to go, so they scroll the note.
    if (!asking() && (cmd == NavCommand::Up || cmd == NavCommand::Down))
        return scroll_by(content(), 0.15f, cmd == NavCommand::Down ? 1 : -1);
    return SetupPage::handle_nav_command(event, cmd);
}

}  // namespace ui
