// Written for Demon's Souls. The setup screens' side of the setup engine's line protocol (docs/88 section 4; the
// engine's side is tools/setup/des_setup/protocol.py): one line of its output read into a Line, and what a run has said
// so far kept in a Status for the screens to draw. Nothing here starts a process or draws: setup_engine.hpp runs the
// engine and setup_window.hpp draws.
#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ui::setup {

// The protocol's version: the engine's first line says which it speaks ("@setup 2"). 2 added the RPCS3 route's unlock
// phase, @file, @rpcs3 and the rpcs3 error code (docs/90 3.8).
inline constexpr int kProtocol = 2;

// One line of the engine's output.
struct Line {
    enum class Kind { Text, Setup, Plan, Step, Progress, Overall, Check, File, Rpcs3, Awake, Memory, Log, Error, Done };
    Kind kind = Kind::Text;
    std::string key;                // Step, Progress, File: the step. Check, Rpcs3: the state. Error: the code.
                                    // Memory: short or ok. Done: the result.
    std::string key2;               // Step: start, done or skip. Check: the file. Error: the step.
    long long n = 0, m = 0;         // Progress, Overall: n/m. Setup: n is the version.
    std::vector<std::string> list;  // Plan: the phases
    std::string text;               // Check, Error: the sentence. File, Rpcs3, Log: the path. A plain line: all of it.
};

// One line, without its line break. A line that starts with '@' but has a keyword this host does not know, or is
// malformed, reads as a plain line.
Line parse_line(std::string_view line);

// What a run of the engine has said so far.
struct Status {
    enum class State { Todo, Running, Done, Skipped, Failed };
    struct Step {
        std::string name;
        State state = State::Todo;
    };

    int protocol = 0;
    std::vector<Step> steps;  // the plan
    std::string progress;     // the last count's step ("ppu", "download", ...), and the count
    long long progressN = 0, progressM = 0;
    std::string file;         // the file the current step works on (@file: the one RPCS3 unlocks)
    std::string rpcs3State;   // the RPCS3 in use (@rpcs3): own, shared or missing
    std::string rpcs3Path;    // and its folder
    int overall = 0;          // the whole run, per mille
    bool awake = false;       // the engine keeps the PC from sleeping
    bool memoryShort = false; // the compile runs fewer jobs than it could, for want of memory (@memory)
    std::vector<Line> checks;  // the checked files
    std::string errorCode, errorStep, errorText, logPath;
    std::string result;            // ok, fix or failed (the last line's), stopped (Cancel); empty while it runs
    std::deque<std::string> tail;  // the last plain lines, for Details

    void apply(const Line& line);
    // The engine's process has ended: `stopped` if Cancel ended it. One that ended without its last line failed.
    void ended(int exit_status, bool stopped);
    const Step* current() const;  // the running step, or null
    bool finished() const { return !result.empty(); }
};

// The time left, from how fast the whole run's bar moves: nothing for the first minute, or until enough of the bar has
// moved to judge, then smoothed, and counted down between the bar's moves.
class Eta {
public:
    void reset() { *this = Eta{}; }
    void add(int overall, unsigned long long now_ms);
    std::string text(unsigned long long now_ms) const;  // "About 25 minutes left", or empty while it cannot tell

private:
    unsigned long long mFirstMs = 0, mStartMs = 0, mLeftAt = 0;
    int mStart = -1, mLast = -1;
    double mLeftMs = -1.0;
};

}  // namespace ui::setup
