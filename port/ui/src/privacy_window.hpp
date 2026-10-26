// Written for Demon's Souls (docs/69 section 6). The privacy note's page (privacy.hpp), on the setup pages' shell:
// the note scrolls under the heading, its buttons stay in view. Ask: at start, before the game goes online with the
// project's server: Accept, or Decline (offline this time). Read: from Settings > Network, with Close.
//
// The pad: L1 and R1 scroll a page, Up and Down a few lines, Left and Right move between the buttons. Circle asks
// whether to play offline (Ask) or closes (Read).
#pragma once

#include "setup_page.hpp"

namespace ui {

class PrivacyWindow : public SetupPage {
public:
    enum class Mode { Ask, Read };
    explicit PrivacyWindow(Mode mode);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void back() override;
    bool scroll(int pages) override;
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void answer(bool accept);

    Mode mMode;
    Buttons mButtons;
};

}  // namespace ui
