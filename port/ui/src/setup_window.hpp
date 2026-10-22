// The setup screen (docs/88 section 2): while the setup engine runs, its phases with a mark each, the current step's
// bar and count, the whole run's bar and the time left, Details (the engine's last lines) and Stop. A run that fails
// opens a window that says what failed and what to do, with Try again (or Check again), Report and Close; a finished
// one offers Play, or Close. The game files' checklist, the folder pickers and the start screen's states are the next
// screens (docs/88 section 10).
#pragma once

#include "button.hpp"
#include "setup_engine.hpp"
#include "window.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

class Modal;

class SetupWindow : public WindowSmall {
public:
    struct Props {
        setup::Request request;         // empty: the folders the last run used
        std::function<void()> onPlay;   // the game is ready; without it, Close in place of Play
        std::function<void()> onClose;  // closed without playing
    };

    explicit SetupWindow(Props props);

    void update() override;
    bool focus() override;
    bool obscures_game() const override { return true; }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    enum class Phase { Running, Stopping, Stopped, Failed, Done };
    struct Action {
        Rml::String label;
        std::function<void()> onPressed;  // none: shown, but it cannot be pressed
    };

    void start();
    void stop_run();
    void finish();
    void close(bool play);
    void refresh(unsigned long long now);
    void build_steps();
    void set_actions(std::vector<Action> actions, const char* circle);
    void toggle_details();
    void ask_stop();
    void show_failure();
    void show_details(Modal& over);
    std::string send_report();
    void put(Rml::Element* element, std::string& shown, const std::string& text);

    Props mProps;
    setup::Status mStatus;
    setup::Eta mEta;
    Phase mPhase = Phase::Running;
    bool mDetailsShown = false;
    std::function<void()> mPending;  // a button's action, run in update() (it may rebuild the buttons)

    Rml::Element* mTitle = nullptr;
    Rml::Element* mIntro = nullptr;
    Rml::Element* mSteps = nullptr;
    std::vector<Rml::Element*> mStepRows;
    std::vector<std::string> mStepNames;
    std::vector<int> mStepStates;  // as drawn
    Rml::Element* mCurrent = nullptr;
    Rml::Element* mCurrentBar = nullptr;
    Rml::Element* mOverall = nullptr;
    Rml::Element* mOverallBar = nullptr;
    Rml::Element* mAwake = nullptr;
    Rml::Element* mDetails = nullptr;
    Rml::Element* mActions = nullptr;
    Rml::Element* mHints = nullptr;
    std::string mShownCurrent, mShownOverall, mShownAwake, mShownDetails;
    std::vector<std::unique_ptr<Button>> mButtons;
    setup::Engine mEngine;  // (last: destroyed first, which ends a run still going)
};

}  // namespace ui
