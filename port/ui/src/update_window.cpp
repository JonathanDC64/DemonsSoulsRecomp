// The update's screen (see update_window.hpp; the engine's side is tools/setup/des_setup/update.py, docs/105).
#include "update_window.hpp"

#include "app.hpp"
#include "crash_report.hpp"
#include "portable.hpp"
#include "report.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <utility>

namespace ui {
namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// "25 October 2026" from Unix seconds; empty when not known.
std::string date_of(long long when) {
    if (when <= 0) return {};
    const std::time_t t = std::time_t(when);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char out[64];
    if (!std::strftime(out, sizeof out, "%d %B %Y", &tm)) return {};
    return out[0] == '0' ? out + 1 : out;
}

// A line of Markdown as plain words: ** and ` dropped, [words](link) as its words.
std::string plain(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '`' || (c == '*' && i + 1 < s.size() && s[i + 1] == '*')) {
            if (c == '*') ++i;
            continue;
        }
        if (c == '[') {
            const size_t close = s.find("](", i);
            const size_t end = close == std::string_view::npos ? close : s.find(')', close);
            if (end != std::string_view::npos) {
                out += s.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// The release's notes as the page's parts: headings, list items (a deeper one indented) and lines of text.
void draw_notes_into(Rml::Element* parent, const std::vector<std::string>& notes) {
    for (const std::string& raw : notes) {
        const size_t indent = raw.find_first_not_of(' ');
        if (indent == std::string::npos) continue;
        std::string_view line(raw);
        line.remove_prefix(indent);
        if (line[0] == '#') {
            line.remove_prefix(std::min(line.find_first_not_of('#'), line.size()));
            line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
            append_text_element(parent, "update-note-head", plain(line));
        } else if (line.size() > 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            auto* item = append_text_element(parent, "update-note-item", "\xE2\x80\xA2 " + plain(line.substr(2)));
            if (indent >= 2) item->SetClass("sub", true);
        } else {
            append_text_element(parent, "update-note-text", plain(line));
        }
    }
}

bool scroll_page(Rml::Element* page, int pages) {
    const float step = std::max(24.f, page->GetClientHeight() * 0.8f);
    page->SetScrollTop(std::max(0.f, page->GetScrollTop() + step * float(pages)));
    return true;
}

}  // namespace

UpdateWindow::UpdateWindow(Props props) : SetupPage("setup-update"), mProps(std::move(props)) {
    mBody = append(content(), "setup-body");
    mBack = mProps.open == Open::Back;
    if (mProps.open == Open::Notes) draw_notes();
    else start();
}

void UpdateWindow::draw_notes() {
    using S = RailStep::State;
    const updates::Release& r = mProps.release;
    mPhase = Phase::Notes;
    const std::string date = date_of(r.when);
    set_heading("Version " + r.version + " is out", "You have version " + updates::this_version() + "." +
                                                     (date.empty() ? "" : " It came out on " + date + "."));
    mMain.clear();  // (their listeners go before their elements)
    mMore.clear();
    clear_children(mBody);
    callout(mBody, "info", "Updating keeps your saves and settings",
            "Setup downloads the new version, checks it, puts it in place, then builds the game again for it. The "
            "version you have now is kept, so you can go back to it in Settings, About.");
    auto* box = panel(mBody, true);
    box->SetClass("update-notes", true);
    if (r.notes.empty()) text(box, "update-note-text", "This release has no notes. Its page has the details.");
    else draw_notes_into(box, r.notes);
    std::vector<Action> main = {{"Update", [this] { update_now(); }, Look::Primary},
                                {"Later", [this] { close(); }, Look::Quiet}};
    if (!updates::skipped())
        main.push_back({"Skip this version",
                        [this] {
                            updates::skip();
                            close();
                        },
                        Look::Quiet});
    set_buttons(mMain, footer_row(), std::move(main));
    std::vector<Action> more;
    if (!r.page.empty()) more.push_back({"Release page", [page = r.page] { report::open_url(page); }, Look::Quiet});
    set_buttons(mMore, footer_end(), std::move(more));
    set_rail({{"New version", r.version, S::Now}, {"Download", "", S::Todo}, {"Build", "", S::Todo},
              {"Play", "", S::Todo}});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Later"}, {nullptr, "L1 R1", "Scroll"}});
}

// Update: at once on setup's screens alone; over the game, the program starts again for it first.
void UpdateWindow::update_now() {
    play_sound(kSoundClick);
    if (mProps.overGame) {
        updates::restart_into("update-run", mProps.release.version);
        return;
    }
    mBack = false;
    start();
}

void UpdateWindow::start() {
    mStatus = {};
    mPhase = Phase::Running;
    draw_running();
    const std::string install = utf8(updates::install_dir()), data = utf8(setup::data_dir());
    std::vector<std::string> args = {mBack ? "update-back" : "update-apply", "--app", updates::this_version()};
    if (!mBack) args.insert(args.end(), {"--to", mProps.release.version});
    args.insert(args.end(), {"--install", install, "--data", data});
    if (!mEngine.start_command(args)) {  // (shown by the next tick)
        mStatus.result = "failed";
        mStatus.errorCode = "internal";
        mStatus.errorStep = "-";
        mStatus.errorText = mEngine.why();
    }
}

void UpdateWindow::draw_running() {
    using S = RailStep::State;
    const std::string kept = updates::kept_version();
    if (mBack)
        set_heading("Going back to version " + kept, "Setup puts version " + kept +
                                                         " back in place, with its game build. Your saves and "
                                                         "settings stay.");
    else
        set_heading("Updating to version " + mProps.release.version,
                    "Setup downloads it, checks it, then puts it in place of version " + updates::this_version() +
                        ". Your saves and settings stay.");
    mMain.clear();
    mMore.clear();
    clear_children(mBody);
    mShownPercent.clear();
    mShownWhat.clear();
    mShownStop = false;
    auto* box = panel(mBody, true);
    auto* row = append(box, "setup-row");
    mPercent = append(row, "setup-percent");
    mWhat = append(row, "setup-eta");
    mBar = append(box, "progress");
    mBar->SetClass("setup-meter", true);
    mBar->SetAttribute("value", 0.f);
    set_buttons(mMain, footer_row(), {});
    set_buttons(mMore, footer_end(), {});
    if (mBack)
        set_rail({{"Earlier version", kept, S::Busy}, {"Play", "", S::Todo}});
    else
        set_rail({{"New version", mProps.release.version, S::Done}, {"Download", "", S::Busy}, {"Build", "", S::Todo},
                  {"Play", "", S::Todo}});
    set_hints({{"cross", nullptr, "Select"}});
    refresh();
}

void UpdateWindow::refresh() {
    const int pm = std::clamp(mStatus.overall, 0, 1000);
    const std::string percent = std::to_string(pm / 10) + "%";
    if (percent != mShownPercent) {
        set_text_content(mPercent, percent);
        mBar->SetAttribute("value", float(pm) / 1000.f);
        mShownPercent = percent;
    }
    const setup::Status::Step* now = mStatus.current();
    const bool downloading = now && now->name == "download";
    std::string what;
    if (downloading && mStatus.progress == "download" && mStatus.progressM > 0)
        what = "Downloading, " + std::to_string(mStatus.progressN >> 20) + " of " +
               std::to_string(std::max(1LL, mStatus.progressM >> 20)) + " MB";
    else if (downloading)
        what = "Downloading";
    else if (now && now->name == "install")
        what = mBack ? "Putting it back" : "Putting it in place";
    if (what != mShownWhat) {
        set_text_content(mWhat, what);
        mShownWhat = what;
    }
    // The swap is a second's renames: nothing stops it half way, not even the window's close button.
    updates::hold_close(now && now->name == "install");
    if (downloading != mShownStop) {
        mShownStop = downloading;
        set_buttons(mMain, footer_row(),
                    downloading ? std::vector<Action>{{"Stop", [this] { ask_stop(); }, Look::Danger}}
                                : std::vector<Action>{});
        if (downloading && !asking()) focus();
    }
}

void UpdateWindow::tick() {
    if (mPhase != Phase::Running) return;
    mEngine.poll(mStatus);
    refresh();
    if (mStatus.finished()) finish();
}

void UpdateWindow::finish() {
    updates::hold_close(false);
    const std::string& r = mStatus.result;
    std::fprintf(stderr, "[update] %s ended: %s%s%s\n", mBack ? "going back" : "the update", r.c_str(),
                 mStatus.errorText.empty() ? "" : ", ", mStatus.errorText.c_str());
    if (r == "stopped") {
        close();
        return;
    }
    if (r != "ok") {
        mPhase = Phase::Failed;
        draw_failed();
        return;
    }
    // In place: the program starts again from the same file, now the other version's. After an update it goes
    // straight into setup's rebuild; after going back, the kept build is that version's, so the game starts.
    mPhase = Phase::Done;
    const std::string to = mBack ? updates::kept_version() : mProps.release.version;
    set_heading(mBack ? "Version " + updates::this_version() + " is kept" : "Version " + to + " is in place",
                mBack ? "The game starts again as the earlier version."
                      : "The game is built again for it now. Your game files are kept, so there is nothing to choose.");
    // Gone back: the version left is not offered again on the start screen (Settings, About still has it).
    if (mBack) settings::store("update_skip", updates::this_version());
    std::vector<std::string> set;
    if (!mBack) set.push_back("DES_UI_OPEN=setup-run");
    if (const char* as = std::getenv("DES_UPDATE_AS_VERSION"); as && *as && !to.empty())
        set.push_back("DES_UPDATE_AS_VERSION=" + to);  // (a test's: the new one is that version)
    if (crash_report::start_again(set, false)) {
        if (app().quit) app().quit();
        return;
    }
    mStatus.errorCode = "internal";
    mStatus.errorText = "The program could not start again. Start the game again yourself.";
    mPhase = Phase::Failed;
    draw_failed();
}

void UpdateWindow::draw_failed() {
    const bool changed = mPhase == Phase::Failed && mStatus.result == "ok";  // (in place, but no new start)
    set_heading(mBack ? "Going back stopped" : "The update stopped",
                changed ? "The files are in place." : "Nothing was changed: this is still version " +
                                                          updates::this_version() + ".");
    mMain.clear();
    mMore.clear();
    clear_children(mBody);
    auto* body = callout(mBody, "err", mStatus.errorText.empty() ? "Setup stopped unexpectedly." : mStatus.errorText,
                         "");
    std::string tail;
    const size_t from = mStatus.tail.size() > 8 ? mStatus.tail.size() - 8 : 0;
    for (size_t i = from; i < mStatus.tail.size(); ++i) tail += (tail.empty() ? "" : "\n") + mStatus.tail[i];
    if (!tail.empty()) append(body, "setup-log")->SetInnerRML(escape_lines(tail));
    set_buttons(mMain, footer_row(),
                changed ? std::vector<Action>{{"Close", [this] { close(); }, Look::Primary}}
                        : std::vector<Action>{{"Try again", [this] { start(); }, Look::Primary},
                                              {"Back", [this] { close(); }, Look::Quiet}});
    set_buttons(mMore, footer_end(), {});
    set_hints({{"cross", nullptr, "Select"}, {"circle", nullptr, "Back"}});
    if (!asking()) focus();
}

void UpdateWindow::ask_stop() {
    ask("Stop the update?", escape("What is downloaded is kept, so the next try carries on from there."),
        {{"Stop", [this] { mEngine.stop(); }, Look::Danger}, {"Keep going", {}, Look::Plain}}, 1);
}

void UpdateWindow::close() {
    updates::hold_close(false);
    mEngine.stop();
    pop();
}

std::vector<std::vector<Button*>> UpdateWindow::nav_lines() const {
    std::vector<Button*> all = line(mMain);
    for (Button* b : line(mMore)) all.push_back(b);
    return {all};
}

void UpdateWindow::back() {
    if (mPhase == Phase::Running) {
        if (mShownStop) ask_stop();  // (not while it swaps)
    } else if (mPhase != Phase::Done) {
        close();
    }
}

bool UpdateWindow::scroll(int pages) {
    return scroll_page(content(), pages);
}

}  // namespace ui
