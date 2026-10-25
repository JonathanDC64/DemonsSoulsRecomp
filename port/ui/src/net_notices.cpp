// Written for Demon's Souls (ps3recomp's docs/ONLINE.md, "Sign-in"). See net_notices.hpp.
#include "net_notices.hpp"

#include "ui.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>

// ---- What this uses of the runtime (ps3emu/ps3_net_signin.h, declared here: des_ui does not take ps3recomp's
// include paths) ----
extern "C" int ps3_net_signin_notice(char* out, size_t cap);  // 1 and the message, once per message

namespace ui::net_notices {
namespace {

constexpr auto kToastDuration = std::chrono::seconds(10);  // (longer than a trophy's: there is more to read)
constexpr unsigned kPollFrames = 15;                         // the runtime is asked every this many frames

std::mutex sLock;
std::string sText;
std::atomic<bool> sPending{false};
unsigned sFrames = 0;

}  // namespace

void poll() {
    if (++sFrames % kPollFrames) return;
    char text[256];
    if (!ps3_net_signin_notice(text, sizeof text)) return;
    std::lock_guard<std::mutex> lock(sLock);
    sText = text;
    sPending.store(true, std::memory_order_release);
}

bool pending() {
    return sPending.load(std::memory_order_acquire);
}

void drain() {
    if (!pending()) return;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(sLock);
        text.swap(sText);
        sPending.store(false, std::memory_order_relaxed);
    }
    // The server's words, so as text, never as markup.
    push_toast({
        .type = "warning",
        .title = "Online",
        .content = "<toast-message-text>" + escape(text) + "</toast-message-text>",
        .duration = kToastDuration,
    });
    std::fprintf(stderr, "[ui] online notice shown: %s\n", text.c_str());
}

}  // namespace ui::net_notices
