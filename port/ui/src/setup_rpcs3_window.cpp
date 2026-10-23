// The setup screens' RPCS3 route (see setup_rpcs3_window.hpp), on the setup shell as the setup design draws it. The
// texts are the screens'; the engine's lines say only what it found (@rpcs3, @user, @library, @check, @found, @setting)
// and what failed (setup_protocol.hpp, docs/90 section 3.8).
#include "setup_rpcs3_window.hpp"

#include "host.hpp"
#include "settings.hpp"
#include "setup_pc.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != tail[i]) return false;
    return true;
}

bool good(const std::string& state) {
    return state == "ok" || state == "unmeasured";
}

// A @check line's part, by its table name, as the game folder page sorts them (setup_screens.cpp).
enum class Part { Game, Disc, Eboot, Script, Firmware };

Part part_of(const std::string& name) {
    if (ends_with(name, ".prx")) return Part::Firmware;
    if (ends_with(name, ".elf")) return Part::Eboot;
    if (ends_with(name, ".unedat")) return Part::Script;
    return name.find('/') == std::string::npos ? Part::Game : Part::Disc;
}

// "19 Oct 2026, 22:41" from Unix seconds, in this PC's time zone; empty for "-".
std::string saved_at(const std::string& seconds) {
    char* end = nullptr;
    const long long s = std::strtoll(seconds.c_str(), &end, 10);
    if (seconds.empty() || *end || s <= 0) return {};
    const std::time_t t = std::time_t(s);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &t)) return {};
#else
    if (!localtime_r(&t, &local)) return {};
#endif
    char text[48];
    return std::strftime(text, sizeof text, "%d %b %Y, %H:%M", &local) ? text : "";
}

long long number(const std::string& s) {
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return s.empty() || *end ? -1 : v;
}

// One of the engine's @found lines: its state, and its values ("-": unknown).
struct Found {
    std::string state;
    std::vector<std::string> values;
    std::string value(size_t i) const { return i < values.size() && values[i] != "-" ? values[i] : std::string(); }
};

Found found_of(const setup::Status& s, const char* what) {
    const auto it = s.found.find(what);
    if (it == s.found.end()) return {};
    return {it->second.state, it->second.values};
}

std::string imported_setting(const setup::Status& s, const char* key) {
    for (const auto& [k, v] : s.settings)
        if (k == key) return v;
    return {};
}

// The imported settings as the row says them, in docs/90 1.3's order: "Frame rate 60 (Unlock FPS patch), render
// resolution 150%, motion blur off, intro skipped, 16x anisotropic filtering".
std::string settings_words(const setup::Status& s) {
    std::vector<std::string> parts;
    if (const std::string fps = imported_setting(s, "DES_FPS"); !fps.empty())
        parts.push_back("frame rate " + (fps == "unlocked" ? std::string("unlocked") : fps) + " (Unlock FPS patch)");
    if (const std::string scale = imported_setting(s, "RSX_SCALE"); !scale.empty())
        parts.push_back("render resolution " + scale + "%");
    if (imported_setting(s, "DES_MOTION_BLUR") == "0") parts.push_back("motion blur off");
    if (imported_setting(s, "DES_SKIP_INTRO") == "1") parts.push_back("intro skipped");
    if (const std::string aniso = imported_setting(s, "RSX_ANISO"); !aniso.empty() && aniso != "0")
        parts.push_back(aniso + "x anisotropic filtering");
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    if (!out.empty()) out[0] = char(std::toupper((unsigned char)out[0]));
    return out;
}

// "3 characters" / "1 character"; empty when unknown.
std::string characters(const std::string& n) {
    const long long v = number(n);
    if (v < 0) return {};
    return std::to_string(v) + (v == 1 ? " character" : " characters");
}

// L1 and R1: the page's content up or down by most of its height.
bool scroll_page(Rml::Element* page, int pages) {
    const float step = std::max(24.f, page->GetClientHeight() * 0.8f);
    page->SetScrollTop(std::max(0.f, page->GetScrollTop() + step * float(pages)));
    return true;
}

}  // namespace

// ---- 1. Your game in RPCS3 ----

SetupRpcs3Window::SetupRpcs3Window(Props props)
    : SetupPage("setup-rpcs3"), mProps(std::move(props)), mRequest(mProps.request),
      mPicked(std::make_shared<Mailbox<PickedFolder>>()) {
    set_rail({{"Your game", "", RailStep::State::Now}, {"Build", build_time_line(), RailStep::State::Todo}, options_step(),
              {"Play", "", RailStep::State::Todo}});
    mBody = append(content(), "setup-body");
    set_buttons(mActions, footer_row(),
                {{"Continue", [this] { go_on(); }, Look::Primary}, {"Back", [this] { back(); }, Look::Quiet}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}, {nullptr, "L1 R1", "Scroll"}});
    check();
    draw();
}

bool SetupRpcs3Window::focus() {
    if (asking()) return SetupPage::focus();
    if (mStatus.result == "ok" && !mActions.empty() && mActions[0]->focus()) return true;
    if (!mGameButtons.empty() && mGameButtons[0]->focus()) return true;
    if (!mRowButtons.empty() && mRowButtons[0]->focus()) return true;
    return SetupPage::focus();
}

std::vector<std::vector<Button*>> SetupRpcs3Window::nav_lines() const {
    std::vector<std::vector<Button*>> out;
    for (const auto& b : mRowButtons) out.push_back({b.get()});
    if (!mLinkButtons.empty()) out.push_back(line(mLinkButtons));
    if (!mGameButtons.empty()) out.push_back(line(mGameButtons));
    for (const auto& b : mChecks) out.push_back({b.get()});
    out.push_back(line(mActions));
    return out;
}

void SetupRpcs3Window::tick() {
    if (PickedFolder folder; mPicked->take(folder)) picked(folder);
    if (mChecking) mEngine.poll(mStatus);
    draw();
}

void SetupRpcs3Window::back() {
    pop();
}

bool SetupRpcs3Window::scroll(int pages) {
    return scroll_page(content(), pages);
}

// `run --check-only --rpcs3`: the RPCS3 lines, then an @check line for each file, and the verdict.
void SetupRpcs3Window::check() {
    mStatus = {};
    mTickedSet = false;
    mRequest.rpcs3 = setup::rpcs3_arg(mProps.rpcs3);
    mChecking = mEngine.start(mRequest, true);
    if (!mChecking) {
        mStatus.result = "failed";
        mStatus.errorText = mEngine.why();
    }
    std::fprintf(stderr, "[setup] reading RPCS3 in %s%s\n", utf8(mProps.rpcs3.folder).c_str(),
                 mRequest.game.empty() ? "" : (", with the game folder " + utf8(mRequest.game)).c_str());
}

void SetupRpcs3Window::choose(Picking what) {
    if (mPicking != Picking::None) return;  // (one dialog at a time)
    mPicking = what;
    const bool rpcs3 = what == Picking::Rpcs3;
    pick_folder(rpcs3 ? mProps.rpcs3.folder : mRequest.game,
                rpcs3 ? "Choose the folder that holds RPCS3" : "Choose the folder of your disc copy", host::window(),
                [box = mPicked](PickedFolder folder) { box->put(std::move(folder)); });
}

void SetupRpcs3Window::picked(const PickedFolder& folder) {
    const Picking what = std::exchange(mPicking, Picking::None);
    if (folder.failed) {
        mPickProblem = "The folder window could not open on this PC.";
        return;
    }
    if (folder.folder.empty()) return;  // (closed without a choice)
    mPickProblem.clear();
    if (what == Picking::Rpcs3) {
        const setup::Rpcs3 r = setup::rpcs3_at(folder.folder);
        if (!r.found()) {
            mPickProblem = "There is no RPCS3 in this folder. Pick the folder that holds rpcs3.exe.";
            return;
        }
        mProps.rpcs3 = r;
    } else {
        mRequest.game = folder.folder;
    }
    check();
}

// The game is one setup can build from: the check passed, with the game in RPCS3's library or the folder chosen.
bool SetupRpcs3Window::supported() const {
    return !(mChecking && !mStatus.finished()) && mStatus.result == "ok";
}

// The page, drawn again only when what it shows has changed: while the check runs, a line with the count; then the
// RPCS3 row, its user, the game, and what can come across.
void SetupRpcs3Window::draw() {
    const bool running = mChecking && !mStatus.finished();
    std::string key = std::to_string(int(running)) + "|" + mStatus.result + "|" + std::to_string(mStatus.progressN) +
                      "|" + std::to_string(mStatus.checks.size()) + "|" + mStatus.errorText + "|" + mPickProblem +
                      "|" + utf8(mProps.rpcs3.folder) + "|" + utf8(mRequest.game) + "|" + mStatus.libraryKind;
    for (const bool t : mTicked) key += t ? "1" : "0";
    if (key == mShown) return;
    mShown = key;

    const auto place = focus_place();
    const bool hadFocus = has_focus();
    mRowButtons.clear();  // (their listeners go before their elements)
    mLinkButtons.clear();
    mGameButtons.clear();
    mChecks.clear();
    clear_children(mBody);

    set_heading("Your game in RPCS3",
                mStatus.rpcs3State == "shared"
                    ? "Setup reads your game, save and settings from RPCS3, and never changes them. To unlock the game "
                      "files it starts your RPCS3 once, which replaces RPCS3's last log."
                    : "Setup reads your game, save and settings from RPCS3. It never changes RPCS3's files or settings.");

    auto* box = panel(mBody);
    auto* rpcs3Row = found(box, "setup-ok", "RPCS3", utf8(mProps.rpcs3.folder));
    set_buttons(mRowButtons, append(rpcs3Row, "setup-row-end"),
                {{"Change", [this] { choose(Picking::Rpcs3); }, Look::Quiet}});
    if (!mPickProblem.empty()) callout(mBody, "err", mPickProblem, "");

    if (running) {
        std::string line = "Reading your RPCS3...";
        if (mStatus.progress == "check" && mStatus.progressM > 0)
            line = "Checking the game files: " + grouped(mStatus.progressN >> 20) + " of " +
                   grouped(std::max(1LL, mStatus.progressM >> 20)) + " MB";
        else if (mStatus.current() && mStatus.current()->name == setup::kPythonStep)
            line = "Downloading Python first...";
        auto* row = append(box, "setup-row");
        row->SetClass("start", true);
        append(row, "setup-spin");
        text(row, "setup-note", line);
    } else {
        if (mStatus.users >= 2)
            found(box, "setup-ok", "RPCS3 user",
                  (mStatus.userName.empty() ? std::string("User") : mStatus.userName) + " (" + mStatus.userId +
                      "). RPCS3 has " + std::to_string(mStatus.users) + " users; this is the one it uses now.");

        std::vector<const setup::Line*> game, disc, other;
        for (const setup::Line& c : mStatus.checks) {
            const Part p = part_of(c.key2);
            (p == Part::Game ? game : p == Part::Disc ? disc : other).push_back(&c);
        }
        const auto bad = [](const setup::Line* c) { return !good(c->key); };
        const bool isGame = !game.empty() && std::none_of(game.begin(), game.end(), bad);
        const std::string where = !mRequest.game.empty()      ? utf8(mRequest.game)
                                  : !mStatus.libraryPath.empty() ? mStatus.libraryPath
                                                                 : utf8(mProps.rpcs3.game);
        const auto tag = [](Rml::Element* row, const char* kind, const char* words) {
            text(row, "setup-tag", words)->SetClass(kind, true);
        };
        const auto check_again = [this](Rml::Element* row) {
            set_buttons(mLinkButtons, append(row, "setup-row-end"), {{"Check again", [this] { check(); }, Look::Quiet}});
        };
        if (mRequest.game.empty() && mStatus.libraryKind == "iso") {
            auto* row = found(box, "setup-err", "Demon's Souls, as an ISO file",
                              "Setup cannot use ISO files yet. Copy the game out of the ISO into a folder, add that "
                              "folder to RPCS3's library, then press Check again.");
            tag(row, "err", "Not yet");
            check_again(row);
        } else if (mRequest.game.empty() && mStatus.libraryKind == "store") {
            auto* row = found(box, "setup-err", "Demon's Souls, PlayStation Store version (NPUB30910)",
                              "Only the disc version, BLUS30443, is supported. The store version is packaged and "
                              "locked differently.");
            tag(row, "err", "Not supported");
        } else if (mRequest.game.empty() && (mStatus.libraryKind == "none" || mStatus.libraryKind.empty()) &&
                   game.empty()) {
            auto* row = found(box, "setup-warn", "Demon's Souls is not in RPCS3's library",
                              "Add it in RPCS3 (File, then Add Games), then press Check again. Or choose the game "
                              "folder here.");
            check_again(row);
            auto* more = actions_row(mBody);
            more->SetClass("near", true);
            set_buttons(mGameButtons, more, {{"Choose the game folder", [this] { choose(Picking::Game); }, Look::Plain}});
        } else if (isGame) {
            const auto wrong = std::find_if(disc.begin(), disc.end(), bad);
            if (wrong == disc.end()) {
                auto* row = found(box, "setup-ok", "Demon's Souls, US disc, version 1.00", "BLUS30443 · " + where);
                tag(row, "ok", "Supported");
            } else {
                auto* row = found(box, "setup-err", "Demon's Souls, US disc, version 1.00", (*wrong)->text);
                tag(row, "err", "Needs fixing");
                check_again(row);
            }
        } else if (!game.empty()) {  // (another region or a folder that is not the game: the line says why)
            auto* row = found(box, "setup-err", "This is not a copy setup can use",
                              (*std::find_if(game.begin(), game.end(), bad))->text);
            tag(row, "err", "Not supported");
            check_again(row);
        } else {  // (the engine's sentence: RPCS3 gone, the first Python's download, the disk)
            std::string why = mStatus.errorText.empty() ? std::string("Check again.") : mStatus.errorText;
            if (mStatus.errorCode == "download") why += " Check the internet connection, then check again.";
            else if (mStatus.errorCode == "disk") why += " Free some space, then check again.";
            check_again(found(box, "setup-err", "The check did not finish", why));
        }

        // A file the unlock cannot make (the PS3 system software not installed in RPCS3, a file that does not match):
        // the check's sentence says what to do.
        if (isGame && mStatus.result != "ok")
            for (const setup::Line* c : other)
                if (bad(c) && c->key != "missing") found(box, "setup-err", c->key2.substr(c->key2.rfind('/') + 1), c->text);

        if (supported()) draw_imports(mBody);
    }

    const bool ok = supported();
    if (!mActions.empty()) mActions[0]->set_disabled(!ok);
    if (ok && !mShownOk && visible() && !asking()) {
        mActions[0]->focus();
        reveal_focus();
    } else if (hadFocus && !button_focused() && visible() && !focus_at(place)) {
        focus();
    }
    mShownOk = ok;
    if (!running && !mStatus.result.empty())
        std::fprintf(stderr, "[setup] RPCS3's check: %s, %s; library %s, user %s of %lld\n", mStatus.result.c_str(),
                     mStatus.errorCode.empty() ? "-" : mStatus.errorCode.c_str(),
                     mStatus.libraryKind.empty() ? "-" : mStatus.libraryKind.c_str(),
                     mStatus.userId.empty() ? "-" : mStatus.userId.c_str(), mStatus.users);
}

// "Bring across from RPCS3": a box for each, ticked when the engine found it, and not tickable when not. The save
// starts unticked when this install already has a newer one (docs/90 question 4).
void SetupRpcs3Window::draw_imports(Rml::Element* parent) {
    const Found save = found_of(mStatus, "save"), current = found_of(mStatus, "current-save"),
                trophies = found_of(mStatus, "trophies"), settingsFound = found_of(mStatus, "settings");
    const bool can[kItems] = {save.state == "ok", trophies.state == "ok",
                              settingsFound.state == "ok" && !settings_words(mStatus).empty()};
    if (!mTickedSet) {
        mTickedSet = true;
        for (int i = 0; i < kItems; ++i) mTicked[i] = can[i];
        if (can[Save] && current.state == "ok" && number(current.value(0)) > number(save.value(1)))
            mTicked[Save] = false;
    }

    std::string saveLine;
    if (save.state == "ok") {
        const std::string count = characters(save.value(0)), when = saved_at(save.value(1));
        if (!count.empty()) saveLine = count + (when.empty() ? "." : ", last saved " + when + ".");
        else if (!when.empty()) saveLine = "Last saved " + when + ".";
        saveLine += (saveLine.empty() ? "" : " ") + std::string("Copied, so RPCS3 keeps its own.");
        if (current.state == "ok") {
            const std::string mine = saved_at(current.value(0));
            saveLine += " It replaces the save you have here" + (mine.empty() ? "" : " (saved " + mine + ")") +
                        ". A backup is kept.";
        }
    } else if (save.state == "other-user") {
        saveLine = "This RPCS3 user has no Demon's Souls save. Another user has one: switch to that user in RPCS3, then "
                   "press Check again.";
    } else if (save.state == "busy") {
        saveLine = "RPCS3 is saving the game right now. Close the game in RPCS3, then press Check again.";
    } else {
        saveLine = "This RPCS3 user has no Demon's Souls save.";
    }
    std::string trophyLine = "No trophies yet for this RPCS3 user.";
    if (trophies.state == "ok") {
        const std::string earned = trophies.value(0);
        const size_t slash = earned.find('/');
        trophyLine = slash == std::string::npos ? earned + " unlocked."
                                                : earned.substr(0, slash) + " of " + earned.substr(slash + 1) +
                                                      " unlocked.";
    }
    const std::string settingsLine =
        can[Settings] ? settings_words(mStatus) + ". After the build, the Custom options start from these."
                      : "RPCS3 runs the game with its own settings, so there is nothing to bring across.";

    text(parent, "setup-heading", "Bring across from RPCS3");
    auto* box = panel(parent);
    const struct {
        Item item;
        const char* key;
        const std::string& line;
    } kRows[] = {{Save, "Save data", saveLine}, {Trophies, "Trophies", trophyLine},
                 {Settings, "Settings and patches", settingsLine}};
    for (const auto& r : kRows) {
        auto button = std::make_unique<Button>(box, "");
        Rml::Element* root = button->root();
        root->SetClass("setup-check", true);
        auto* mark = icon(root, "setup-box");
        mark->SetClass("on", mTicked[r.item]);
        auto* words = append(root, "setup-found-text");
        text(words, "setup-found-key", r.key);
        text(words, "setup-found-value", r.line);
        button->set_disabled(!can[r.item]);
        button->on_pressed([this, item = r.item] {
            play_sound(kSoundItemChange);
            mPending = [this, item] { mTicked[item] = !mTicked[item]; };
        });
        mChecks.push_back(std::move(button));
    }
}

// Continue: the unlock page while files are still to unlock, else Ready to build. The RPCS3 is remembered (settings.ini's
// rpcs3_dir), so the card finds it at once next time.
void SetupRpcs3Window::go_on() {
    if (!supported()) return;
    settings::store("rpcs3_dir", utf8(mProps.rpcs3.folder));
    setup::Request request = mRequest;
    request.imports.clear();
    static const char* const kNames[kItems] = {"save", "trophies", "settings"};
    for (int i = 0; i < kItems; ++i)
        if (mTicked[i]) request.imports.push_back(kNames[i]);

    bool eboot = false, firmware = false;
    int scripts = 0;
    for (const setup::Line& c : mStatus.checks) {
        if (good(c.key)) continue;
        switch (part_of(c.key2)) {
        case Part::Eboot: eboot = true; break;
        case Part::Script: ++scripts; break;
        case Part::Firmware: firmware = true; break;
        default: break;
        }
    }

    // Ready to build's lines: where the game is from, and what comes across.
    const std::string from = mRequest.game.empty() ? std::string("From RPCS3's library.")
                                                   : "From " + utf8(mRequest.game) + ", unlocked by your RPCS3.";
    std::vector<std::string> items;
    bool plural = false;  // (the verb after them: "Your save comes", "14 trophies come")
    if (mTicked[Save]) {
        const std::string count = characters(found_of(mStatus, "save").value(0));
        items.push_back(count.empty() ? "Your save" : "Your save (" + count + ")");
    }
    if (mTicked[Trophies]) {
        const std::string earned = found_of(mStatus, "trophies").value(0);
        const long long n = number(earned.substr(0, earned.find('/')));
        items.push_back(n < 0 ? "your trophies" : std::to_string(n) + (n == 1 ? " trophy" : " trophies"));
        plural = n != 1;
    }
    std::string importLine;
    if (!items.empty()) {
        items[0][0] = char(std::toupper((unsigned char)items[0][0]));
        importLine = items[0] + (items.size() > 1 ? " and " + items[1] : std::string()) +
                     (items.size() > 1 || plural ? " come" : " comes") + " across when the build is done.";
    }
    if (mTicked[Settings])
        importLine += (importLine.empty() ? "" : " ") + std::string("Your RPCS3 settings are offered on the options page.");

    const auto start = [this](setup::Request r) {
        force_hide(true);  // (covered by the next page: it closes without coming back)
        if (mProps.onStart) mProps.onStart(std::move(r));
    };
    if (eboot || scripts || firmware) {
        push(std::make_unique<SetupUnlockWindow>(SetupUnlockWindow::Props{
            .request = request,
            .shared = mStatus.rpcs3State == "shared",
            .eboot = eboot,
            .scripts = scripts,
            .firmware = firmware,
            .fromLine = from + " All the unlocked files match.",
            .importLine = importLine,
            .onStart = start,
        }));
        return;
    }
    push(std::make_unique<SetupBeforeWindow>(SetupBeforeWindow::Props{
        .request = request,
        .onStart = start,
        .fromLine = from + " The game files check out.",
        .importLine = importLine,
    }));
}

// ---- 2. RPCS3 was not found ----

SetupRpcs3MissingWindow::SetupRpcs3MissingWindow(Props props)
    : SetupPage("setup-rpcs3-missing"), mProps(std::move(props)), mPicked(std::make_shared<Mailbox<PickedFolder>>()) {
    set_rail({{"Your game", "", RailStep::State::Now}, {"Build", build_time_line(), RailStep::State::Todo}, options_step(),
              {"Play", "", RailStep::State::Todo}});
    set_heading("RPCS3 was not found",
                "Setup looked in the usual places and did not find RPCS3. If it is somewhere else, show setup the "
                "folder that holds rpcs3.exe. If you do not have it yet, it takes a few minutes to get ready.");
    auto* field = append(content(), "setup-field");
    field->SetClass("folder", true);
    icon(field, "setup-folder");
    mFolderText = append(field, "setup-field-text");
    set_text_content(mFolderText, "No folder chosen");
    mFolderText->SetClass("none", true);
    mProblem = append(content(), "setup-body");
    text(content(), "setup-note", "Looked in: " + setup::rpcs3_looked_in() + ".")->SetClass("after", true);
    std::vector<Action> actions = {{"Choose folder", [this] { choose(); }, Look::Primary}};
    actions.push_back({"How to get RPCS3 ready",
                       [this] { push(std::make_unique<SetupHelpWindow>(SetupHelpWindow::Props{})); }, Look::Plain});
    actions.push_back({"Back", [this] { back(); }, Look::Quiet});
    set_buttons(mActions, footer_row(), std::move(actions));
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
}

std::vector<std::vector<Button*>> SetupRpcs3MissingWindow::nav_lines() const {
    return {line(mActions)};
}

void SetupRpcs3MissingWindow::tick() {
    PickedFolder folder;
    if (!mPicked->take(folder)) return;
    mPicking = false;
    clear_children(mProblem);
    if (folder.failed) {
        callout(mProblem, "err", "The folder window could not open on this PC.", "");
        return;
    }
    if (folder.folder.empty()) return;
    set_text_content(mFolderText, utf8(folder.folder));
    mFolderText->SetClass("none", false);
    const setup::Rpcs3 r = setup::rpcs3_at(folder.folder);
    std::fprintf(stderr, "[setup] RPCS3 picked: %s, %s\n", utf8(folder.folder).c_str(), r.found() ? "found" : "none");
    if (!r.found()) {
        callout(mProblem, "err", "There is no RPCS3 in this folder", "Pick the folder that holds rpcs3.exe.");
        return;
    }
    auto found = mProps.onFound;
    pop();
    if (found) found(r);
}

void SetupRpcs3MissingWindow::back() {
    pop();
}

void SetupRpcs3MissingWindow::choose() {
    if (mPicking) return;
    mPicking = true;
    pick_folder({}, "Choose the folder that holds RPCS3", host::window(),
                [box = mPicked](PickedFolder folder) { box->put(std::move(folder)); });
}

// ---- 3. Unlocking the game files ----

SetupUnlockWindow::SetupUnlockWindow(Props props) : SetupPage("setup-unlock"), mProps(std::move(props)) {
    set_rail({{"Your game", "", RailStep::State::Now}, {"Build", build_time_line(), RailStep::State::Todo}, options_step(),
              {"Play", "", RailStep::State::Todo}});
    std::string what;
    if (mProps.eboot && mProps.scripts == 26 && !mProps.firmware)
        what = "Your RPCS3 unlocks the game's code and its 26 script files.";
    else if (!mProps.eboot && mProps.scripts == 26 && !mProps.firmware)
        what = "The game's code is already unlocked, so your RPCS3 unlocks the 26 script files.";
    else {
        std::vector<std::string> parts;
        if (mProps.eboot) parts.push_back("the game's code");
        if (mProps.scripts)
            parts.push_back(std::to_string(mProps.scripts) + (mProps.scripts == 1 ? " script file" : " script files"));
        if (mProps.firmware) parts.push_back("one file of the PS3 system software");
        what = "Your RPCS3 unlocks ";
        for (size_t i = 0; i < parts.size(); ++i)
            what += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
        what += ".";
    }
    set_heading("Unlocking the game files",
                what + " It works on copies in setup's own folder, and setup checks each result.");
    mBody = append(content(), "setup-body");
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
    if (!mProps.shared) start();
    draw();
}

std::vector<std::vector<Button*>> SetupUnlockWindow::nav_lines() const {
    return {line(mActions)};
}

void SetupUnlockWindow::tick() {
    if (mRunning) mEngine.poll(mStatus);
    draw();
}

// Back while it unlocks stops it: the copies go with it (the engine cleans setup/unlock/ up after a failure too).
void SetupUnlockWindow::back() {
    mEngine.stop();
    pop();
}

void SetupUnlockWindow::start() {
    mStatus = {};
    mDetailsShown = false;
    mRunning = mEngine.start(mProps.request, false, true);
    if (!mRunning) {
        mStatus.result = "failed";
        mStatus.errorCode = "internal";
        mStatus.errorText = mEngine.why();
    }
}

void SetupUnlockWindow::draw() {
    const bool running = mRunning && !mStatus.finished();
    const bool before = !mRunning && mStatus.result.empty();  // (a portable RPCS3, not started yet)
    const bool done = mStatus.result == "ok";
    const std::string key = std::to_string(int(running)) + std::to_string(int(before)) + "|" + mStatus.result + "|" +
                            std::to_string(mStatus.progressN) + "/" + std::to_string(mStatus.progressM) + "|" +
                            mStatus.file + "|" + std::to_string(int(mDetailsShown)) + "|" +
                            (mStatus.current() ? mStatus.current()->name : "");
    if (key == mShown) return;
    mShown = key;
    const bool hadFocus = has_focus();
    mActions.clear();
    clear_children(mBody);
    std::vector<Action> actions;
    const int total = int(mProps.eboot) + mProps.scripts + int(mProps.firmware);

    if (before) {
        callout(mBody, "warn", "Close RPCS3 first",
                "Setup starts your RPCS3 once to unlock the files. Like any start of RPCS3, this replaces its last log.");
        actions.push_back({"Unlock", [this] { start(); }, Look::Primary});
        actions.push_back({"Back", [this] { back(); }, Look::Quiet});
    } else if (running || done) {
        auto* box = panel(mBody, true);
        auto* row = append(box, "setup-row");
        const bool unlocking = mStatus.progress == "unlock" && mStatus.progressM > 0;
        const long long m = unlocking ? mStatus.progressM : total;
        std::string left = done ? "All " + std::to_string(m) + " files unlocked and checked"
                                : unlocking ? "File " + std::to_string(std::min(m, mStatus.progressN + 1)) + " of " +
                                                  std::to_string(m)
                                            : std::string("Checking the game files first...");
        text(row, "setup-text", left);
        if (!done && unlocking && !mStatus.file.empty()) text(row, "setup-eta", mStatus.file);
        auto* bar = append(box, "progress");
        bar->SetClass("setup-meter", true);
        bar->SetAttribute("value", done ? 1.f : unlocking ? float(mStatus.progressN) / float(m) : 0.f);
        if (done) {
            std::string files = mProps.eboot ? "EBOOT.elf" : "";
            if (mProps.scripts)
                files += (files.empty() ? "" : mProps.firmware ? ", " : " and ") + std::to_string(mProps.scripts) +
                         (mProps.scripts == 1 ? " script file" : " script files");
            if (mProps.firmware) files += (files.empty() ? "" : " and ") + std::string("libsre.prx");
            found(box, "setup-ok", files, "All match the known good version. The copies are deleted.");
            actions.push_back({"Continue", [this] { go_on(); }, Look::Primary});
        } else {
            actions.push_back({"Stop", [this] { back(); }, Look::Danger});
        }
    } else {  // failed or stopped
        auto* body = callout(mBody, "err", mStatus.errorText.empty() ? "The unlock stopped." : mStatus.errorText,
                             "Try again. If it keeps failing, you can unlock the files yourself.");
        if (mDetailsShown) {
            std::string tail;
            const size_t from = mStatus.tail.size() > 12 ? mStatus.tail.size() - 12 : 0;
            for (size_t i = from; i < mStatus.tail.size(); ++i)
                tail += (i > from ? "\n" : "") + mStatus.tail[i].substr(0, 300);
            if (!tail.empty()) append(body, "setup-log")->SetInnerRML(escape_lines(tail));
        }
        actions.push_back({"Try again", [this] { start(); }, Look::Primary});
        actions.push_back(
            {"Show me how", [this] { push(std::make_unique<SetupHelpWindow>(SetupHelpWindow::Props{})); }, Look::Plain});
        actions.push_back({mDetailsShown ? "Hide details" : "Details",
                           [this] { mDetailsShown = !mDetailsShown; }, Look::Quiet});
        actions.push_back({"Back", [this] { back(); }, Look::Quiet});
    }
    set_buttons(mActions, footer_row(), std::move(actions));
    if (visible() && !asking() && (hadFocus || done)) focus();
    if (!running && !mStatus.result.empty())
        std::fprintf(stderr, "[setup] the unlock: %s, %s\n", mStatus.result.c_str(),
                     mStatus.errorCode.empty() ? "-" : mStatus.errorCode.c_str());
}

void SetupUnlockWindow::go_on() {
    push(std::make_unique<SetupBeforeWindow>(SetupBeforeWindow::Props{
        .request = mProps.request,
        .onStart =
            [this](setup::Request r) {
                force_hide(true);
                if (mProps.onStart) mProps.onStart(std::move(r));
            },
        .fromLine = mProps.fromLine,
        .importLine = mProps.importLine,
    }));
}

}  // namespace ui
