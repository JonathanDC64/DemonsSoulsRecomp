// The setup screen (see setup_window.hpp). The small modal window, as the startup shader window (prework_modal.cpp),
// with the phases, two bars, Details, a row of buttons and the pad's buttons under them. Left and right move between
// the buttons, Cross presses one, Circle asks to stop a run (or closes the window once it has ended). The texts are
// the screen's; the engine's lines say only which phase and step, and what failed (setup_protocol.hpp).
#include "setup_window.hpp"

#include "modal.hpp"
#include "portable.hpp"
#include "report.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace ui {
namespace {

// What the player can do about each kind of failure, and the failure window's second sentence: protocol.py's ERRORS
// (tools/setup/tests/test_pipeline.py checks that the two lists agree). The first sentence, what failed, is the
// engine's.
enum class Fix { CheckAgain, TryAgain, Report };
struct ErrorKind {
    const char* code;
    Fix fix;
    const char* todo;
};
constexpr ErrorKind kErrors[] = {
    {"game", Fix::CheckAgain, "Then check again."},
    {"busy", Fix::TryAgain, "Wait until it has finished, then try again."},
    {"disk", Fix::TryAgain, "Free some space, then try again. Setup carries on where it stopped."},
    {"download", Fix::TryAgain,
     "Check the internet connection, then try again. The download carries on where it stopped."},
    {"memory", Fix::TryAgain, "Close other programs, then try again. Setup carries on where it stopped."},
    {"step", Fix::Report, "Try again. If it fails again, please report it."},
    {"internal", Fix::Report, "Try again. If it fails again, please report it."},
};

const ErrorKind& error_kind(const std::string& code) {
    for (const ErrorKind& k : kErrors)
        if (code == k.code) return k;
    return kErrors[std::size(kErrors) - 1];  // (an unknown code: as "internal")
}

// The phases' names on screen.
std::string phase_label(const std::string& name) {
    static const struct {
        const char* name;
        const char* label;
    } kLabels[] = {
        {"check", "Check the game files"},      {"fetch", "Download the tools"}, {"stage", "Copy the game files"},
        {"lift", "Convert the game's code"},    {"compile", "Build the game"},  {"shaders", "Prepare the shaders"},
    };
    for (const auto& l : kLabels)
        if (name == l.name) return l.label;
    return name;
}

// A phase's row as drawn, by Status::State (Todo, Running, Done, Skipped, Failed).
struct StepLook {
    const char* row;
    const char* icon;
    const char* badgeClass;
    const char* badge;
};
constexpr StepLook kLooks[] = {
    {"todo", "step-todo", "", ""},
    {"running", "step-running", "info", ""},
    {"done", "step-done", "success", "Done"},
    {"done", "step-done", "success", "Already done"},
    {"failed", "step-failed", "error", "Failed"},
};

std::string grouped(long long v) {
    const std::string digits = std::to_string(std::max(0LL, v));
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// The current step's count in words: "1,200 of 94,017 functions", "120 of 1,376 MB".
std::string count_text(const setup::Status& s) {
    if (s.progressM <= 0) return {};
    if (s.progress == "check" || s.progress == "download")
        return grouped(s.progressN >> 20) + " of " + grouped(std::max(1LL, s.progressM >> 20)) + " MB";
    static const struct {
        const char* step;
        const char* noun;
    } kNouns[] = {{"ppu", "functions"}, {"spu", "programs"}, {"compile", "files"}};
    for (const auto& n : kNouns)
        if (s.progress == n.step) return grouped(s.progressN) + " of " + grouped(s.progressM) + " " + n.noun;
    return grouped(s.progressN) + " of " + grouped(s.progressM);
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// One entry of the pad's row: the button, from the icon font, and what it does.
void add_hint(Rml::Element* row, const char* button, const char* action) {
    auto* item = append(row, "setup-hint");
    append(item, "icon")->SetClass(Rml::String("pad-") + button, true);
    append_text(item, action);
}

}  // namespace

SetupWindow::SetupWindow(Props props) : WindowSmall("modal"), mProps(std::move(props)) {
    mRoot->SetClass("setup", true);
    auto* header = append(mDialog, "modal-header");
    mTitle = append(header, "modal-title");
    append(header, "icon")->SetClass("verifying", true);
    mIntro = append(mDialog, "modal-body");
    mSteps = append(mDialog, "setup-steps");

    auto* bars = append(mDialog, "verification-progress");
    mCurrent = append(bars, "file-path");
    mCurrentBar = append(bars, "progress");
    mCurrentBar->SetClass("info", true);
    mCurrentBar->SetAttribute("value", 0.f);
    mOverall = append(bars, "small");
    mOverallBar = append(bars, "progress");
    mOverallBar->SetAttribute("value", 0.f);
    mAwake = append(bars, "small");

    mDetails = append(mDialog, "setup-details");
    set_display(mDetails, Rml::Style::Display::None);
    mActions = append(mDialog, "modal-actions");
    mHints = append(mDialog, "setup-hints");
    play_sound(kSoundWindowOpen);
    start();
}

void SetupWindow::start() {
    mStatus = {};
    mEta.reset();
    mPhase = Phase::Running;
    mDetailsShown = false;
    set_display(mDetails, Rml::Style::Display::None);
    set_text_content(mTitle, "Setting up the game");
    set_text_content(mIntro, "The first time takes a while. You can stop and carry on later.");
    set_actions({{"Details", [this] { toggle_details(); }}, {"Stop", [this] { ask_stop(); }}}, "Stop");
    if (!mEngine.start(mProps.request)) {  // (shown by the next update, once this window is up)
        mStatus.result = "failed";
        mStatus.errorCode = "internal";
        mStatus.errorStep = "-";
        mStatus.errorText = mEngine.why();
    }
}

void SetupWindow::stop_run() {
    if (mPhase != Phase::Running) return;
    mPhase = Phase::Stopping;
    mEngine.stop();
    set_actions({{"Stopping...", {}}}, nullptr);
}

void SetupWindow::finish() {
    const std::string& r = mStatus.result;
    if (r == "ok") {
        mPhase = Phase::Done;
        const bool ready = setup::launch_state().state == setup::State::Ready;
        set_text_content(mTitle, ready ? "The game is ready" : "Setup finished");
        set_text_content(mIntro,
                         ready ? "Setup is done." : "This version has no build step yet, so the game is not built.");
        if (ready && mProps.onPlay) set_actions({{"Play", [this] { close(true); }}}, "Close");
        else set_actions({{"Close", [this] { close(false); }}}, "Close");
    } else if (r == "stopped") {
        mPhase = Phase::Stopped;
        set_text_content(mTitle, "Setup stopped");
        set_text_content(mIntro, "It carries on from here the next time.");
        set_actions({{"Resume", [this] { start(); }}, {"Close", [this] { close(false); }}}, "Close");
    } else {
        mPhase = Phase::Failed;
        set_text_content(mTitle, "Setup did not finish");
        set_text_content(mIntro, mStatus.errorText);
        set_actions({{error_kind(mStatus.errorCode).fix == Fix::CheckAgain ? "Check again" : "Try again",
                      [this] { start(); }},
                     {"Close", [this] { close(false); }}},
                    "Close");
        show_failure();
    }
    std::fprintf(stderr, "[setup] the run ended: %s%s%s\n", r.c_str(), mStatus.errorCode.empty() ? "" : ", ",
                 mStatus.errorCode.c_str());
}

void SetupWindow::close(bool play) {
    mEngine.stop();
    if (play) {
        play_sound(kSoundPlay);
        if (mProps.onPlay) mProps.onPlay();
    } else if (mProps.onClose) {
        mProps.onClose();
    }
    pop();
}

void SetupWindow::update() {
    if (mPending) {
        auto action = std::exchange(mPending, {});
        action();
    }
    mEngine.poll(mStatus);
    const unsigned long long now = now_ms();
    if (mPhase == Phase::Running) mEta.add(mStatus.overall, now);
    if (mStatus.finished() && (mPhase == Phase::Running || mPhase == Phase::Stopping)) finish();
    refresh(now);
    for (const auto& button : mButtons) button->update();
    WindowSmall::update();
}

void SetupWindow::put(Rml::Element* element, std::string& shown, const std::string& text) {
    if (shown == text) return;
    shown = text;
    set_text_content(element, text);
}

void SetupWindow::build_steps() {
    clear_children(mSteps);
    mStepRows.clear();
    mStepNames.clear();
    mStepStates.clear();
    for (const auto& s : mStatus.steps) {
        auto* row = append(mSteps, "setup-step");
        append(row, "icon");
        append_text(append(row, "setup-step-name"), phase_label(s.name));
        append(row, "status-badge");
        mStepRows.push_back(row);
        mStepNames.push_back(s.name);
        mStepStates.push_back(-1);
    }
}

void SetupWindow::refresh(unsigned long long now) {
    bool same = mStepNames.size() == mStatus.steps.size();
    for (size_t i = 0; same && i < mStepNames.size(); ++i) same = mStepNames[i] == mStatus.steps[i].name;
    if (!same) build_steps();
    for (size_t i = 0; i < mStepRows.size(); ++i) {
        const int state = int(mStatus.steps[i].state);
        if (state == mStepStates[i]) continue;
        mStepStates[i] = state;
        const StepLook& look = kLooks[state];
        mStepRows[i]->SetClassNames(look.row);
        mStepRows[i]->GetChild(0)->SetClassNames(look.icon);
        Rml::Element* badge = mStepRows[i]->GetChild(2);
        badge->SetClassNames(look.badgeClass);
        set_text_content(badge, look.badge);
    }

    const setup::Status::Step* step = mStatus.current();
    std::string current;
    if (step && mPhase == Phase::Running) {
        current = phase_label(step->name);
        if (const std::string count = count_text(mStatus); !count.empty()) current += ": " + count;
    }
    put(mCurrent, mShownCurrent, current);
    mCurrentBar->SetAttribute("value", step && mStatus.progressM > 0
                                           ? std::clamp(float(mStatus.progressN) / float(mStatus.progressM), 0.f, 1.f)
                                           : 0.f);

    std::string overall = std::to_string(mStatus.overall / 10) + "% done.";
    if (mPhase == Phase::Running) {
        if (const std::string eta = mEta.text(now); !eta.empty()) overall += " " + eta + ".";
    }
    put(mOverall, mShownOverall, overall);
    mOverallBar->SetAttribute("value", float(mStatus.overall) / 1000.f);
    const bool awake = mStatus.awake && mPhase == Phase::Running;
    put(mAwake, mShownAwake, awake ? "Your PC stays awake until setup is done." : "");

    if (mDetailsShown) {
        std::string text;
        const size_t from = mStatus.tail.size() > 12 ? mStatus.tail.size() - 12 : 0;
        for (size_t i = from; i < mStatus.tail.size(); ++i)
            text += (i > from ? "\n" : "") + mStatus.tail[i].substr(0, 300);
        if (text != mShownDetails) {
            mShownDetails = text;
            mDetails->SetInnerRML(escape_lines(text));
        }
    }
}

void SetupWindow::set_actions(std::vector<Action> actions, const char* circle) {
    const bool hadFocus = has_focus();
    mButtons.clear();  // (their listeners go with them, before their elements)
    clear_children(mActions);
    for (Action& a : actions) {
        auto button = std::make_unique<Button>(mActions, a.label);
        button->root()->SetClass("modal-btn", true);
        if (a.onPressed) {
            button->on_pressed([this, fn = std::move(a.onPressed)] {
                play_sound(kSoundClick);
                mPending = fn;
            });
        } else {
            button->set_disabled(true);
        }
        mButtons.push_back(std::move(button));
    }
    clear_children(mHints);
    add_hint(mHints, "cross", "Select");
    if (circle) add_hint(mHints, "circle", circle);
    if (hadFocus || visible()) focus();
}

bool SetupWindow::focus() {
    for (const auto& button : mButtons)
        if (button->focus()) return true;
    return false;
}

void SetupWindow::toggle_details() {
    mDetailsShown = !mDetailsShown;
    mShownDetails.clear();
    if (!mDetailsShown) mDetails->SetInnerRML("");
    set_display(mDetails, mDetailsShown ? Rml::Style::Display::Block : Rml::Style::Display::None);
    if (!mButtons.empty() && mPhase == Phase::Running)
        mButtons[0]->set_text(mDetailsShown ? "Hide details" : "Details");
}

void SetupWindow::ask_stop() {
    push(std::make_unique<Modal>(Modal::Props{
        .title = "Stop setup?",
        .bodyText = "It carries on from here the next time.",
        .actions =
            {
                ModalAction{.label = "Keep going", .onPressed = [](Modal& modal) { modal.pop(); }},
                ModalAction{.label = "Stop",
                            .onPressed =
                                [this](Modal& modal) {
                                    modal.pop();
                                    mPending = [this] { stop_run(); };
                                }},
            },
        .onDismiss = [](Modal& modal) { modal.pop(); },
    }));
}

void SetupWindow::show_failure() {
    const ErrorKind& kind = error_kind(mStatus.errorCode);
    std::string body = "<b>" + escape(mStatus.errorText) + "</b>";
    if (kind.fix == Fix::CheckAgain) {  // the files and what to do about each, from the check
        int listed = 0;
        for (const setup::Line& c : mStatus.checks) {
            if (c.key == "ok" || c.key == "unmeasured") continue;
            if (listed++ == 5) {
                body += "<br/>...";
                break;
            }
            body += "<br/>" + escape(c.text);
        }
    }
    body += "<br/><br/>" + escape(kind.todo);

    std::vector<ModalAction> actions;
    actions.push_back(ModalAction{.label = kind.fix == Fix::CheckAgain ? "Check again" : "Try again",
                                  .onPressed =
                                      [this](Modal& modal) {
                                          modal.pop();
                                          mPending = [this] { start(); };
                                      }});
    if (kind.fix == Fix::Report)
        actions.push_back(ModalAction{.label = "Report",
                                      .onPressed = [this](Modal& modal) { modal.set_body_text(send_report()); }});
    actions.push_back(ModalAction{.label = "Details", .onPressed = [this](Modal& modal) { show_details(modal); }});
    actions.push_back(ModalAction{.label = "Close",
                                  .onPressed =
                                      [this](Modal& modal) {
                                          modal.pop();
                                          mPending = [this] { close(false); };
                                      }});
    push(std::make_unique<Modal>(Modal::Props{
        .title = kind.fix == Fix::CheckAgain ? "Some game files need fixing" : "Setup did not finish",
        .bodyRml = body,
        .actions = std::move(actions),
        .onDismiss = [](Modal& modal) { modal.pop(); },
        .variant = "danger",
        .icon = "error",
    }));
}

// The engine's last lines and the failed step's log, as the crash dialog's Details.
void SetupWindow::show_details(Modal& over) {
    std::string text;
    const size_t from = mStatus.tail.size() > 20 ? mStatus.tail.size() - 20 : 0;
    for (size_t i = from; i < mStatus.tail.size(); ++i) text += mStatus.tail[i].substr(0, 300) + "\n";
    if (!mStatus.logPath.empty()) text += "\nThe log: " + mStatus.logPath;
    over.push(std::make_unique<Modal>(Modal::Props{
        .title = "Details",
        .bodyRml = escape_lines(text),
        .actions = {ModalAction{.label = "Close", .onPressed = [](Modal& d) { d.pop(); }}},
        .onDismiss = [](Modal& d) { d.pop(); },
    }));
}

// A problem report (report.hpp), as the menu's: a zip in the reports folder with this PC's facts, setup's log, the
// failed step's log and the journal, all with the player's name and folders hidden; then the issue form, filled in.
// Nothing of the game: the logs hold step names, counts, addresses and the tools' messages (docs/88 section 6).
std::string SetupWindow::send_report() {
    const auto setupDir = setup::data_dir() / "setup";
    std::vector<report::File> files;
    files.push_back({"system.txt", report::bytes(report::redact(report::system_info()))});
    const auto tail_of = [](const std::filesystem::path& path, size_t head, size_t tail) {
        return report::bytes(report::redact(report::read_head_tail(path, head, tail)));
    };
    files.push_back({"setup.log", tail_of(setupDir / "logs" / "run.log", 40, 256 * 1024)});
    if (!mStatus.logPath.empty()) {
        const std::filesystem::path stepLog(std::u8string(mStatus.logPath.begin(), mStatus.logPath.end()));
        files.push_back({"step.log", tail_of(stepLog, 20, 64 * 1024)});
    }
    files.push_back({"journal.json", tail_of(setupDir / "journal.json", 4000, 256 * 1024)});
    const auto zip = report::write_bundle("setup-" + report::timestamp(), files);
    std::fprintf(stderr, "[setup] report: %s\n", zip.empty() ? "could not be saved" : utf8(zip).c_str());
    if (zip.empty()) return "The report could not be saved.";
    const std::string name = utf8(zip.filename());
    const std::string body = "**What happened:** " + mStatus.errorText + "\n**Step:** " + mStatus.errorStep +
                             "\n**Version:** " + kVersion + "\n\nAttach " + name +
                             " from the folder that opened: it has setup's log and this PC's facts, and no game files.";
    report::open_issue(report::issue_url("Setup did not finish: " + mStatus.errorStep, report::redact(body)), zip);
    return "The issue form is open in your browser. Attach " + name + " to it: it is in the folder that opened.";
}

bool SetupWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel) {
        play_sound(kSoundWindowClose);
        if (mPhase == Phase::Running) ask_stop();
        else if (mPhase != Phase::Stopping) close(false);
        return true;
    }
    if (cmd == NavCommand::Menu) return true;  // (nothing to show or hide here)
    if (cmd != NavCommand::Left && cmd != NavCommand::Right) return false;
    const int direction = cmd == NavCommand::Right ? 1 : -1;
    auto* target = event.GetTargetElement();
    for (int i = 0; i < int(mButtons.size()); ++i) {
        if (!mButtons[i]->contains(target)) continue;
        for (int next = i + direction; next >= 0 && next < int(mButtons.size()); next += direction) {
            if (mButtons[next]->focus()) {
                play_sound(kSoundItemFocus);
                return true;
            }
        }
        return true;
    }
    return focus();
}

}  // namespace ui
