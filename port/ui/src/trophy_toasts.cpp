// Trophy pop-ups (docs/44, section 5.3). The sceNpTrophy HLE calls the unlock hook on the game's thread once it has
// saved the unlock, and the hook only queues. frame() wakes for a queued unlock and, on the render thread, turns it
// into a toast on the overlay: the trophy's icon (its PNG in the disc's TROPHY.TRP, served through host::add_png),
// "Trophy unlocked" with the grade, and the name. One toast at a time, about 5 s each; a platinum comes right after
// the trophy that completed it. Settings, Interface, "Trophy pop-ups" off hides the toasts only.
#include "trophy_toasts.hpp"

#include "host.hpp"
#include "settings.hpp"
#include "ui.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

// ---- What this uses of the runtime (libs/network/sceNpTrophy.h and ps3emu/ps3_knob.h, declared here: des_ui does
// not take ps3recomp's include paths) ----
extern "C" {
typedef struct sceNpTrophyUnlockEvent {
    int trophyId;
    unsigned grade;  // SCE_NP_TROPHY_GRADE_*: 1 platinum, 2 gold, 3 silver, 4 bronze
    unsigned char hidden;
    unsigned char platinum;  // 1: the platinum, completed by the trophy before it
    unsigned char from_set;  // 0: no TROPHY.TRP was found, and the name is a placeholder
    unsigned char reserved;
    unsigned long long unix_time;
    char commId[16];
    char name[128];  // UTF-8
    char detail[1024];
} sceNpTrophyUnlockEvent;
typedef void (*sceNpTrophyUnlockHook)(const sceNpTrophyUnlockEvent* ev, void* user);
void sceNpTrophy_set_unlock_hook(sceNpTrophyUnlockHook hook, void* user);
int sceNpTrophy_icon(int trophyId, void** png, unsigned* size);
void sceNpTrophy_free(void* p);
int sceNpTrophy_debug_unlock(int trophyId);
extern volatile unsigned ps3_env_gen;  // the debug console's setenv bumps it
}
static_assert(sizeof(sceNpTrophyUnlockEvent) == 1192 && offsetof(sceNpTrophyUnlockEvent, name) == 40,
              "sceNpTrophyUnlockEvent as libs/network/sceNpTrophy.h declares it");

namespace ui::trophies {
namespace {

using clock_type = std::chrono::steady_clock;

constexpr auto kToastDuration = std::chrono::seconds(5);
constexpr size_t kMaxQueued = 64;  // (a set has at most 128 trophies, and they come one or two at a time)
constexpr unsigned kNotInitialized = 0x80022902u;       // SCE_NP_TROPHY_ERROR_NOT_INITIALIZED
constexpr unsigned kContextNotRegistered = 0x80022904u;  // SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED

struct Unlock {
    int id;
    unsigned grade;
    std::string name;
};

std::mutex sLock;
std::deque<Unlock> sQueue;
std::atomic<bool> sPending{false};

// The game's thread, inside sceNpTrophyUnlockTrophy after the unlock is saved: copy and queue, nothing else.
void on_unlock(const sceNpTrophyUnlockEvent* ev, void*) {
    if (!ev) return;
    Unlock u{ev->trophyId, ev->grade, std::string(ev->name, strnlen(ev->name, sizeof ev->name))};
    std::lock_guard<std::mutex> lock(sLock);
    if (sQueue.size() >= kMaxQueued) return;
    sQueue.push_back(std::move(u));
    sPending.store(true, std::memory_order_release);
}

struct Grade {
    const char* css;
    const char* label;
};

Grade grade_of(unsigned grade) {
    switch (grade) {
    case 1: return {"platinum", "Platinum"};
    case 2: return {"gold", "Gold"};
    case 3: return {"silver", "Silver"};
    case 4: return {"bronze", "Bronze"};
    default: return {"", ""};
    }
}

// The trophy's icon as RML: its PNG from the trophy set, or the trophy symbol when there is none (no TROPHY.TRP).
std::string icon_rml(int id) {
    void* png = nullptr;
    unsigned size = 0;
    const bool found = sceNpTrophy_icon(id, &png, &size) == 0 && png && size;
    std::string rml = "<icon class=\"trophy\"/>";
    if (found) {
        const std::string source = "trophy-icon:" + std::to_string(id);
        host::add_png(source, png, size);
        rml = "<img src=\"" + source + "\"/>";
    }
    if (png) sceNpTrophy_free(png);
    return rml;
}

Toast make_toast(const Unlock& u) {
    const Grade grade = grade_of(u.grade);
    Toast t;
    t.type = "trophy";
    // (markup: create_toast reads a title or a message that starts with "<" as RML)
    t.title = "<toast-title>Trophy unlocked</toast-title>";
    if (*grade.label) {
        t.title += std::string("<grade class=\"") + grade.css + "\"><icon class=\"trophy\"/>" + grade.label + "</grade>";
    }
    t.content = "<trophy-name>" + escape(u.name.empty() ? "Trophy " + std::to_string(u.id) : u.name) +
                "</trophy-name>";
    t.duration = kToastDuration;
    t.icon = icon_rml(u.id);
    return t;
}

}  // namespace

void install() {
    sceNpTrophy_set_unlock_hook(on_unlock, nullptr);
}

bool pending() {
    return sPending.load(std::memory_order_acquire);
}

void drain() {
    if (!pending()) return;
    std::deque<Unlock> items;
    {
        std::lock_guard<std::mutex> lock(sLock);
        items.swap(sQueue);
        sPending.store(false, std::memory_order_relaxed);
    }
    const bool show = settings::get_int("trophy_popups") != 0;
    for (const Unlock& u : items) {
        if (show) {
            Toast t = make_toast(u);
            std::fprintf(stderr, "[ui] trophy %d: pop-up %s\n", u.id,
                         t.icon.starts_with("<img") ? "with its icon" : "with the trophy symbol (no icon in the set)");
            push_toast(std::move(t));
        } else {
            std::fprintf(stderr, "[ui] trophy %d unlocked, not shown (Settings, Interface, Trophy pop-ups is off)\n",
                         u.id);
        }
    }
}

void discard() {
    std::lock_guard<std::mutex> lock(sLock);
    sQueue.clear();
    sPending.store(false, std::memory_order_relaxed);
}

// DES_TROPHY_TEST=<id> (a test knob, unset by default): unlocks trophy <id> (0-127) through the HLE exactly as the
// game's sceNpTrophyUnlockTrophy would (the platinum rule, the unlock file, the hook and the pop-up), so all of it can
// be checked without earning the trophy in play. It is a real unlock in the unlock file: point PS3_TROPHY_DIR at a
// scratch folder when testing. Set it at launch, or mid-run with the debug console (setenv DES_TROPHY_TEST=12); each
// new value unlocks once. Until the game has registered its trophies it retries once a second, without a message.
void poll_test_knob() {
    static unsigned sGen = 0;
    static std::string sValue;
    static int sWant = -1;
    static clock_type::time_point sNext{};
    if (const unsigned gen = ps3_env_gen; gen != sGen) {
        sGen = gen;
        const char* v = std::getenv("DES_TROPHY_TEST");
        const std::string value = v ? v : "";
        if (value != sValue) {
            sValue = value;
            char* end = nullptr;
            const long id = value.empty() ? -1 : std::strtol(value.c_str(), &end, 10);
            sWant = (end && *end == '\0' && id >= 0 && id < 128) ? int(id) : -1;
            sNext = {};
            if (!value.empty() && sWant < 0)
                std::fprintf(stderr, "[ui] DES_TROPHY_TEST=%s: not a trophy id (0-127)\n", value.c_str());
        }
    }
    if (sWant < 0) return;
    const auto now = clock_type::now();
    if (now < sNext) return;
    const unsigned rc = unsigned(sceNpTrophy_debug_unlock(sWant));
    if (rc == kNotInitialized || rc == kContextNotRegistered) {  // the game has not registered its trophies yet
        sNext = now + std::chrono::seconds(1);
        return;
    }
    std::fprintf(stderr, "[ui] DES_TROPHY_TEST=%d: %s (0x%08X)\n", sWant, rc == 0 ? "unlocked" : "refused", rc);
    sWant = -1;
}

}  // namespace ui::trophies
