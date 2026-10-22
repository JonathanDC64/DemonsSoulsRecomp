// The setup screens before a run (see setup_screens.hpp), on the setup shell as the setup design draws them. The
// texts are the screens'; the engine's lines say only which file is in what state and what failed (setup_protocol.hpp).
//
// How they stack: the start screen opens "Where is your game?" over itself, which opens the game folder over itself,
// which opens Ready to build over itself. Start building there closes the screens above the start screen without
// uncovering anything and opens the progress screen over the start screen, still covered, so that the start screen
// comes back, with its state read again, when the progress screen closes.
#include "setup_screens.hpp"

#include "app.hpp"
#include "host.hpp"
#include "portable.hpp"
#include "settings_window.hpp"
#include "setup_window.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>
#include <utility>

namespace ui {
namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
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

// "4 GB", "3.4 GB", "120 GB".
std::string gigabytes(unsigned long long bytes) {
    const double gb = double(bytes) / double(1ull << 30);
    char text[32];
    std::snprintf(text, sizeof text, gb >= 10.0 || gb == double((unsigned long long)gb) ? "%.0f GB" : "%.1f GB", gb);
    return text;
}

// To 5 minutes under an hour, to 10 above.
int rounded_minutes(int m) {
    return m < 60 ? std::max(5, (m + 2) / 5 * 5) : (m + 5) / 10 * 10;
}

// "15 minutes", "1 hour 20 minutes".
std::string minutes_text(int m) {
    if (m < 60) return std::to_string(m) + " minutes";
    const int hours = m / 60, rest = m % 60;
    std::string out = std::to_string(hours) + (hours == 1 ? " hour" : " hours");
    if (rest) out += " " + std::to_string(rest) + " minutes";
    return out;
}

// "10 to 20 minutes", "40 minutes to 1 hour 20 minutes".
std::string time_text(const setup::Estimate& e) {
    const int low = rounded_minutes(e.lowMinutes), high = std::max(low, rounded_minutes(e.highMinutes));
    if (low == high) return minutes_text(low);
    if (high < 60) return std::to_string(low) + " to " + std::to_string(high) + " minutes";
    return minutes_text(low) + " to " + minutes_text(high);
}

// The rail's line under Build before a run: "About 10 to 20 minutes".
std::string build_time() {
    return "About " + time_text(setup::setup_time());
}

const char* state_name(setup::State s) {
    switch (s) {
    case setup::State::Ready: return "ready";
    case setup::State::Missing: return "missing";
    case setup::State::Interrupted: return "interrupted";
    case setup::State::Outdated: return "outdated";
    case setup::State::Damaged: return "damaged";
    case setup::State::GameMoved: return "game moved";
    }
    return "?";
}

// Which part of the check a @check line is, by its table name (tools/setup/des_setup/game_table.json): the files the
// player decrypts end in .elf or .unedat (and the firmware's in .prx); the disc's own files are paths under PS3_GAME;
// the rest (PS3_GAME, PARAM.SFO, TITLE_ID, APP_VER) are the folder and the title.
enum class Part { Game, Disc, Decrypted };

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != tail[i]) return false;
    return true;
}

Part part_of(const std::string& name) {
    if (ends_with(name, ".elf") || ends_with(name, ".unedat") || ends_with(name, ".prx")) return Part::Decrypted;
    return name.find('/') == std::string::npos ? Part::Game : Part::Disc;
}

bool good(const std::string& state) {
    return state == "ok" || state == "unmeasured";
}

// A decrypted file's row as drawn, by gamefiles.py's states.
struct FileLook {
    const char* state;
    const char* mark;
    const char* words;
};
constexpr FileLook kFileLooks[] = {
    {"ok", "setup-ok", "Found"},
    {"unmeasured", "setup-ok", "Found"},
    {"missing", "setup-warn", "Missing"},
    {"encrypted", "setup-err", "Still encrypted"},
    {"mismatch", "setup-err", "Does not match"},
    {"wrong", "setup-err", "Does not match"},
};

const FileLook& look_of(const std::string& state) {
    for (const FileLook& l : kFileLooks)
        if (state == l.state) return l;
    return kFileLooks[std::size(kFileLooks) - 1];
}

// A file's name as the list shows it: its path under USRDIR ("chr/c0000/c0000.esd.dcx.sdat.unedat"), so that the
// player can find its original.
std::string list_name(const std::string& name) {
    const size_t at = name.find("USRDIR/");
    return at == std::string::npos ? name : name.substr(at + 7);
}

// The rows of the files to fix that the game folder page lists before "And N more".
constexpr int kListedFiles = 6;

// L1 and R1: the page's content up or down by most of its height.
bool scroll_page(Rml::Element* page, int pages) {
    const float step = std::max(24.f, page->GetClientHeight() * 0.8f);
    page->SetScrollTop(std::max(0.f, page->GetScrollTop() + step * float(pages)));
    return true;
}

}  // namespace

// ---- 1. The start screen ----

SetupStartWindow::SetupStartWindow(Props props) : SetupPage("setup-start"), mProps(std::move(props)) {
    mBody = append(content(), "setup-body");
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Quit"}});
    refresh();
}

// Shown first, so that the document gets back the button it had focused; the rebuilt button in its place then takes
// the focus (Settings after the settings window, the main button after a run).
void SetupStartWindow::uncover() {
    SetupPage::uncover();
    const auto place = focus_place();
    refresh();
    if (visible() && !focus_at(place) && !button_focused()) focus();
}

void SetupStartWindow::refresh() {
    using setup::State;
    using S = RailStep::State;
    mLaunch = setup::launch_state();
    mMain.clear();  // (their listeners go before their elements)
    mMore.clear();
    clear_children(mBody);
    std::vector<Action> main;
    Rml::Element* row = nullptr;
    const auto welcome_back = [&](const char* kind, const std::string& title, const std::string& line) {
        set_heading("Welcome back", "Here is where setup stands.");
        row = actions_row(callout(mBody, kind, title, line));
    };
    switch (mLaunch.state) {
    case State::Missing: {
        const std::string time = build_time();
        set_heading("Set up Demon's Souls", "Setup turns your own copy of the game into a program for this PC. It "
                                            "takes " + std::string(1, char(std::tolower(time[0]))) + time.substr(1) +
                                            " here and needs " + gigabytes(setup::kNeedBytes) +
                                            " of free space while it works.");
        auto* box = panel(mBody);
        found(box, "setup-info", "You need your copy of the game",
              "The US disc, BLUS30443 version 1.00, copied to a folder, with 27 of its files decrypted once. Setup "
              "shows you how.");
        found(box, "setup-info", "Setup downloads its build tools once",
              "The compiler and the tools it needs, each checked before it is used. Later builds use the same copy.");
        found(box, "setup-info", "You can stop and come back",
              "Close the window at any point. Setup carries on where it stopped the next time.");
        row = actions_row(mBody);
        main.push_back({"Start", [this] { open_source(); }, Look::Primary});
        set_rail({{"Your game", "", S::Now}, {"Build", time, S::Todo}, {"Play", "", S::Todo}});
        break;
    }
    case State::Interrupted:
        welcome_back("info", "Setup was interrupted", "It carries on where it stopped. The finished parts are kept.");
        main.push_back({"Resume setup", [this] { run(mProps.request); }, Look::Primary});
        main.push_back({"Start over", [this] { open_source(); }, Look::Quiet});
        set_rail({{"Your game", "", S::Done}, {"Build", "Stopped", S::Now}, {"Play", "", S::Todo}});
        break;
    case State::Outdated:
        welcome_back("info", "This version needs a rebuild",
                     "Your game files are kept, so there is nothing to choose. " + build_time() + " on this PC.");
        main.push_back({"Update now", [this] { run(mProps.request); }, Look::Primary});
        set_rail({{"Your game", "", S::Done}, {"Build", "Needs a rebuild", S::Now}, {"Play", "", S::Todo}});
        break;
    case State::Damaged:
        welcome_back("warn", "Some of setup's files are missing",
                     "Some of the files setup made are missing or have changed. Repair builds only those again.");
        main.push_back({"Repair", [this] { run(mProps.request); }, Look::Primary});
        set_rail({{"Your game", "", S::Done}, {"Build", "Needs a repair", S::Now}, {"Play", "", S::Todo}});
        break;
    case State::GameMoved:
        welcome_back("warn", "Your game folder moved",
                     "It is not in " + utf8(mLaunch.game) + " any more. Choose where it is now: nothing needs building.");
        main.push_back({"Choose the game folder", [this] { open_folder(); }, Look::Primary});
        set_rail({{"Your game", "Moved", S::Now}, {"Build", "", S::Done}, {"Play", "", S::Todo}});
        break;
    case State::Ready:
        set_heading("Ready to play", "The game is built for this PC.");
        {
            auto* box = panel(mBody);
            if (!mLaunch.game.empty()) found(box, "setup-ok", "Your game", "From " + utf8(mLaunch.game) + ".");
            found(box, "setup-ok", "Built", "des_game.dll for version " + std::string(kVersion) + ".");
        }
        row = actions_row(mBody);
        main.push_back({"Play",
                        [this] {
                            play_sound(kSoundPlay);
                            play();
                        },
                        Look::Primary});
        set_rail({{"Your game", "", S::Done}, {"Build", "Done", S::Done}, {"Play", "", S::Now}});
        break;
    }
    set_buttons(mMain, row, std::move(main));
    set_buttons(mMore, actions_row(mBody),
                {{"Settings", [this] { push(std::make_unique<SettingsWindow>(true)); }, Look::Quiet},
                 {"Quit", [this] { ask_quit(); }, Look::Quiet}});
    std::fprintf(stderr, "[setup] the start screen: %s\n", state_name(mLaunch.state));
}

void SetupStartWindow::open_source() {
    push(std::make_unique<SetupSourceWindow>(SetupSourceWindow::Props{
        .request = mProps.request,
        .onStart = [this](setup::Request request) { run(std::move(request)); },
    }));
}

// A moved game folder needs only the folder: nothing is rebuilt, so no Ready to build.
void SetupStartWindow::open_folder() {
    push(std::make_unique<SetupFilesWindow>(SetupFilesWindow::Props{
        .request = mProps.request,
        .moved = mLaunch.game,
        .rebuild = false,
        .onStart = [this](setup::Request request) { run(std::move(request)); },
    }));
}

// The progress screen. From this screen it covers this one. From the screens this one opened (closed by now, this one
// still covered by them) it goes on top without covering it again, so that this one comes back when it closes.
void SetupStartWindow::run(setup::Request request) {
    auto window = std::make_unique<SetupWindow>(SetupWindow::Props{
        .request = std::move(request),
        .onPlay = [this] { play(); },
    });
    if (visible()) push(std::move(window));
    else push_document(std::move(window));
}

void SetupStartWindow::play() {
    std::fprintf(stderr, "[setup] play\n");
    if (mProps.onPlay) mProps.onPlay();
    if (visible()) pop();   // (its own Play)
    else force_hide(true);  // (the progress screen's, which closes itself next)
}

// Nothing is lost on this screen (no run is going, and the journal keeps what is done), but a stray Circle should not
// close the program: it asks, Cancel first.
void SetupStartWindow::ask_quit() {
    ask("Quit setup?", escape("What setup has done so far is kept."),
        {{"Quit",
          [] {
              if (app().quit) app().quit();
          },
          Look::Danger},
         {"Cancel", {}, Look::Plain}},
        1);
}

std::vector<std::vector<Button*>> SetupStartWindow::nav_lines() const {
    return {line(mMain), line(mMore)};
}

void SetupStartWindow::back() {
    ask_quit();
}

// ---- 2. Where is your game? ----

SetupSourceWindow::SetupSourceWindow(Props props) : SetupPage("setup-source"), mProps(std::move(props)) {
    using S = RailStep::State;
    set_rail({{"Your game", "", S::Now}, {"Build", build_time(), S::Todo}, {"Play", "", S::Todo}});
    set_heading("Where is your game?", "Pick how setup gets your game's files. Every way ends in the same build.");
    auto* cards = append(content(), "setup-choices");
    struct Card {
        const char* title;
        const char* tag;
        const char* text;
        std::function<void()> open;
    };
    const Card kCards[] = {
        {"I have decrypted files", "No RPCS3 needed",
         "A folder copy of the disc (it has a PS3_GAME folder), with the game's 27 decrypted files next to the "
         "originals: EBOOT.elf and the .sdat.unedat files.",
         [this] { open_folder(); }},
        {"Show me how", "I have neither yet",
         "How to copy your disc to a folder and decrypt the 27 files, with free tools.", [this] { open_help(); }},
    };
    for (const Card& c : kCards) {
        auto button = std::make_unique<Button>(cards, "");
        Rml::Element* root = button->root();
        root->SetClass("setup-choice", true);
        text(root, "setup-choice-title", c.title);
        text(root, "setup-tag", c.tag)->SetClass("muted", true);
        text(root, "setup-choice-text", c.text);
        button->on_pressed([this, fn = c.open] {
            play_sound(kSoundClick);
            mPending = fn;
        });
        mCards.push_back(std::move(button));
    }
    set_buttons(mActions, actions_row(content()), {{"Back", [this] { back(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
}

std::vector<std::vector<Button*>> SetupSourceWindow::nav_lines() const {
    return {line(mCards), line(mActions)};
}

void SetupSourceWindow::back() {
    pop();
}

// The game folder. Its Start building closes this one too (covered, without coming back) on its way to the progress
// screen.
void SetupSourceWindow::open_folder() {
    push(std::make_unique<SetupFilesWindow>(SetupFilesWindow::Props{
        .request = mProps.request,
        .onStart =
            [this](setup::Request request) {
                force_hide(true);
                if (mProps.onStart) mProps.onStart(std::move(request));
            },
    }));
}

void SetupSourceWindow::open_help() {
    push(std::make_unique<SetupHelpWindow>(SetupHelpWindow::Props{.onFolder = [this] { open_folder(); }}));
}

// ---- 3. The game folder ----

SetupFilesWindow::SetupFilesWindow(Props props)
    : SetupPage("setup-files"), mProps(std::move(props)), mRequest(mProps.request),
      mPicked(std::make_shared<Mailbox<PickedFolder>>()),
      mFound(std::make_shared<Mailbox<std::vector<setup::Place>>>()) {
    using S = RailStep::State;
    set_rail({{"Your game", mProps.moved.empty() ? "" : "Moved", S::Now},
              {"Build", mProps.rebuild ? build_time() : "", mProps.rebuild ? S::Todo : S::Done},
              {"Play", "", S::Todo}});
    if (mProps.moved.empty())
        set_heading("Your game folder", "Pick the folder that holds PS3_GAME. Setup reads it and copies what it needs.");
    else
        set_heading("Your game folder moved", "Choose where it is now. Nothing needs building.");

    auto* field = append(content(), "setup-field");
    field->SetClass("folder", true);
    icon(field, "setup-folder");
    mFolderText = append(field, "setup-field-text");
    mChooseRow = append(content(), "setup-actions");
    mChooseRow->SetClass("near", true);
    mPlacesNote = text(content(), "setup-heading", "");
    mPlacesList = append(content(), "setup-places");
    mCheck = append(content(), "setup-body");
    mLinks = append(content(), "setup-actions");
    mLinks->SetClass("near", true);
    mActionsRow = actions_row(content());
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}, {nullptr, "L1 R1", "Scroll"}});

    set_buttons(mChooseButton, mChooseRow, {{"Choose folder", [this] { choose(Picking::Game); }, Look::Plain}});
    set_buttons(mLinkButtons, mLinks,
                {{"How do I make these?",
                  [this] { push(std::make_unique<SetupHelpWindow>(SetupHelpWindow::Props{})); }, Look::Quiet},
                 {"Check again", [this] { check(); }, Look::Quiet}});
    set_buttons(mActions, mActionsRow,
                {{mProps.rebuild ? "Continue" : "Use this folder", [this] { start(); }, Look::Primary},
                 {"Back", [this] { back(); }, Look::Quiet}});

    // The usual places, off this thread (a sleeping disk wakes up for it).
    std::thread([found = mFound] { found->put(setup::likely_places()); }).detach();
    draw_folder();
    draw_places();
    if (!mRequest.game.empty()) check();
    draw_check();
}

bool SetupFilesWindow::focus() {
    // Continue once the files are good; else the first dump found while no folder is chosen; else Choose folder.
    if (asking()) return SetupPage::focus();
    if (mStatus.result == "ok" && !mActions.empty() && mActions[0]->focus()) return true;
    if (mRequest.game.empty() && !mPlaceButtons.empty() && mPlaceButtons[0]->focus()) return true;
    if (!mChooseButton.empty() && mChooseButton[0]->focus()) return true;
    return SetupPage::focus();
}

std::vector<std::vector<Button*>> SetupFilesWindow::nav_lines() const {
    std::vector<std::vector<Button*>> out;
    out.push_back(line(mChooseButton));
    for (const auto& b : mPlaceButtons) out.push_back({b.get()});
    if (!mDecryptedButton.empty()) out.push_back(line(mDecryptedButton));
    out.push_back(line(mLinkButtons));
    out.push_back(line(mActions));
    return out;
}

void SetupFilesWindow::tick() {
    if (std::vector<setup::Place> places; mFound->take(places)) {
        mPlaces = std::move(places);
        mScanned = true;
        std::fprintf(stderr, "[setup] %zu game folder(s) found in the usual places\n", mPlaces.size());
        draw_places();
        if (!button_focused() && visible()) focus();
    }
    if (PickedFolder folder; mPicked->take(folder)) picked(folder);
    if (mChecking) mEngine.poll(mStatus);
    draw_check();
}

void SetupFilesWindow::back() {
    pop();
}

bool SetupFilesWindow::scroll(int pages) {
    return scroll_page(content(), pages);
}

// The system's folder dialog, on a thread of its own (folder_picker.hpp): its answer comes in tick().
void SetupFilesWindow::choose(Picking what) {
    if (mPicking != Picking::None) return;  // (one dialog at a time)
    mPicking = what;
    const bool game = what == Picking::Game;
    std::filesystem::path from = game || mRequest.decrypted.empty() ? mRequest.game : mRequest.decrypted[0];
    if (from.empty()) from = mProps.moved.parent_path();
    pick_folder(from, game ? "Choose the folder of your disc copy" : "Choose the folder with the decrypted files",
                host::window(), [box = mPicked](PickedFolder folder) { box->put(std::move(folder)); });
}

void SetupFilesWindow::picked(const PickedFolder& folder) {
    const Picking what = std::exchange(mPicking, Picking::None);
    if (folder.failed) {
        mPickProblem = "The folder window could not open on this PC.";
        return;
    }
    if (folder.folder.empty()) return;  // (closed without a choice)
    if (what == Picking::Game) mRequest.game = folder.folder;
    else mRequest.decrypted = {folder.folder};
    draw_folder();
    check();
}

// `run --check-only` on the folders: an @check line for each file, and the verdict.
void SetupFilesWindow::check() {
    mStatus = {};
    mPickProblem.clear();
    mChecking = false;
    if (!mRequest.game.empty()) {
        mChecking = mEngine.start(mRequest, true);
        if (!mChecking) {  // (the engine could not start: why() says why, for the player)
            mStatus.result = "failed";
            mStatus.errorText = mEngine.why();
        }
    }
    for (size_t i = 0; i < mPlaces.size() && i < mPlaceButtons.size(); ++i)
        mPlaceButtons[i]->set_selected(mPlaces[i].folder == mRequest.game);
}

void SetupFilesWindow::start() {
    if (mStatus.result != "ok" || (mChecking && !mStatus.finished())) return;
    if (!mProps.onStart) {
        pop();
        return;
    }
    if (mProps.rebuild) {
        push(std::make_unique<SetupBeforeWindow>(SetupBeforeWindow::Props{
            .request = mRequest,
            .onStart =
                [this](setup::Request request) {
                    force_hide(true);  // (covered by Ready to build: it closes without coming back)
                    mProps.onStart(std::move(request));
                },
        }));
        return;
    }
    hide(true);  // (not pop(): the progress screen goes over the start screen, which comes back after it)
    mProps.onStart(mRequest);
}

void SetupFilesWindow::draw_folder() {
    std::string shown = "No folder chosen";
    if (!mRequest.game.empty()) shown = utf8(mRequest.game);
    else if (!mProps.moved.empty()) shown = "Not found: " + utf8(mProps.moved);
    set_text_content(mFolderText, shown);
    mFolderText->SetClass("none", mRequest.game.empty());
    if (!mChooseButton.empty()) mChooseButton[0]->set_text(mRequest.game.empty() ? "Choose folder" : "Change folder");
}

// The dumps found, one button each: "BLUS30443 version 01.00 in D:\Games\Demon's Souls".
void SetupFilesWindow::draw_places() {
    if (!mScanned) set_text_content(mPlacesNote, "Looking in the usual places...");
    else if (mPlaces.empty()) set_text_content(mPlacesNote, "Nothing found in the usual places");
    else set_text_content(mPlacesNote, "Found on this PC");
    std::vector<Action> actions;
    for (const setup::Place& p : mPlaces) {
        std::string label = p.title + (p.version.empty() ? "" : " version " + p.version) + " in " + utf8(p.folder);
        if (p.title != setup::kTitleId) label += " (not supported)";
        actions.push_back({label, [this, folder = p.folder] {
                               mRequest.game = folder;
                               draw_folder();
                               check();
                           }});
    }
    set_buttons(mPlaceButtons, mPlacesList, std::move(actions));
    for (const auto& b : mPlaceButtons) b->root()->SetClass("setup-place", true);
    set_display(mPlacesList, mPlaces.empty() ? Rml::Style::Display::None : Rml::Style::Display::Flex);
    for (size_t i = 0; i < mPlaces.size() && i < mPlaceButtons.size(); ++i)
        mPlaceButtons[i]->set_selected(mPlaces[i].folder == mRequest.game);
}

// The check's part, drawn again only when what it shows has changed: while it runs, a line with the count; then the
// game's row and, if it is the game, the decrypted files: a row for all of them, then the ones to fix.
void SetupFilesWindow::draw_check() {
    const bool running = mChecking && !mStatus.finished();
    const std::string key = std::to_string(int(running)) + "|" + mStatus.result + "|" +
                            std::to_string(mStatus.progressN) + "|" + std::to_string(mStatus.checks.size()) + "|" +
                            mStatus.errorText + "|" + mPickProblem + "|" + utf8(mRequest.game) + "|" +
                            (mRequest.decrypted.empty() ? std::string() : utf8(mRequest.decrypted[0]));
    if (key == mShownCheck) return;
    mShownCheck = key;

    const bool hadFocus = has_focus();
    mDecryptedButton.clear();  // (their listeners go before their elements)
    clear_children(mCheck);
    if (!mPickProblem.empty()) {
        callout(mCheck, "err", mPickProblem, "");
    } else if (mRequest.game.empty()) {
        // (nothing chosen yet: the field says so)
    } else if (running) {
        std::string line = "Checking the game files...";
        if (mStatus.progress == "check" && mStatus.progressM > 0)
            line = "Checking the game files: " + grouped(mStatus.progressN >> 20) + " of " +
                   grouped(std::max(1LL, mStatus.progressM >> 20)) + " MB";
        auto* row = append(panel(mCheck), "setup-row");
        row->SetClass("start", true);
        append(row, "setup-spin");
        text(row, "setup-note", line);
    } else {
        std::vector<const setup::Line*> game, disc, files;
        for (const setup::Line& c : mStatus.checks) {
            switch (part_of(c.key2)) {
            case Part::Game: game.push_back(&c); break;
            case Part::Disc: disc.push_back(&c); break;
            case Part::Decrypted: files.push_back(&c); break;
            }
        }
        const auto bad = [](const setup::Line* c) { return !good(c->key); };
        const bool isGame = !game.empty() && std::none_of(game.begin(), game.end(), bad);
        auto* box = panel(mCheck);
        if (isGame) {
            std::string value = utf8(mRequest.game);
            if (!disc.empty()) {
                const auto wrong = std::count_if(disc.begin(), disc.end(), bad);
                value += wrong == 0 ? ". The disc's " + std::to_string(disc.size()) + " files match."
                                    : ". " + (*std::find_if(disc.begin(), disc.end(), bad))->text;
            }
            found(box, "setup-ok", "Demon's Souls, US disc, version 1.00", value);
        } else if (!game.empty()) {  // (a folder that is not the game's, or another copy: the line says why)
            found(box, "setup-err", "This is not a copy setup can use",
                  (*std::find_if(game.begin(), game.end(), bad))->text);
        } else {
            found(box, "setup-err", "The check did not finish",
                  mStatus.errorText.empty() ? std::string("Check again.") : mStatus.errorText);
        }
        if (isGame && !files.empty()) {
            text(mCheck, "setup-heading", "The game's decrypted files");
            text(mCheck, "setup-text",
                 "The game's code and 26 of its files are encrypted on the disc. You decrypt them once with a PS3 "
                 "tool; setup checks each one against the right copy.")
                ->SetClass("note", true);
            auto* list = panel(mCheck);
            const auto missing = std::count_if(files.begin(), files.end(), bad);
            found(list, missing ? "setup-warn" : "setup-ok",
                  missing ? std::to_string(files.size() - size_t(missing)) + " of " + std::to_string(files.size()) +
                                " found"
                          : "All " + std::to_string(files.size()) + " found, all match",
                  mRequest.decrypted.empty() ? "Next to the originals" : "In " + utf8(mRequest.decrypted[0]));
            std::stable_partition(files.begin(), files.end(), bad);
            int listed = 0;
            for (const setup::Line* c : files) {
                if (!bad(c)) break;
                if (listed++ == kListedFiles) {
                    found(list, "setup-warn", "And " + std::to_string(missing - kListedFiles) + " more", "");
                    break;
                }
                found(list, look_of(c->key).mark, list_name(c->key2), look_of(c->key).words);
            }
            auto* where = actions_row(mCheck);
            where->SetClass("near", true);
            set_buttons(mDecryptedButton, where,
                        {{mRequest.decrypted.empty() ? "They are in another folder" : "Choose another folder",
                          [this] { choose(Picking::Decrypted); }, Look::Quiet}});
        }
    }

    const bool ok = !running && mStatus.result == "ok";
    if (mLinkButtons.size() > 1) mLinkButtons[1]->set_disabled(mRequest.game.empty());
    if (!mActions.empty()) mActions[0]->set_disabled(!ok);
    if (ok && !mShownOk && visible() && !asking()) {  // (the files are good: Cross goes on)
        mActions[0]->focus();
        reveal_focus();
    } else if (hadFocus && !button_focused() && visible()) {  // (its button went with the redraw)
        focus();
    }
    mShownOk = ok;
    if (!running && !mStatus.result.empty())
        std::fprintf(stderr, "[setup] the game files check: %s, %s\n", mStatus.result.c_str(),
                     mStatus.errorCode.empty() ? "-" : mStatus.errorCode.c_str());
}

// ---- 4. Getting your game's files ----

SetupHelpWindow::SetupHelpWindow(Props props) : SetupPage("setup-help"), mProps(std::move(props)) {
    using S = RailStep::State;
    set_rail({{"Your game", "", S::Now}, {"Build", build_time(), S::Todo}, {"Play", "", S::Todo}});
    set_heading("Getting your game's files",
                "Setup needs your own copy of the game: the US disc, BLUS30443 version 1.00, copied to a folder, "
                "with 27 of its files decrypted once. Setup never decrypts anything itself.");
    const auto guide = [](Rml::Element* body, std::initializer_list<const char*> steps) {
        auto* list = append(body, "setup-guide");
        int n = 0;
        for (const char* step : steps) text(list, "setup-guide-item", std::to_string(++n) + ". " + step);
    };
    auto* stack = append(content(), "setup-stack");
    guide(callout(stack, "info", "Copy your disc",
                  "If your game is still on the disc, copy it to a folder first. Either way gives the folder with "
                  "PS3_GAME in it."),
          {"PS3 Disc Dumper (free) copies the disc with a compatible Blu-ray drive in your PC.",
           "Or, on a PS3 with custom firmware, a file manager such as multiMAN or IRISMAN copies the disc to a USB "
           "drive."});
    guide(callout(stack, "info", "Decrypt the 27 files",
                  "RPCS3, the free PS3 emulator, does this. You do not need to add the game to RPCS3 for it."),
          {"In RPCS3, choose Utilities, then Decrypt PS3 Binaries.",
           "Select PS3_GAME/USRDIR/EBOOT.BIN, then the .sdat files in chr, chr/c0000, menu, script and script/talk.",
           "RPCS3 writes each decrypted file next to its original. Then come back and choose the folder."});
    std::vector<Action> actions;
    if (mProps.onFolder)
        actions.push_back({"I have decrypted files",
                           [this] {
                               auto open = mProps.onFolder;
                               pop();
                               open();
                           },
                           Look::Primary});
    actions.push_back({"Back", [this] { back(); }, Look::Quiet});
    set_buttons(mActions, actions_row(content()), std::move(actions));
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}, {nullptr, "L1 R1", "Scroll"}});
}

std::vector<std::vector<Button*>> SetupHelpWindow::nav_lines() const {
    return {line(mActions)};
}

void SetupHelpWindow::back() {
    pop();
}

bool SetupHelpWindow::scroll(int pages) {
    return scroll_page(content(), pages);
}

// ---- 5. Ready to build ----

SetupBeforeWindow::SetupBeforeWindow(Props props) : SetupPage("setup-before"), mProps(std::move(props)) {
    using S = RailStep::State;
    set_rail({{"Your game", "", S::Now}, {"Build", build_time(), S::Todo}, {"Play", "", S::Todo}});
    set_heading("Ready to build", "Everything setup needs is here. The build runs on this PC and keeps its processor "
                                  "busy for a while.");
    mRows = append(content(), "setup-body");
    mActionsRow = actions_row(content());
    set_buttons(mActions, mActionsRow,
                {{"Start building", [this] { start(); }, Look::Primary}, {"Back", [this] { back(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
    draw();
}

std::vector<std::vector<Button*>> SetupBeforeWindow::nav_lines() const {
    return {line(mSpaceButton), line(mActions)};
}

// The power, asked again every 2 s: the player may plug in while this is up.
void SetupBeforeWindow::tick() {
    if (now_ms() - mPowerAt < 2000) return;
    mPowerAt = now_ms();
    if (setup::power().onBattery != mShownBattery) draw();
}

// The rows: the game, the free space (with Check again when it is short), this PC's time, and the power. Start
// building stays open with too little space: the need is an estimate, and the engine stops on a full disk itself.
void SetupBeforeWindow::draw() {
    const setup::Space space = setup::data_space();
    const setup::Estimate time = setup::setup_time();
    const setup::Power power = setup::power();
    mPowerAt = now_ms();
    mShownBattery = power.onBattery;
    const std::string where = space.where.empty() ? std::string("the disk") : space.where;
    const bool hadFocus = has_focus();
    mSpaceButton.clear();  // (their listeners go before their elements)
    clear_children(mRows);
    auto* box = panel(mRows);
    found(box, "setup-ok", "Demon's Souls, US disc, version 1.00",
          "From " + utf8(mProps.request.game) + ". The game files check out.");
    const bool tight = space.known && space.free < setup::kNeedBytes;
    std::string room = "Setup needs about " + gigabytes(setup::kNeedBytes) + " free on " + where + " while it builds. ";
    if (!space.known) room += "How much is free could not be read.";
    else if (tight) room += "Only " + gigabytes(space.free) + " is free. Free some space first.";
    else room += gigabytes(space.free) + " is free.";
    auto* roomRow = found(box, !space.known ? "setup-info" : tight ? "setup-err" : "setup-ok", "Disk space", room);
    if (tight)
        set_buttons(mSpaceButton, append(roomRow, "setup-row-end"), {{"Check again", [this] { draw(); }, Look::Quiet}});
    found(box, "setup-ok", "This PC",
          "About " + time_text(time) + " here, " + std::to_string(time.jobs) +
              " files at a time. The PC is kept awake while setup works.");
    if (power.onBattery)
        found(box, "setup-warn", "On battery", "Plug it in first: setup keeps this PC busy for a long time.");
    else if (power.battery)
        found(box, "setup-info", "On a laptop", "Keep it plugged in: the build is heavy work.");
    text(mRows, "setup-note", "You can stop setup and carry on later.")->SetClass("after", true);
    if (hadFocus && !button_focused() && visible()) focus();
    std::fprintf(stderr, "[setup] ready to build: %s free on %s, %u jobs, about %d to %d minutes, %s\n",
                 space.known ? gigabytes(space.free).c_str() : "unknown", where.c_str(), time.jobs, time.lowMinutes,
                 time.highMinutes, power.onBattery ? "on battery" : power.battery ? "plugged in" : "no battery");
}

void SetupBeforeWindow::back() {
    pop();
}

void SetupBeforeWindow::start() {
    if (!mProps.onStart) {
        pop();
        return;
    }
    hide(true);  // (not pop(): see SetupFilesWindow::start)
    mProps.onStart(mProps.request);
}

}  // namespace ui
