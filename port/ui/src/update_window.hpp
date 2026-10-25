// Written for Demon's Souls. The update's screen (docs/105), on the setup shell (setup_page.hpp): a newer release's
// notes with Update, Later and Skip this version; then the update itself, the engine's update-apply (the download, its
// check against the release's SHA256SUMS, the swap of the program files), after which the program starts again as the
// new version, straight into setup's rebuild (DES_UI_OPEN=setup-run). Or going back to the version the last update
// kept (update-back), after which the program starts again as that one. Over the game (its start screen), Update and
// Go back start the program again first, with setup's screens alone (updates.hpp's restart_into): a running game cannot
// replace its own files.
#pragma once

#include "setup_engine.hpp"
#include "setup_page.hpp"
#include "updates.hpp"

#include <string>
#include <vector>

namespace ui {

class UpdateWindow : public SetupPage {
public:
    enum class Open {
        Notes,   // the release's notes, and what to do about it
        Update,  // the update at once (DES_UI_OPEN=update-run)
        Back,    // going back at once (DES_UI_OPEN=update-back)
    };
    struct Props {
        Open open = Open::Notes;
        updates::Release release;  // Notes: the release; Update: its version at least
        bool overGame = false;     // the game runs below: Update starts the program again first
    };

    explicit UpdateWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;
    bool scroll(int pages) override;

private:
    enum class Phase { Notes, Running, Failed, Done };

    void draw_notes();
    void update_now();
    void start();
    void draw_running();
    void draw_failed();
    void finish();
    void refresh();
    void close();
    void ask_stop();

    Props mProps;
    bool mBack = false;  // this run goes back (update-back), not forward
    Phase mPhase = Phase::Notes;
    setup::Status mStatus;
    Rml::Element* mBody = nullptr;
    Rml::Element* mPercent = nullptr;
    Rml::Element* mWhat = nullptr;
    Rml::Element* mBar = nullptr;
    std::string mShownPercent, mShownWhat;
    bool mShownStop = false;  // Stop is up (only while it downloads: the swap is not to be cut short)
    Buttons mMain, mMore;
    setup::Engine mEngine;  // (last: destroyed first, which ends a run still going)
};

}  // namespace ui
