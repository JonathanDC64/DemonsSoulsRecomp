// Written for Demon's Souls (ps3recomp's docs/ONLINE.md, "Sign-in"). See net_notices.hpp.
#include "net_notices.hpp"

#include "privacy.hpp"
#include "settings.hpp"
#include "ui.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

// ---- What this uses of the runtime (ps3emu/ps3_net_signin.h, declared here: des_ui does not take ps3recomp's
// include paths) ----
extern "C" int ps3_net_signin_notice(char* out, size_t cap);  // 1 and the message, once per message
extern "C" int ps3_net_signin_banner(char* out, size_t cap);  // 1 and a new banner, once per banner; never blocks
extern "C" int ps3_net_players_result(int* ok, char* out, size_t cap);  // a block's or report's outcome, once
extern "C" int ps3_net_erase_result(int* ok, int* kept, char* out, size_t cap);  // Delete my data's outcome, once

namespace ui::net_notices {
namespace {

constexpr auto kToastDuration = std::chrono::seconds(10);  // a refusal's (longer than a trophy's: more to read)
constexpr unsigned kPollFrames = 15;                         // the runtime is asked every this many frames
constexpr std::size_t kMaxWaiting = 8;                       // (more than that waiting: the newest are dropped)

enum class Kind {
    Refusal,  // the sign-in's or psnr's, or a block or report the server turned down: a warning toast
    Banner,   // the server's: the thin bar at the top
    Done,     // a block, report or deletion that went through (players_window.cpp, settings_window.cpp): a toast
};

struct Message {
    Kind kind;
    std::string text;
};

std::mutex sLock;
std::deque<Message> sWaiting;
std::atomic<bool> sPending{false};
unsigned sFrames = 0;

void keep(Kind kind, const std::string& text) {
    std::lock_guard<std::mutex> lock(sLock);
    if (sWaiting.size() < kMaxWaiting) sWaiting.push_back({kind, text});
    sPending.store(true, std::memory_order_release);
}

}  // namespace

void poll() {
    if (++sFrames % kPollFrames) return;
    char text[256];
    int ok = 0;
    if (ps3_net_signin_notice(text, sizeof text)) keep(Kind::Refusal, text);
    // (not before the privacy note's answer: the first call starts the fetches from the server)
    if (!privacy::unanswered())
        while (ps3_net_signin_banner(text, sizeof text)) keep(Kind::Banner, text);
    if (ps3_net_players_result(&ok, text, sizeof text)) keep(ok ? Kind::Done : Kind::Refusal, text);
    // The data deleted: online stays off from the next start too (this run is locked offline already), since a
    // sign-in would start a new record.
    int kept = 0;
    if (ps3_net_erase_result(&ok, &kept, text, sizeof text)) {
        if (ok) {
            settings::net_off("Off: your data on the server was deleted");
            if (!settings::pinned("DES_ONLINE")) settings::set("DES_ONLINE", "0");
        }
        keep(ok ? Kind::Done : Kind::Refusal,
             ok ? std::string(text) + " Online play is off now; turning it on again starts a new record." : text);
    }
}

bool pending() {
    return sPending.load(std::memory_order_acquire);
}

void drain() {
    if (!pending()) return;
    std::deque<Message> waiting;
    {
        std::lock_guard<std::mutex> lock(sLock);
        waiting.swap(sWaiting);
        sPending.store(false, std::memory_order_relaxed);
    }
    for (const Message& m : waiting) {
        // The server's words, so as text, never as markup. A banner goes in the thin bar at the top (it does not
        // cover the game); a refusal in a toast.
        if (m.kind == Kind::Banner) {
            push_ticker(m.text);
        } else {
            push_toast({
                .type = m.kind == Kind::Refusal ? "warning" : "",
                .title = "Online",
                .content = "<toast-message-text>" + escape(m.text) + "</toast-message-text>",
                .duration = kToastDuration,
            });
        }
        std::fprintf(stderr, "[ui] online %s shown: %s\n",
                     m.kind == Kind::Banner ? "banner" : m.kind == Kind::Done ? "outcome" : "notice", m.text.c_str());
    }
}

}  // namespace ui::net_notices
