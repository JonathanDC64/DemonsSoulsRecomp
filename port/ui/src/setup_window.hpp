// The setup screen (docs/88 section 2, docs/89): while the setup engine runs, the whole run's percent, the time left
// and its bar, then the build's tasks, each with its mark and count; Stop (which asks first) and Details (the engine's
// last lines). A run that fails says what failed and what to do, with the engine's last lines, Try again (or Check
// again), Report the problem and Show the full log; a stopped one offers Resume; a finished one, Play. The page is
// the setup shell's (setup_page.hpp). The game files' checklist, the folder pickers and the start screen's states are
// the next screens (docs/88 section 10).
#pragma once

#include "setup_engine.hpp"
#include "setup_page.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ui {

class SetupWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;         // empty: the folders the last run used
        std::function<void()> onPlay;   // the game is ready; without it, Close in place of Play
        std::function<void()> onClose;  // closed without playing
    };

    explicit SetupWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    enum class Phase { Running, Stopping, Stopped, Failed, Done };
    // A line of the task list: a phase of the plan, or one of its steps (the lift's two).
    struct Task {
        std::string id;     // "check", "ppu", ...
        size_t phase = 0;   // its phase in the plan
        int state = -1;     // as drawn (Status::State)
        std::string count;  // as drawn
        Rml::Element* row = nullptr;
        Rml::Element* mark = nullptr;
        Rml::Element* sub = nullptr;
        Rml::Element* num = nullptr;
    };

    void start();
    void stop_run();
    void finish();
    void close(bool play);
    void draw_running();
    void draw_failed();
    void draw_stopped();
    void draw_done();
    void draw_rail(const std::string& buildSub);
    void build_tasks();
    void refresh(unsigned long long now);
    void set_actions(std::vector<Action> actions, const std::string& note = {});
    void toggle_details();
    void ask_stop();
    void ask_report();
    void open_log();
    std::filesystem::path log_file() const;
    std::string log_tail(size_t lines) const;
    std::string send_report();
    void put(Rml::Element* element, std::string& shown, const std::string& text);

    Props mProps;
    setup::Status mStatus;
    setup::Eta mEta;
    Phase mPhase = Phase::Running;
    bool mDetailsShown = false;
    unsigned long long mStartMs = 0;

    Rml::Element* mBody = nullptr;  // the page's parts under the heading, drawn again for each phase
    Rml::Element* mPercent = nullptr;
    Rml::Element* mEtaText = nullptr;
    Rml::Element* mBar = nullptr;
    Rml::Element* mTasks = nullptr;
    Rml::Element* mDetails = nullptr;
    Rml::Element* mActions = nullptr;
    Rml::Element* mNote = nullptr;
    std::vector<Task> mTaskRows;
    std::vector<std::string> mTaskPlan;  // the plan the rows were made for
    std::string mShownPercent, mShownEta, mShownNote, mShownDetails, mShownRail;
    Buttons mButtons;
    setup::Engine mEngine;  // (last: destroyed first, which ends a run still going)
};

}  // namespace ui
