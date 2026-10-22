// The setup engine's line protocol, read (see setup_protocol.hpp; the lines are listed in protocol.py and docs/88).
#include "setup_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>

namespace ui::setup {
namespace {

// Every keyword of protocol.py's KINDS (tools/setup/tests/test_pipeline.py checks that the two lists agree).
struct KindName {
    const char* name;
    Line::Kind kind;
};
constexpr KindName kKinds[] = {
    {"setup", Line::Kind::Setup},       {"plan", Line::Kind::Plan},   {"step", Line::Kind::Step},
    {"progress", Line::Kind::Progress}, {"overall", Line::Kind::Overall}, {"check", Line::Kind::Check},
    {"awake", Line::Kind::Awake},       {"log", Line::Kind::Log},     {"error", Line::Kind::Error},
    {"done", Line::Kind::Done},
};

constexpr size_t kTailLines = 200;

// The next word of `rest` (up to a space), and `rest` moved past it and the space.
std::string_view take_word(std::string_view& rest) {
    const size_t space = rest.find(' ');
    const std::string_view word = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    return word;
}

bool parse_number(std::string_view s, long long& out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && end == s.data() + s.size();
}

// "n/m", with m > 0 and 0 <= n.
bool parse_count(std::string_view s, long long& n, long long& m) {
    const size_t slash = s.find('/');
    return slash != std::string_view::npos && parse_number(s.substr(0, slash), n) &&
           parse_number(s.substr(slash + 1), m) && m > 0 && n >= 0;
}

}  // namespace

Line parse_line(std::string_view text) {
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    Line plain;
    plain.text = std::string(text);
    if (text.size() < 2 || text[0] != '@') return plain;
    std::string_view rest = text.substr(1);
    const std::string_view keyword = take_word(rest);
    const auto* known = std::find_if(std::begin(kKinds), std::end(kKinds),
                                     [keyword](const KindName& k) { return keyword == k.name; });
    if (known == std::end(kKinds)) return plain;

    Line l;
    l.kind = known->kind;
    switch (l.kind) {
    case Line::Kind::Setup:
        if (!parse_number(take_word(rest), l.n)) return plain;
        break;
    case Line::Kind::Plan:
        while (!rest.empty()) {
            const std::string_view phase = take_word(rest);
            if (!phase.empty()) l.list.emplace_back(phase);
        }
        if (l.list.empty()) return plain;
        break;
    case Line::Kind::Step:
        l.key = take_word(rest);
        l.key2 = take_word(rest);
        if (l.key.empty() || l.key2.empty()) return plain;
        break;
    case Line::Kind::Progress:
        l.key = take_word(rest);
        if (l.key.empty() || !parse_count(take_word(rest), l.n, l.m)) return plain;
        break;
    case Line::Kind::Overall:
        if (!parse_count(take_word(rest), l.n, l.m)) return plain;
        break;
    case Line::Kind::Check:
    case Line::Kind::Error:
        l.key = take_word(rest);
        l.key2 = take_word(rest);
        l.text = std::string(rest);
        if (l.key.empty() || l.key2.empty()) return plain;
        break;
    case Line::Kind::Log:
        l.text = std::string(rest);
        if (l.text.empty()) return plain;
        break;
    case Line::Kind::Done:
        l.key = take_word(rest);
        if (l.key.empty()) return plain;
        break;
    case Line::Kind::Awake:
    case Line::Kind::Text:
        break;
    }
    return l;
}

void Status::apply(const Line& l) {
    switch (l.kind) {
    case Line::Kind::Text:
        tail.push_back(l.text);
        while (tail.size() > kTailLines) tail.pop_front();
        break;
    case Line::Kind::Setup:
        protocol = int(l.n);
        if (protocol != kProtocol) {  // (setup_engine.cpp then stops it)
            errorCode = "internal";
            errorText = "The setup files do not match this version of the game.";
            result = "failed";
        }
        break;
    case Line::Kind::Plan:
        steps.clear();
        for (const std::string& name : l.list) steps.push_back({name, State::Todo});
        break;
    case Line::Kind::Step:
        for (Step& s : steps) {
            if (s.name != l.key) continue;
            if (l.key2 == "start") s.state = State::Running;
            else if (l.key2 == "done") s.state = State::Done;
            else if (l.key2 == "skip") s.state = State::Skipped;
        }
        if (l.key2 == "start") {
            progress.clear();
            progressN = progressM = 0;
        }
        break;
    case Line::Kind::Progress:
        progress = l.key;
        progressN = l.n;
        progressM = l.m;
        break;
    case Line::Kind::Overall:
        overall = int(std::clamp(l.n * 1000 / l.m, 0LL, 1000LL));
        break;
    case Line::Kind::Check:
        checks.push_back(l);
        break;
    case Line::Kind::Awake:
        awake = true;
        break;
    case Line::Kind::Log:
        logPath = l.text;
        break;
    case Line::Kind::Error:
        errorCode = l.key;
        errorStep = l.key2;
        errorText = l.text;
        for (Step& s : steps)
            if (s.state == State::Running) s.state = State::Failed;
        break;
    case Line::Kind::Done:
        if (result.empty()) result = l.key;
        if (result == "ok") overall = 1000;
        break;
    }
}

void Status::ended(int exit_status, bool stopped) {
    if (!result.empty()) return;
    for (Step& s : steps)
        if (s.state == State::Running) s.state = stopped ? State::Todo : State::Failed;
    if (stopped) {
        result = "stopped";
        return;
    }
    result = "failed";
    if (errorCode.empty()) {
        errorCode = "internal";
        errorStep = "-";
        errorText = "Setup stopped unexpectedly.";
    }
    tail.push_back("(setup ended with exit status " + std::to_string(exit_status) + ")");
}

const Status::Step* Status::current() const {
    for (const Step& s : steps)
        if (s.state == State::Running) return &s;
    return nullptr;
}

void Eta::add(int overall, unsigned long long now) {
    if (mFirstMs == 0) mFirstMs = now ? now : 1;
    // For the first 10 s the starting point moves with the bar: the phases a resumed run skips fill it at once.
    if (mStart < 0 || now - mFirstMs < 10000) {
        mStart = mLast = overall;
        mStartMs = mLeftAt = now;
        return;
    }
    if (overall <= mLast) return;
    mLast = overall;
    const double done = double(overall - mStart), elapsed = double(now - mStartMs);
    if (elapsed < 60000.0 || done < 10.0) return;  // too early to tell
    const double left = elapsed / done * double(1000 - overall);
    mLeftMs = mLeftMs < 0 ? left : mLeftMs * 0.7 + left * 0.3;
    mLeftAt = now;
}

std::string Eta::text(unsigned long long now) const {
    if (mLeftMs < 0) return {};
    const double left = std::max(0.0, mLeftMs - double(now - mLeftAt));
    if (left < 45000.0) return "Less than a minute left";
    const long long minutes = (long long)((left + 30000.0) / 60000.0);
    if (minutes <= 1) return "About a minute left";
    if (minutes < 60) return "About " + std::to_string(minutes) + " minutes left";
    const long long rounded = (minutes + 5) / 10 * 10, hours = rounded / 60, rest = rounded % 60;
    std::string out = "About " + std::to_string(hours) + (hours == 1 ? " hour" : " hours");
    if (rest) out += " " + std::to_string(rest) + " minutes";
    return out + " left";
}

}  // namespace ui::setup
