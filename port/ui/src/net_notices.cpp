// Written for Demon's Souls (ps3recomp's docs/ONLINE.md, "Sign-in"). See net_notices.hpp.
#include "net_notices.hpp"

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

namespace ui::net_notices {
namespace {

constexpr auto kToastDuration = std::chrono::seconds(10);  // a refusal's (longer than a trophy's: more to read)
constexpr unsigned kPollFrames = 15;                         // the runtime is asked every this many frames
constexpr std::size_t kMaxWaiting = 8;                       // (more than that waiting: the newest are dropped)

struct Message {
    bool warning;  // a refusal; else a banner
    std::string text;
};

std::mutex sLock;
std::deque<Message> sWaiting;
std::atomic<bool> sPending{false};
unsigned sFrames = 0;

void keep(bool warning, const char* text) {
    std::lock_guard<std::mutex> lock(sLock);
    if (sWaiting.size() < kMaxWaiting) sWaiting.push_back({warning, text});
    sPending.store(true, std::memory_order_release);
}

}  // namespace

void poll() {
    if (++sFrames % kPollFrames) return;
    char text[256];
    if (ps3_net_signin_notice(text, sizeof text)) keep(true, text);
    while (ps3_net_signin_banner(text, sizeof text)) keep(false, text);
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
        if (m.warning) {
            push_toast({
                .type = "warning",
                .title = "Online",
                .content = "<toast-message-text>" + escape(m.text) + "</toast-message-text>",
                .duration = kToastDuration,
            });
        } else {
            push_ticker(m.text);
        }
        std::fprintf(stderr, "[ui] online %s shown: %s\n", m.warning ? "notice" : "banner", m.text.c_str());
    }
}

}  // namespace ui::net_notices
