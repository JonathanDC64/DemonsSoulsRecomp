// The setup screen (see setup_window.hpp), on the setup shell (setup_page.hpp) as the setup design draws it: the
// rail, the heading, a panel with the percent, the time left, the bar and the tasks, then the buttons. The texts are
// the screen's; the engine's lines say only which phase and step, and what failed (setup_protocol.hpp).
#include "setup_window.hpp"

#include "input.hpp"
#include "portable.hpp"
#include "report.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <system_error>
#include <utility>

namespace ui {
namespace {

// What the player can do about each kind of failure, and the failed page's line under the heading: protocol.py's
// ERRORS (tools/setup/tests/test_pipeline.py checks that the two lists agree). What failed is the engine's.
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

// The task list's lines: a phase, or a step of one (the lift is two), with its name and the line under it.
struct TaskText {
    const char* id;
    const char* name;
    const char* sub;
};
constexpr TaskText kTasks[] = {
    {"check", "Check the game files", "Every file is read once"},
    {"fetch", "Download the build tools", "Only the first time"},
    {"stage", "Copy the game files", "Into setup's own folder"},
    {"ppu", "Translate the main processor code", "The game's code, one function at a time"},
    {"spu", "Translate the SPU programs", "The code for the PS3's helper processors"},
    {"compile", "Compile", "Builds des_game.dll from the translated code"},
    {"shaders", "Prepare the shaders", "So that the game starts smoothly"},
};

const TaskText* task_text(const std::string& id) {
    for (const TaskText& t : kTasks)
        if (id == t.id) return &t;
    return nullptr;
}

// The count's step for each line ("download" is the fetch phase's).
const char* count_step(const std::string& id) {
    return id == "fetch" ? "download" : id.c_str();
}

std::string grouped(long long v) {
    const std::string digits = std::to_string(std::max(0LL, v));
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// The current step's count: "41,200 / 94,017", "120 / 1,376 MB".
std::string count_text(const setup::Status& s) {
    if (s.progressM <= 0) return {};
    if (s.progress == "check" || s.progress == "download")
        return grouped(s.progressN >> 20) + " / " + grouped(std::max(1LL, s.progressM >> 20)) + " MB";
    return grouped(s.progressN) + " / " + grouped(s.progressM);
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// "About 3 minutes left" as the rail's "about 3 minutes left".
std::string lower_first(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') s[0] = char(s[0] - 'A' + 'a');
    return s;
}

std::string took(unsigned long long ms) {
    const unsigned long long minutes = (ms + 30000) / 60000;
    if (minutes < 1) return "It took less than a minute.";
    if (minutes == 1) return "It took about a minute.";
    return "It took about " + std::to_string(minutes) + " minutes.";
}

}  // namespace

SetupWindow::SetupWindow(Props props) : SetupPage("setup-run"), mProps(std::move(props)) {
    mBody = append(content(), "setup-body");
    start();
}

void SetupWindow::start() {
    mStatus = {};
    mEta.reset();
    mPhase = Phase::Running;
    mDetailsShown = false;
    mStartMs = now_ms();
    draw_running();
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
    set_actions({{"Stopping...", {}, Look::Danger}}, mShownNote);
}

void SetupWindow::finish() {
    const std::string& r = mStatus.result;
    if (r == "ok") {
        mPhase = Phase::Done;
        draw_done();
    } else if (r == "stopped") {
        mPhase = Phase::Stopped;
        draw_stopped();
    } else {
        mPhase = Phase::Failed;
        draw_failed();
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

void SetupWindow::tick() {
    mEngine.poll(mStatus);
    const unsigned long long now = now_ms();
    if (mPhase == Phase::Running) mEta.add(mStatus.overall, now);
    if (mStatus.finished() && (mPhase == Phase::Running || mPhase == Phase::Stopping)) finish();
    if (mPhase == Phase::Running || mPhase == Phase::Stopping) refresh(now);
}

std::vector<std::vector<Button*>> SetupWindow::nav_lines() const {
    return {line(mButtons)};
}

void SetupWindow::back() {
    if (mPhase == Phase::Running) ask_stop();
    else if (mPhase != Phase::Stopping) close(false);
}

void SetupWindow::put(Rml::Element* element, std::string& shown, const std::string& text) {
    if (shown == text) return;
    shown = text;
    set_text_content(element, text);
}

// The rail: the game is in hand (this screen runs with it), the build is where this screen is, Play comes last.
void SetupWindow::draw_rail(const std::string& buildSub) {
    if (buildSub == mShownRail && !mShownRail.empty()) return;
    mShownRail = buildSub;
    using S = RailStep::State;
    const bool done = mPhase == Phase::Done;
    set_rail({
        {"Your game", "", S::Done},
        {"Build", buildSub, done ? S::Done : S::Now},
        {"Play", "", done ? S::Now : S::Todo},
    });
}

void SetupWindow::draw_running() {
    set_heading("Building the game for this PC",
                "Setup translates the game's PS3 code into a program for this PC. The first time takes a while.");
    mButtons.clear();  // (their listeners go before their elements)
    clear_children(mBody);
    mShownPercent.clear();
    mShownEta.clear();
    mShownNote.clear();
    mShownDetails.clear();
    mShownRail.clear();
    auto* box = panel(mBody, true);
    auto* row = append(box, "setup-row");
    mPercent = append(row, "setup-percent");
    mEtaText = append(row, "setup-eta");
    mBar = append(box, "progress");
    mBar->SetClass("setup-meter", true);
    mBar->SetAttribute("value", 0.f);
    mTasks = append(box, "setup-tasks");
    mTaskRows.clear();
    mTaskPlan.clear();
    mDetails = append(mBody, "setup-log");
    set_display(mDetails, Rml::Style::Display::None);
    mActions = actions_row(mBody);
    set_actions({{"Stop", [this] { ask_stop(); }, Look::Danger},
                 {"Details", [this] { toggle_details(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Stop"}});
    refresh(now_ms());
}

void SetupWindow::build_tasks() {
    clear_children(mTasks);
    mTaskRows.clear();
    mTaskPlan.clear();
    for (size_t i = 0; i < mStatus.steps.size(); ++i) {
        const std::string& name = mStatus.steps[i].name;
        mTaskPlan.push_back(name);
        const std::vector<std::string> ids = name == "lift" ? std::vector<std::string>{"ppu", "spu"}
                                                            : std::vector<std::string>{name};
        for (const std::string& id : ids) {
            Task t;
            t.id = id;
            t.phase = i;
            t.row = append(mTasks, "setup-task");
            t.mark = append(t.row, "setup-task-mark");
            auto* words = append(t.row, "setup-task-text");
            const TaskText* tt = task_text(id);
            text(words, "setup-task-name", tt ? tt->name : id);
            t.sub = text(words, "setup-task-sub", tt ? tt->sub : "");
            t.num = append(t.row, "setup-task-num");
            mTaskRows.push_back(t);
        }
    }
}

void SetupWindow::refresh(unsigned long long now) {
    bool same = mTaskPlan.size() == mStatus.steps.size();
    for (size_t i = 0; same && i < mTaskPlan.size(); ++i) same = mTaskPlan[i] == mStatus.steps[i].name;
    if (!same) build_tasks();

    using State = setup::Status::State;
    for (Task& t : mTaskRows) {
        State state = mStatus.steps[t.phase].state;
        // The lift's two steps: the translation of the main code first, then the SPU programs'.
        if (t.id == "ppu" || t.id == "spu") {
            const bool onSpu = mStatus.progress == "spu";
            if (state == State::Running || state == State::Failed) {
                if (t.id == "ppu" && onSpu) state = State::Done;
                else if (t.id == "spu" && !onSpu) state = State::Todo;
            }
        }
        if (int(state) != t.state) {
            t.state = int(state);
            static const char* const kRows[] = {"", "run", "done", "done", "fail"};
            t.row->SetClassNames(kRows[int(state)]);
            clear_children(t.mark);
            if (state == State::Running) append(t.mark, "setup-spin");
            else icon(t.mark, state == State::Todo      ? "setup-wait"
                              : state == State::Failed ? "setup-err"
                                                       : "setup-ok");
            if (state == State::Skipped) {
                set_text_content(t.sub, t.id == "fetch" ? "Already downloaded" : "Already done");
            }
        }
        std::string count;
        if ((state == State::Running || state == State::Failed) && mStatus.progress == count_step(t.id))
            count = count_text(mStatus);
        put(t.num, t.count, count);
    }

    const std::string percent = std::to_string(mStatus.overall / 10) + "%";
    put(mPercent, mShownPercent, percent);
    const std::string eta = mPhase == Phase::Running ? mEta.text(now) : "";
    put(mEtaText, mShownEta, eta);
    mBar->SetAttribute("value", float(mStatus.overall) / 1000.f);
    draw_rail(eta.empty() ? percent : percent + ", " + lower_first(eta));

    if (mPhase == Phase::Running) {
        put(mNote, mShownNote,
            mStatus.awake ? "Your PC stays awake until setup is done. You can stop and carry on later."
                          : "You can stop and carry on later.");
    }

    if (mDetailsShown) {
        const std::string tail = log_tail(12);
        if (tail != mShownDetails) {
            mShownDetails = tail;
            mDetails->SetInnerRML(escape_lines(tail));
        }
    }
}

void SetupWindow::set_actions(std::vector<Action> actions, const std::string& note) {
    const bool hadFocus = has_focus();
    set_buttons(mButtons, mActions, std::move(actions));
    mNote = text(mActions, "setup-note", note);
    mShownNote = note;
    if (!asking() && (hadFocus || visible())) focus();
}

void SetupWindow::toggle_details() {
    mDetailsShown = !mDetailsShown;
    mShownDetails.clear();
    if (!mDetailsShown) mDetails->SetInnerRML("");
    set_display(mDetails, mDetailsShown ? Rml::Style::Display::Block : Rml::Style::Display::None);
    if (mButtons.size() > 1 && mPhase == Phase::Running)
        mButtons[1]->set_text(mDetailsShown ? "Hide details" : "Details");
}

void SetupWindow::ask_stop() {
    ask("Stop the build?", escape("The finished parts are kept. Next time, setup carries on from here."),
        {{"Stop", [this] { stop_run(); }, Look::Danger}, {"Keep building", {}, Look::Plain}}, 1);
}

void SetupWindow::draw_failed() {
    const ErrorKind& kind = error_kind(mStatus.errorCode);
    set_heading(kind.fix == Fix::CheckAgain ? "Some game files need fixing" : "The build stopped", kind.todo);
    mButtons.clear();  // (their listeners go before their elements)
    clear_children(mBody);
    mDetails = nullptr;
    const char* report = kind.fix == Fix::Report
                             ? "Report it with one click. The report has setup's log and this PC's details, and "
                               "nothing from the game."
                             : "";
    auto* body = callout(mBody, "err", mStatus.errorText, report);
    if (kind.fix == Fix::CheckAgain) {  // the files and what to do about each, from the check
        std::string files;
        int listed = 0;
        for (const setup::Line& c : mStatus.checks) {
            if (c.key == "ok" || c.key == "unmeasured") continue;
            if (listed++ == 5) {
                files += "\n...";
                break;
            }
            files += (files.empty() ? "" : "\n") + c.text;
        }
        if (!files.empty()) append(body, "setup-callout-text")->SetInnerRML(escape_lines(files));
    }
    if (const std::string tail = log_tail(12); !tail.empty())
        append(body, "setup-log")->SetInnerRML(escape_lines(tail));

    mActions = actions_row(mBody);
    const char* again = kind.fix == Fix::CheckAgain ? "Check again" : "Try again";
    std::vector<Action> actions;
    if (kind.fix == Fix::Report) {
        actions.push_back({"Report the problem", [this] { ask_report(); }, Look::Primary});
        actions.push_back({again, [this] { start(); }, Look::Plain});
    } else {
        actions.push_back({again, [this] { start(); }, Look::Primary});
    }
    if (!log_file().empty()) actions.push_back({"Show the full log", [this] { open_log(); }, Look::Quiet});
    actions.push_back({"Close", [this] { close(false); }, Look::Quiet});
    set_actions(std::move(actions));
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Close"}});
    mShownRail.clear();
    draw_rail("Stopped");
}

void SetupWindow::draw_stopped() {
    set_heading("The build is stopped", "The finished parts are kept. Next time, setup carries on from here.");
    mButtons.clear();  // (their listeners go before their elements)
    clear_children(mBody);
    mDetails = nullptr;
    mActions = actions_row(mBody);
    mActions->SetClass("first", true);  // (straight under the heading)
    set_actions({{"Resume", [this] { start(); }, Look::Primary}, {"Close", [this] { close(false); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Close"}});
    mShownRail.clear();
    draw_rail("Stopped");
}

void SetupWindow::draw_done() {
    const bool ready = setup::launch_state().state == setup::State::Ready;
    mButtons.clear();  // (their listeners go before their elements)
    clear_children(mBody);
    mDetails = nullptr;
    if (ready) {
        set_heading("Ready to play", "The game is built for this PC.");
        found(panel(mBody), "setup-ok", "Built", "des_game.dll for version " + std::string(kVersion) + ". " +
                                                     took(now_ms() - mStartMs));
    } else {
        set_heading("Setup finished", "This version has no build step yet, so the game is not built.");
    }
    mActions = actions_row(mBody);
    if (ready && mProps.onPlay) set_actions({{"Play", [this] { close(true); }, Look::Primary}});
    else set_actions({{"Close", [this] { close(false); }, Look::Primary}});
    if (ready) {
        text(mBody, "setup-note", "In the game, F1 or " +
                                      std::string(input::chord_label(settings::get_int("pad_menu_chord"))) +
                                      " opens the menu.")
            ->SetClass("after", true);
    }
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Close"}});
    mShownRail.clear();
    draw_rail("Done");
}

// The failed step's own log if it has one, else setup's.
std::filesystem::path SetupWindow::log_file() const {
    std::error_code ec;
    if (!mStatus.logPath.empty()) {
        const auto step = from_utf8(mStatus.logPath);
        if (std::filesystem::is_regular_file(step, ec)) return step;
    }
    const auto run = setup::data_dir() / "setup" / "logs" / "run.log";
    if (std::filesystem::is_regular_file(run, ec)) return run;
    return {};
}

void SetupWindow::open_log() {
    const auto path = log_file();
    std::fprintf(stderr, "[setup] the log: %s\n", path.empty() ? "none" : utf8(path).c_str());
    if (!path.empty()) report::open_url(utf8(path));
}

// The engine's last plain lines, at most `lines` of them, each cut at 300 characters.
std::string SetupWindow::log_tail(size_t lines) const {
    std::string text;
    const size_t from = mStatus.tail.size() > lines ? mStatus.tail.size() - lines : 0;
    for (size_t i = from; i < mStatus.tail.size(); ++i)
        text += (i > from ? "\n" : "") + mStatus.tail[i].substr(0, 300);
    return text;
}

void SetupWindow::ask_report() {
    ask("Report the problem",
        escape("Saves one file and opens the issue page with the details filled in. The file holds setup's log, "
               "this PC's processor, memory, graphics card and system, and this program's version. It holds "
               "nothing from the game."),
        {{"Save and open the issue page",
          [this] {
              const std::string said = send_report();
              ask("Report the problem", escape(said), {{"Close", {}, Look::Plain}});
          },
          Look::Primary},
         {"Close", {}, Look::Quiet}},
        0);
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
    if (!mStatus.logPath.empty()) files.push_back({"step.log", tail_of(from_utf8(mStatus.logPath), 20, 64 * 1024)});
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

}  // namespace ui
