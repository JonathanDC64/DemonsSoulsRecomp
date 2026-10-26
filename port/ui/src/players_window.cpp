// The Players window (players_window.hpp). Everything in it comes from the runtime's ps3_net_players calls: the window
// asks for the list when it opens, and draws it again whenever an answer comes (the runtime counts them). A block or a
// report goes to the runtime, which sends it, asks for the list again, and hands its outcome to net_notices.cpp, so
// the toast comes even when the window has closed by then.
//
// Laid out like the Trophies window (trophies_window.cpp): one tab, the summary above one pane, one row per player.
// Up and Down move between rows, L1 and R1 (Page Up and Page Down) six rows at a time, and pressing a row (Cross, Enter
// or a click) opens what can be done with that player.
#include "players_window.hpp"

#include "modal.hpp"
#include "pane.hpp"
#include "portable.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// ---- What this uses of the runtime (ps3emu/ps3_net_signin.h, declared here: des_ui does not take ps3recomp's
// include paths) ----
extern "C" {
typedef struct ps3_net_player {
    char name[20];
    char fingerprint[20];
    char met[12];  // "help", "invasion", "monk"; "" for one blocked and not met lately
    int host;      // the meeting was in this player's world
    long long at;  // seconds since 1970 UTC; -1 unknown
    int blocked;
    int reported;
    int reportable;
} ps3_net_player;
int ps3_net_players_refresh(void);
int ps3_net_players(ps3_net_player* out, int max, int* state, int* days, unsigned* serial, char* msg, size_t mcap);
int ps3_net_player_block(const char* name, int blocked);
int ps3_net_player_report(const char* name, const char* reason);
}
static_assert(sizeof(ps3_net_player) == 80 && offsetof(ps3_net_player, at) == 56 &&
                  offsetof(ps3_net_player, reportable) == 72,
              "ps3_net_player as ps3emu/ps3_net_signin.h declares it");

namespace ui {
namespace {

constexpr int kMaxPlayers = 100;  // PS3_NET_PLAYERS_MAX
enum { kNone, kAsking, kReady, kFailed };  // PS3_NET_PLAYERS_*
constexpr int kPageRows = 6;
constexpr unsigned long long kRetryMs = 15000;  // no sign-in yet, or a failed ask: ask again this often

std::string text_of(const char* s, size_t max) {
    return std::string(s, strnlen(s, max));
}

// How the two met, as this player sees it.
std::string how_met(const ps3_net_player& p) {
    const std::string met = text_of(p.met, sizeof p.met);
    if (met == "help") return p.host ? "Came to help you" : "You helped them";
    if (met == "invasion") return p.host ? "Invaded you" : "You invaded them";
    if (met == "monk") return p.host ? "Came as your Old Monk" : "You were their Old Monk";
    return "Blocked";
}

const char* met_icon(const ps3_net_player& p) {
    const std::string met = text_of(p.met, sizeof p.met);
    if (met == "help") return "met-help";
    if (met == "invasion") return "met-invasion";
    if (met == "monk") return "met-monk";
    return "met-blocked";
}

// "just now", "5 minutes ago", "3 hours ago", "2 days ago".
std::string ago(long long at) {
    if (at <= 0) return {};
    const long long d = std::max(0LL, (long long)std::time(nullptr) - at);
    const auto count = [](long long n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (d < 60) return "just now";
    if (d < 3600) return count(d / 60, "minute");
    if (d < 86400) return count(d / 3600, "hour");
    return count(d / 86400, "day");
}

// The key's fingerprint in groups of four: "a1b2 c3d4 e5f6 0718".
std::string key_text(const ps3_net_player& p) {
    const std::string f = text_of(p.fingerprint, sizeof p.fingerprint);
    std::string out;
    for (size_t i = 0; i < f.size(); i += 4) out += (i ? " " : "") + f.substr(i, 4);
    return out;
}

void busy_toast() {
    push_toast({
        .type = "warning",
        .title = "Players",
        .content = "<toast-message-text>The last change is still on its way. Try again in a moment."
                   "</toast-message-text>",
        .duration = std::chrono::seconds(5),
    });
}

void open_report(const ps3_net_player& p);

// What can be done with a player: Block or Unblock, and Report.
void open_actions(const ps3_net_player& p) {
    const std::string name = text_of(p.name, sizeof p.name);
    std::string body = escape(how_met(p));
    if (!ago(p.at).empty()) body += ", " + escape(ago(p.at));
    body += ".<br/>Their key: " + escape(key_text(p)) + "<br/><br/>";
    body += p.blocked ? "Unblock lets you meet them online again."
                      : "Block: you no longer see each other's summon signs, and neither of you can summon or "
                        "invade the other. You can unblock them here.";
    if (p.reported) body += "<br/><br/>You reported them today.";
    const bool blocked = p.blocked != 0;
    const bool reportable = p.reportable != 0;
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = name,
        .bodyRml = body,
        .actions =
            {
                ModalAction{.label = "Cancel",
                    .onPressed =
                        [](Modal& modal) {
                            play_sound(kSoundWindowClose);
                            modal.pop();
                        }},
                ModalAction{.label = blocked ? "Unblock" : "Block",
                    .onPressed =
                        [name, blocked](Modal& modal) {
                            if (ps3_net_player_block(name.c_str(), blocked ? 0 : 1) != 0) {
                                busy_toast();
                                return;
                            }
                            play_sound(blocked ? kSoundItemEnable : kSoundItemDisable);
                            modal.pop();
                        }},
                ModalAction{.label = "Report",
                    .onPressed =
                        [p](Modal& modal) {
                            play_sound(kSoundClick);
                            modal.pop();
                            open_report(p);
                        },
                    .isDisabled = [reportable] { return !reportable; }},
            },
        .onDismiss = [](Modal& modal) { modal.pop(); },
        .icon = "question-mark",
    }));
}

// Report a player: why, then the runtime sends it.
void open_report(const ps3_net_player& p) {
    const std::string name = text_of(p.name, sizeof p.name);
    const auto reason = [name](const char* label, const char* why) {
        return ModalAction{.label = label,
            .onPressed = [name, why](Modal& modal) {
                if (ps3_net_player_report(name.c_str(), why) != 0) {
                    busy_toast();
                    return;
                }
                play_sound(kSoundClick);
                modal.pop();
            }};
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Report " + name + "?",
        .bodyRml = "The server's admins see the report, with how and when you met. Why?",
        .actions =
            {
                reason("Cheating", "cheating"),
                reason("Offensive name", "name"),
                reason("Other", "other"),
                ModalAction{.label = "Cancel",
                    .onPressed =
                        [](Modal& modal) {
                            play_sound(kSoundWindowClose);
                            modal.pop();
                        }},
            },
        .onDismiss = [](Modal& modal) { modal.pop(); },
        .icon = "warning",
        .isVertical = true,
    }));
}

}  // namespace

struct PlayersWindow::State {
    std::vector<ps3_net_player> players;  // the list shown
    int status = kNone;
    int days = 0;
    unsigned serial = 0;
    std::string why;               // kFailed: for the player
    bool signedIn = false;         // the runtime took the ask (online play is on, with a sign-in)
    unsigned generation = 1;       // moves with every change the rows show
    std::vector<Component*> rows;  // the player rows, in order (for L1 / R1)

    void load() {
        std::vector<ps3_net_player> got(kMaxPlayers);
        char msg[256] = "";
        const int n = ps3_net_players(got.data(), kMaxPlayers, &status, &days, &serial, msg, sizeof msg);
        got.resize(std::clamp(n, 0, kMaxPlayers));
        players = std::move(got);
        why = msg;
        generation++;
    }
};

namespace {

// One player: how you met, their name and key, when, and whether you blocked or reported them.
class PlayerRow : public FluentComponent<PlayerRow> {
public:
    PlayerRow(Rml::Element* parent, std::shared_ptr<PlayersWindow::State> state, int index)
        : FluentComponent(append(parent, "player-row")), mState(std::move(state)), mIndex(index) {
        render();
        on_nav_command([this](Rml::Event&, NavCommand cmd) { return handle_nav_command(cmd); });
    }

    void update() override {
        if (mRendered != mState->generation) render();
        FluentComponent::update();
    }

private:
    bool handle_nav_command(NavCommand cmd) {
        if (cmd == NavCommand::Confirm) {
            if (mIndex < int(mState->players.size())) {
                play_sound(kSoundClick);
                open_actions(mState->players[mIndex]);
            }
            return true;
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
        if (mIndex >= int(mState->players.size())) return;
        const ps3_net_player& p = mState->players[mIndex];
        mRoot->SetClass("blocked", p.blocked != 0);
        std::string rml = std::string("<player-icon><icon class=\"") + met_icon(p) + "\"/></player-icon><player-text>";
        rml += "<player-name>" + escape(text_of(p.name, sizeof p.name));
        if (p.blocked) rml += "<player-tag class=\"blocked\">Blocked</player-tag>";
        if (p.reported) rml += "<player-tag>Reported</player-tag>";
        rml += "</player-name><player-detail>" + escape(how_met(p)) + "<player-key>key " +
               escape(text_of(p.fingerprint, sizeof p.fingerprint).substr(0, 8)) + "</player-key></player-detail>";
        rml += "</player-text><player-date>" + escape(ago(p.at)) + "</player-date>";
        mRoot->SetInnerRML(rml);
    }

    std::shared_ptr<PlayersWindow::State> mState;
    int mIndex;
    unsigned mRendered = 0;
};

}  // namespace

PlayersWindow::PlayersWindow()
    : Window(Props{.tabBar = true, .styleSheets = {"res/rml/players.rcss"}}), mState(std::make_shared<State>()) {
    mState->signedIn = ps3_net_players_refresh() == 0;
    mState->load();
    mNextAskAt = now_ms() + kRetryMs;
    std::fprintf(stderr, "[ui] players window: %s, %zu rows so far\n",
                 mState->signedIn ? "asking the server" : "no sign-in", mState->players.size());
    add_tab("Players", [this](Rml::Element* content) { build_tab(content); });
}

void PlayersWindow::build_tab(Rml::Element* content) {
    mState->rows.clear();
    mSummary = append(content, "players-summary");
    render_summary();
    auto& pane = add_child<Pane>(content, Pane::Type::Uncontrolled);
    for (int i = 0; i < int(mState->players.size()); i++) {
        mState->rows.push_back(&pane.add_child<PlayerRow>(mState, i));
    }
}

void PlayersWindow::render_summary() {
    if (mSummary == nullptr) return;
    const State& s = *mState;
    std::string line;
    if (!s.signedIn) {
        line = "Play online to see the players you met: Settings > Network > Online.";
    } else if (s.status == kFailed) {
        line = escape(s.why);
    } else if (s.status != kReady) {
        line = "Asking the online server...";
    } else if (s.players.empty()) {
        line = "No one in the last " + std::to_string(s.days) + " days. Players who help you, you help, or who "
               "invade show here.";
    } else {
        line = "The last " + std::to_string(s.days) + " days, the latest first. Press a player to block or report "
               "them.";
    }
    mSummary->SetInnerRML("<summary-icon><icon class=\"players\"/></summary-icon><summary-text><summary-title>Players "
                          "you met</summary-title><summary-line>" + line + "</summary-line></summary-text>");
}

void PlayersWindow::update() {
    const unsigned long long now = now_ms();
    if ((!mState->signedIn || mState->status == kFailed) && now >= mNextAskAt) {
        mNextAskAt = now + kRetryMs;
        const bool was = mState->signedIn;
        mState->signedIn = ps3_net_players_refresh() == 0;
        if (mState->signedIn != was) render_summary();
    }
    unsigned serial = 0;
    ps3_net_players(nullptr, 0, nullptr, nullptr, &serial, nullptr, 0);
    if (serial != mState->serial) {
        const size_t before = mState->players.size();
        mState->load();
        std::fprintf(stderr, "[ui] players window: %zu rows (state %d)\n", mState->players.size(), mState->status);
        if (mState->players.size() != before) {
            refresh_active_tab();  // (rows come and go: build the list again)
        } else {
            render_summary();  // (the rows follow the generation)
        }
    }
    Window::update();
}

}  // namespace ui
