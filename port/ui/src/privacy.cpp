// Written for Demon's Souls. See privacy.hpp.
#include "privacy.hpp"

#include "settings.hpp"
#include "ui_assets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>

// ps3emu/ps3_net.h (declared here: des_ui does not take ps3recomp's include paths).
extern "C" void ps3_net_lock_offline(void);

namespace ui::privacy {
namespace {

constexpr const char* kNotePath = "../../PRIVACY.md";  // (embedded from the top of the repository: port/ui/CMakeLists.txt)

struct Note {
    std::vector<Part> parts;
    int version = 1;
};

// ** and ` taken out: the page shows the words.
std::string plain(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') continue;
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

// The note's Markdown as parts: "# " is the title (the page has its own), "Version N" the version, "## " a heading,
// "- " a list item; a line that follows another (wrapped, or indented under an item) joins it; a blank line ends it.
Note parse(std::string_view text) {
    Note note;
    Part* open = nullptr;
    bool versionSeen = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const size_t indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos) {
            open = nullptr;
            continue;
        }
        const std::string_view body = line.substr(indent);
        if (body[0] == '#') {
            open = nullptr;
            const size_t level = body.find_first_not_of('#');
            if (level == 1 || level == std::string_view::npos) continue;  // (the title)
            const size_t start = body.find_first_not_of(' ', level);
            if (start != std::string_view::npos)
                note.parts.push_back({Part::Kind::Heading, plain(body.substr(start))});
        } else if (body.size() > 2 && body[0] == '-' && body[1] == ' ') {
            note.parts.push_back({Part::Kind::Item, plain(body.substr(2))});
            open = &note.parts.back();
        } else if (open) {
            open->text += ' ';
            open->text += plain(body);
        } else if (!versionSeen && body.substr(0, 8) == "Version ") {
            versionSeen = true;
            note.version = std::max(1, std::atoi(std::string(body.substr(8)).c_str()));
        } else {
            note.parts.push_back({Part::Kind::Text, plain(body)});
            open = &note.parts.back();
        }
    }
    return note;
}

const Note& note() {
    static const Note n = [] {
        const Asset* a = find_asset(kNotePath);
        if (!a) {
            std::fprintf(stderr, "[ui] the privacy note is not in this build\n");
            return Note{};
        }
        return parse(std::string_view(reinterpret_cast<const char*>(a->data), a->size));
    }();
    return n;
}

std::atomic<bool> sNeeded{false};
std::atomic<bool> sAsking{false};
std::atomic<bool> sAnsweredFlag{false};  // (sAnswered, readable without the lock)
std::mutex sLock;
std::condition_variable sAnswer;
bool sAnswered = false;  // (sLock)

}  // namespace

const std::vector<Part>& parts() {
    return note().parts;
}

int version() {
    return note().version;
}

void set_needed(bool needed) {
    sNeeded.store(needed);
}

bool unanswered() {
    return sNeeded.load() && !sAnsweredFlag.load();
}

bool asking() {
    return sAsking.load();
}

void begin_asking() {
    sAsking.store(true);
}

void wait_answer(void (*wake)()) {
    std::unique_lock<std::mutex> lock(sLock);
    while (!sAnswered) {
        if (wake) {
            lock.unlock();
            wake();
            lock.lock();
        }
        sAnswer.wait_for(lock, std::chrono::milliseconds(250), [] { return sAnswered; });
    }
}

void answer(bool accept) {
    static std::atomic<bool> sOnce{false};
    if (sOnce.exchange(true)) return;
    // The answer's effects first: the start goes on (wait_answer) only once the run is offline after a no.
    if (accept) {
        settings::set("privacy_accepted", std::to_string(version()));
        std::fprintf(stderr, "[ui] privacy note version %d accepted\n", version());
    } else {
        ps3_net_lock_offline();
        settings::net_off("Off: the privacy note was declined");
        std::fprintf(stderr, "[ui] privacy note version %d declined: this run is offline\n", version());
    }
    {
        std::lock_guard<std::mutex> lock(sLock);
        sAnswered = true;
        sAnsweredFlag.store(true);
    }
    sAsking.store(false);
    sAnswer.notify_all();
}

}  // namespace ui::privacy
