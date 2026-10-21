// The Trophies window (docs/44, section 5.4). Everything in it comes from sceNpTrophy_snapshot, which reads the trophy
// set (the disc's TROPHY.TRP) and the unlock file under the HLE's lock. It works before the game registers its
// trophies, so the window opens from the start screen too. The icons are the set's PNGs (sceNpTrophy_icon), served
// through host::add_png as the pop-ups' are. The list is read again when the unlock hook sees an unlock
// (trophy_toasts.cpp), so a trophy earned while the window is open shows at once.
//
// Laid out like the other windows (Dusklight's, CC0, by way of FFCCEOT-Recomp): one tab, the summary above one pane.
// The first row of the pane is "Show hidden trophies"; then one row per trophy, in the set's order. Up and Down move
// between rows, L1 and R1 (Page Up and Page Down) six rows at a time, and pressing a hidden trophy's row (Cross,
// Enter or a click) reveals it until the window closes.
#include "trophies_window.hpp"

#include "bool_button.hpp"
#include "pane.hpp"
#include "settings.hpp"
#include "trophy_toasts.hpp"
#include "ui.hpp"

#ifndef _WIN32
#include "platform.hpp"  // (localtime_s)
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

// ---- What this uses of the runtime (libs/network/sceNpTrophy.h, declared here: des_ui does not take ps3recomp's
// include paths) ----
extern "C" {
typedef struct sceNpTrophySnapshotEntry {
    int trophyId;
    unsigned grade;  // SCE_NP_TROPHY_GRADE_*: 1 platinum, 2 gold, 3 silver, 4 bronze
    unsigned char hidden;
    unsigned char unlocked;
    unsigned char reserved[2];
    unsigned long long unix_time;  // 0 while locked
    char name[128];                // UTF-8
    char detail[1024];
} sceNpTrophySnapshotEntry;
typedef struct sceNpTrophySnapshot {
    char commId[16];
    char title[128];
    char detail[1024];
    unsigned count;  // entries used in trophies[], in id order
    unsigned unlocked;
    unsigned per_grade[5];  // indexed by grade
    unsigned unlocked_per_grade[5];
    int percent;
    sceNpTrophySnapshotEntry trophies[128];
} sceNpTrophySnapshot;
int sceNpTrophy_snapshot(sceNpTrophySnapshot* out);
}
static_assert(sizeof(sceNpTrophySnapshotEntry) == 1176 && offsetof(sceNpTrophySnapshotEntry, name) == 24 &&
                  offsetof(sceNpTrophySnapshot, trophies) == 1224 &&
                  sizeof(sceNpTrophySnapshot) == 1224 + 128 * sizeof(sceNpTrophySnapshotEntry),
              "sceNpTrophySnapshot as libs/network/sceNpTrophy.h declares it");

namespace ui {
namespace {

constexpr int kPageRows = 6;                  // L1 / R1 move this many rows
constexpr unsigned long long kRetryMs = 2000;  // no trophy set found yet: look again this often
constexpr const char* kShowHidden = "trophies_show_hidden";

unsigned long long now_ms() {
    using namespace std::chrono;
    return static_cast<unsigned long long>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// DES_TROPHY_HIDDEN=<id>,<id>,... (a test knob, unset by default): this window treats those trophies as hidden, as if
// the set said so. Demon's Souls has no hidden trophy, so this is how a hidden row, and revealing it, can be seen.
// Nothing else sees it: not the HLE, the pop-ups or the unlock file. Read when the window opens.
std::set<int> test_hidden() {
    std::set<int> ids;
    const char* p = std::getenv("DES_TROPHY_HIDDEN");
    while (p && *p) {
        char* end = nullptr;
        const long id = std::strtol(p, &end, 10);
        if (end == p) {  // a separator
            p++;
            continue;
        }
        if (id >= 0 && id < 128) ids.insert(int(id));
        p = end;
    }
    return ids;
}

std::string text_of(const char* s, size_t max) {
    return std::string(s, strnlen(s, max));
}

// "18 Oct 2026, 21:04", in local time.
std::string unlock_date(unsigned long long unixTime) {
    static constexpr const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0 || tm.tm_mon < 0 || tm.tm_mon > 11) return {};
    char out[48];
    std::snprintf(out, sizeof out, "%d %s %d, %02d:%02d", tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min);
    return out;
}

std::string icon_rml(int id) {
    const std::string source = trophies::icon_source(id);
    return source.empty() ? "<icon class=\"trophy\"/>" : "<img src=\"" + source + "\"/>";
}

std::string grade_rml(unsigned grade, const std::string& text) {
    if (!*trophies::grade_class(grade)) return {};
    return std::string("<grade class=\"") + trophies::grade_class(grade) + "\"><icon class=\"trophy\"/>" + text +
           "</grade>";
}

}  // namespace

struct TrophiesWindow::State {
    std::unique_ptr<sceNpTrophySnapshot> snap = std::make_unique<sceNpTrophySnapshot>();  // (148 KB: not the stack)
    bool found = false;
    int hiddenCount = 0;
    std::set<int> testHidden = test_hidden();
    std::set<int> revealed;        // hidden trophies pressed in this window; not kept
    unsigned generation = 1;       // moves with every change the rows show
    std::vector<Component*> rows;  // the trophy rows, in order (for L1 / R1)

    void load() {
        found = sceNpTrophy_snapshot(snap.get()) == 0 && snap->count > 0;
        if (snap->count > 128) snap->count = 128;
        hiddenCount = 0;
        for (unsigned i = 0; found && i < snap->count; i++) {
            if (hidden(snap->trophies[i])) hiddenCount++;
        }
        generation++;
    }
    bool hidden(const sceNpTrophySnapshotEntry& t) const { return t.hidden || testHidden.count(t.trophyId); }
    bool concealed(const sceNpTrophySnapshotEntry& t) const {
        return hidden(t) && !t.unlocked && settings::get_int(kShowHidden) == 0 && !revealed.count(t.trophyId);
    }
};

namespace {

// One trophy: its icon, name and description, its grade, and when it was unlocked or "Locked". A hidden trophy that
// is still locked shows the lock symbol and "Hidden trophy" instead of its icon, name and description.
class TrophyRow : public FluentComponent<TrophyRow> {
public:
    TrophyRow(Rml::Element* parent, std::shared_ptr<TrophiesWindow::State> state, int index)
        : FluentComponent(append(parent, "trophy-row")), mState(std::move(state)), mIndex(index) {
        render();
        on_nav_command([this](Rml::Event&, NavCommand cmd) { return handle_nav_command(cmd); });
    }

    void update() override {
        if (mRendered != mState->generation) render();
        FluentComponent::update();
    }

private:
    const sceNpTrophySnapshotEntry& entry() const { return mState->snap->trophies[mIndex]; }

    // A hidden trophy that is still locked, while "Show hidden trophies" is off: pressing the row shows or hides it.
    bool can_reveal() const {
        const auto& t = entry();
        return mState->hidden(t) && !t.unlocked && settings::get_int(kShowHidden) == 0;
    }

    bool handle_nav_command(NavCommand cmd) {
        if (cmd == NavCommand::Confirm) {
            const auto& t = entry();
            if (can_reveal()) {
                const bool reveal = mState->revealed.erase(t.trophyId) == 0;
                if (reveal) mState->revealed.insert(t.trophyId);
                mState->generation++;
                play_sound(reveal ? kSoundItemEnable : kSoundItemDisable);
            }
            return true;  // (taken in any case: the window would move the focus to the top of the list)
        }
        if (cmd == NavCommand::Next || cmd == NavCommand::Previous) {
            const auto& rows = mState->rows;
            const auto it = std::find(rows.begin(), rows.end(), this);
            if (it == rows.end()) return false;
            const int at = int(it - rows.begin());
            const int to = std::clamp(at + (cmd == NavCommand::Next ? kPageRows : -kPageRows), 0, int(rows.size()) - 1);
            if (to != at && rows[to]->focus()) play_sound(kSoundItemFocus);
            return true;
        }
        return false;
    }

    void render() {
        mRendered = mState->generation;
        const auto& t = entry();
        const bool hidden = mState->hidden(t);
        const bool concealed = mState->concealed(t);
        mRoot->SetClass("unlocked", t.unlocked != 0);
        mRoot->SetClass("locked", t.unlocked == 0);
        mRoot->SetClass("concealed", concealed);
        mRoot->SetClass("can-reveal", can_reveal());

        std::string rml = "<trophy-icon>";
        rml += concealed ? "<icon class=\"trophy-hidden\"/>" : icon_rml(t.trophyId);
        rml += "</trophy-icon><trophy-text>";
        if (concealed) {
            rml += "<trophy-name>Hidden trophy</trophy-name><trophy-detail>Press to reveal it.</trophy-detail>";
        } else {
            const std::string name = text_of(t.name, sizeof t.name);
            rml += "<trophy-name>" + escape(name.empty() ? "Trophy " + std::to_string(t.trophyId) : name);
            if (hidden) rml += "<trophy-tag>Hidden</trophy-tag>";
            rml += "</trophy-name><trophy-detail>" + escape(text_of(t.detail, sizeof t.detail)) + "</trophy-detail>";
        }
        rml += "</trophy-text><trophy-meta>" + grade_rml(t.grade, trophies::grade_label(t.grade));
        if (t.unlocked) {
            const std::string date = t.unix_time ? unlock_date(t.unix_time) : std::string();
            rml += "<trophy-date>" + (date.empty() ? std::string("Unlocked") : escape(date)) + "</trophy-date>";
        } else {
            rml += "<trophy-date class=\"locked\"><icon class=\"lock\"/>Locked</trophy-date>";
        }
        rml += "</trophy-meta>";
        mRoot->SetInnerRML(rml);
    }

    std::shared_ptr<TrophiesWindow::State> mState;
    int mIndex;
    unsigned mRendered = 0;
};

}  // namespace

TrophiesWindow::TrophiesWindow()
    : Window(Props{.tabBar = true, .styleSheets = {"res/rml/trophies.rcss"}}),
      mState(std::make_shared<State>()) {
    mSeenUnlocks = trophies::unlock_count();  // (before the read: an unlock in between reads the list again)
    mState->load();
    mNextRetryAt = now_ms() + kRetryMs;
    std::fprintf(stderr, "[ui] trophies window: %s\n",
                 mState->found ? (std::to_string(mState->snap->unlocked) + " of " +
                                  std::to_string(mState->snap->count) + " unlocked, " +
                                  std::to_string(mState->hiddenCount) + " hidden")
                                     .c_str()
                               : "no trophy set found");
    add_tab("Trophies", [this](Rml::Element* content) { build_tab(content); });
}

void TrophiesWindow::build_tab(Rml::Element* content) {
    mState->rows.clear();
    mSummary = append(content, "trophy-summary");
    render_summary();

    auto& pane = add_child<Pane>(content, Pane::Type::Uncontrolled);
    const auto state = mState;
    pane.add_child<BoolButton>(BoolButton::Props{
        .key = "Show hidden trophies",
        .getValue = [] { return settings::get_int(kShowHidden) != 0; },
        .setValue =
            [state](bool on) {
                settings::set(kShowHidden, on ? "1" : "0");
                state->generation++;
            },
        .isDisabled = [state] { return state->hiddenCount == 0; },
        .isModified = [] { return !settings::is_default(kShowHidden); },
        .valueOverride = [state]() -> Rml::String { return state->hiddenCount == 0 ? "None in this game" : ""; },
    });
    if (!mState->found) {
        pane.add_rml("<trophy-note>The game's trophy set (PS3_GAME/TROPDIR on the disc) was not found, so there is "
                     "nothing to list.</trophy-note>");
        return;
    }
    for (unsigned i = 0; i < mState->snap->count; i++) {
        mState->rows.push_back(&pane.add_child<TrophyRow>(mState, int(i)));
    }
}

void TrophiesWindow::render_summary() {
    if (mSummary == nullptr) return;
    const sceNpTrophySnapshot& s = *mState->snap;
    if (!mState->found) {
        mSummary->SetInnerRML("<summary-icon><icon class=\"trophy\"/></summary-icon><summary-text>"
                              "<summary-title>Trophies</summary-title><summary-line>No trophy set found"
                              "</summary-line></summary-text>");
        return;
    }
    const std::string title = text_of(s.title, sizeof s.title);
    std::string rml = "<summary-icon>" + icon_rml(-1) + "</summary-icon><summary-text>";
    rml += "<summary-title>" + escape(title.empty() ? std::string("Trophies") : title) + "</summary-title>";
    rml += "<summary-line><summary-count>" + std::to_string(s.unlocked) + " of " + std::to_string(s.count) +
           " unlocked</summary-count><summary-percent>" + std::to_string(s.percent) + "%</summary-percent>" +
           "</summary-line>";
    char fraction[16];
    std::snprintf(fraction, sizeof fraction, "%.4f", s.count ? double(s.unlocked) / double(s.count) : 0.0);
    rml += std::string("<progress value=\"") + fraction + "\"/><grade-counts>";
    for (unsigned g = 1; g <= 4; g++) {
        if (s.per_grade[g] == 0) continue;
        rml += grade_rml(g, std::string(trophies::grade_label(g)) + " " + std::to_string(s.unlocked_per_grade[g]) +
                                " / " + std::to_string(s.per_grade[g]));
    }
    if (mState->hiddenCount > 0) {
        rml += "<grade class=\"hidden\"><icon class=\"trophy-hidden\"/>" + std::to_string(mState->hiddenCount) +
               " hidden</grade>";
    }
    rml += "</grade-counts></summary-text>";
    mSummary->SetInnerRML(rml);
}

void TrophiesWindow::update() {
    const unsigned unlocks = trophies::unlock_count();
    const unsigned long long now = now_ms();
    if (unlocks != mSeenUnlocks || (!mState->found && now >= mNextRetryAt)) {
        mSeenUnlocks = unlocks;
        mNextRetryAt = now + kRetryMs;
        const unsigned before = mState->found ? mState->snap->count : 0;
        mState->load();
        const unsigned after = mState->found ? mState->snap->count : 0;
        if (after != before) {
            refresh_active_tab();  // the set appeared (or changed): build the list again
        } else {
            render_summary();  // (the rows follow the generation)
        }
    }
    Window::update();
}

}  // namespace ui
