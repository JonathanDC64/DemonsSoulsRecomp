/* DES_ROUTE: a fixed walk through a level, played inside the game on its own
 * clock, with named screenshots and a frame-time summary. The test kit's runs
 * (testers on Linux and macOS, with no Python and no debug console) and
 * tools/des/regress.sh use it.
 *
 * DES_ROUTE=regress plays regress.sh's route (regress_text below), which walks
 * from the first level's spawn point. DES_ROUTE=look stays where the save puts
 * the player and turns the camera, so it is safe anywhere (the test kit's,
 * tools/testkit). DES_ROUTE=<file> plays a route file. DES_ROUTE_OUT=<folder>
 * (default des_route) gets the shots as <name>.bmp, route.txt (the timeline)
 * and summary.json.
 * DES_ROUTE_QUIT=1 ends the game when the route is done; the verdict is in
 * summary.json ("result": ok, missing or no_level) and route.txt's last line.
 *
 * A route file has one step a line ('#' starts a comment):
 *   anchor                 wait for the level: the opening cutscene's end (no
 *                          cutscene for 2 s after it), or, with no cutscene,
 *                          the player present for 10 s; the anchor is the
 *                          first frame after the cutscene, or the player's
 *                          first frame
 *   wait <s>               the next step comes s game seconds later
 *   shot <name>            save the next frame with draws as <name>.bmp
 *   hold <s> lx ly rx ry   hold the sticks (0-255, 128 centred; ly 0 is
 *                          forward) for s game seconds
 *   set KNOB=value         set a knob live (an empty value unsets it)
 *   press <s> <b>[+<b>..]  hold buttons on port 0 for s game seconds: start,
 *                          select, cross, circle, triangle, square, up, down,
 *                          left, right, l1, l2, r1, r2 (press 0.15 r2+cross)
 *   continue               after a Quit Game: once the level is gone, press
 *                          through the title into the last save
 *                          (DES_AUTO_CONTINUE's presses), wait for the next
 *                          level to load and anchor again as `anchor` does;
 *                          the steps after it time from the new anchor
 * DES_ROUTE=loads plays loads_text below: DES_ROUTE_LOADS (default 5) times
 * Quit Game and continue, with a shot after each load (load0 before the
 * first). Its route.txt names each load; summary.json counts them ("loads").
 * Every time is game time: the sum of the steps the game was handed, 1/30 s a
 * frame at the original 30 fps and the frame's own time under DES_FPS. So a
 * step is the same game moment at any frame rate and on a slow PC. Each step
 * comes at its planned time from the anchor; a hold does not move the steps
 * after it, and a step waits only for a shot still being saved before it.
 *
 * A hold is frame-exact: the stick is set at one flip request and let go at
 * the one where the frames after it have been handed the hold's time, less
 * half a frame step (so at 30 fps an 8 s hold is 240 frames). regress.sh's
 * Python route did the same with wall-clock windows it corrected while they
 * ran (regress_route.py). route.txt says how long the game read the left
 * stick in each hold, from cellPad's last read.
 *
 * summary.json covers the frames from the moment the anchor is known (2 or
 * 10 game seconds after it) to the route's end: their
 * count, the mean frame rate, the 1% and 0.1% lows (from the 99th and 99.9th
 * percentile frame times), the hitches (frames over 2.5 times the median
 * time) and game time over real time. */

#include "ps3emu/ps3_knob.h"   /* ps3_setenv: through the runtime's getenv cache */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" volatile unsigned g_pad0_lstick;    /* cellPad.c: the left stick of port 0's last read, (x << 8) | y */
extern "C" void ps3_exit_now(const char* tag); /* ppu_hle.cpp: flush and end the process */
extern "C" float des_frame_step(void);         /* des_options.cpp: the step this frame was handed, s */
extern "C" int des_cutscene_playing(void);     /* des_options.cpp */
extern "C" int des_player_xyz(float* x, float* y, float* z);   /* des_options.cpp: 0 with no player */
extern "C" void des_auto_continue_again(void); /* des_options.cpp: DES_AUTO_CONTINUE's presses, once more */

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

/* regress.sh's route from the spawn point (its timings: regress.sh's
 * header). DES_ROUTE_SETTLE, _WALK, _TURN and _ON change the four times;
 * DES_ROUTE_AB=KNOB=value adds s5_ab (the knob set) and s6_back (cleared). */
constexpr double kGap = 0.80;    /* game s from a shot to the next hold */
constexpr double kStop = 2.30;   /* game s from the end of a hold to its shot */

constexpr double kNoCut = 10.0;      /* game s with the player present and no cutscene: anchor there */
constexpr double kCutDone = 2.0;     /* game s with no cutscene after one: it has ended */
constexpr double kShotWait = 20.0;   /* wall s for a shot's file */
constexpr unsigned kGoneFrames = 30; /* frames in a row with no player: a Quit Game's level has gone */

enum class Op { Anchor, Wait, Shot, Hold, Set, Press, Continue };

struct Step {
    Op op;
    double secs = 0;
    std::string text;            /* the shot's name, KNOB=value, or a press's buttons */
    unsigned stick[4] = {128, 128, 128, 128};
    unsigned mask = 0;           /* a press's buttons, CELL_PAD_CTRL_* as (DIGITAL2 << 8) | DIGITAL1 */
    int line = 0;
};

/* A press step's button names. */
unsigned button_bit(const std::string& b)
{
    static const struct { const char* name; unsigned bit; } kButtons[] = {
        {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040},
        {"left", 0x0080}, {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800},
        {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000},
    };
    for (const auto& k : kButtons)
        if (b == k.name) return k.bit;
    return 0;
}

/* What a press step holds, for the pad (des_route_buttons, on cellPad's
 * thread), and the level script a continue step waits for (des_route_on_open,
 * on the file system's). */
std::atomic<unsigned> s_buttons{0};
std::atomic<bool> s_level_opened{false};

struct Hold {
    bool on = false;
    double want = 0, got = 0, from_g = 0;
    unsigned frames = 0, read_frames = 0, want_ls = 0;
    double read_g = 0;
    std::string spec;
};

struct Shot {
    bool on = false;
    std::string name;
    fs::path dir;
    Clock::time_point asked;
    double g = 0;
};

struct Press {
    bool on = false;
    double want = 0, got = 0;
    unsigned frames = 0;
    std::string spec;
};

/* A continue step in flight: the level going (no player for kGoneFrames
 * flips), then the title's presses until the next level's script opens, then
 * the anchor's own wait. */
enum class Cont { Off, Going, Loading, Anchoring };

struct Route {
    std::vector<Step> steps;
    size_t next = 0;
    bool done = false, quit = false;
    fs::path out;
    FILE* log = nullptr;
    Clock::time_point t0, last_flip;
    double g = 0;                  /* game time so far */
    double wait_wall = 600.0;      /* wall s for the anchor */
    /* the anchor */
    bool anchored = false;
    double anchor_g = 0, at = 0;   /* at: the planned game time of the next step */
    bool cut_seen = false, pos_seen = false, prev_cut = false;
    double cut_last_g = 0, cut_end_g = 0, pos_first_g = 0;   /* the last cutscene frame's, the first after it */
    /* steps in flight */
    Hold hold;
    Shot shot;
    Press press;
    Cont cont = Cont::Off;
    unsigned gone = 0;               /* flips in a row with no player, while the level goes */
    Clock::time_point cont_t0;       /* when the continue step began */
    int loads = 0;                   /* continue steps done */
    double g_on = 0;                 /* game time on the frames counted in iv */
    int shots = 0, missing = 0;
    std::string dump_dir_was;
    bool dump_dir_set = false;
    /* frame times from the anchor, ms */
    std::vector<float> iv;
    double route_wall = 0, known_g = 0;   /* known_g: the game time when the anchor was found */
};

Route* s_route = nullptr;

void say(Route& r, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const double t = std::chrono::duration<double>(Clock::now() - r.t0).count();
    fprintf(stderr, "[route] %s\n", buf);
    if (r.log) {
        fprintf(r.log, "t=%.1fs %s\n", t, buf);
        fflush(r.log);
    }
}

std::string g_str(const Route& r, double g)
{
    char b[96];
    const double d = g - r.anchor_g;
    snprintf(b, sizeof b, "game %.2f s (anchor %+.2f s, %ld frames at 30 fps)", g, d, std::lround(d * 30));
    return b;
}

double env_secs(const char* name, double def)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    char* end = nullptr;
    const double v = strtod(e, &end);
    return end != e && v >= 0 ? v : def;
}

std::string regress_text()
{
    char b[1024];
    snprintf(b, sizeof b,
             "anchor\nwait %.3f\nshot s1_spawn\n"
             "wait %.3f\nhold %.3f 128 0 128 128\nwait %.3f\nshot s2_walk\n"
             "wait %.3f\nhold %.3f 128 128 255 128\nwait %.3f\nshot s3_turn\n"
             "wait %.3f\nhold %.3f 128 0 128 128\nwait %.3f\nshot s4_on\n",
             env_secs("DES_ROUTE_SETTLE", 35.9), kGap, env_secs("DES_ROUTE_WALK", 8.0), kStop,
             kGap, env_secs("DES_ROUTE_TURN", 1.5), kStop, kGap, env_secs("DES_ROUTE_ON", 5.0), kStop);
    std::string t = b;
    const char* ab = getenv("DES_ROUTE_AB");
    if (ab && *ab && strchr(ab, '=')) {
        const std::string kv = ab, k = kv.substr(0, kv.find('='));
        snprintf(b, sizeof b, "wait %.3f\nset %s\nwait %.3f\nshot s5_ab\nset %s=\nwait %.3f\nshot s6_back\n",
                 kGap, kv.c_str(), kStop, k.c_str(), kStop);
        t += b;
    }
    return t;
}

/* The test kit's route: no walking, so it suits any save. A shot, three turns
 * of the camera with a shot after each, then a pan for frame times. The camera
 * starts a turn slowly: from rest, 3 s at full stick turns it about 15 degrees,
 * 9 s about 150. A stick at 160 is inside the game's dead zone; 192 turns. */
std::string look_text()
{
    char b[512];
    snprintf(b, sizeof b,
             "anchor\nwait 15\nshot look1\n"
             "wait %.3f\nhold 5 128 128 255 128\nwait %.3f\nshot look2\n"
             "wait %.3f\nhold 5 128 128 255 128\nwait %.3f\nshot look3\n"
             "wait %.3f\nhold 5 128 128 255 128\nwait %.3f\nshot look4\n"
             "wait %.3f\nhold 30 128 128 200 128\nwait %.3f\nshot look5\n",
             kGap, kStop, kGap, kStop, kGap, kStop, kGap, kStop);
    return b;
}

/* DES_ROUTE=loads: Quit Game from the pause menu, then continue, n times
 * (DES_ROUTE_LOADS, default 5), with a shot of each level. Start opens the
 * menu's bar on Inventory, and L2 turns it to System, the last tab. Cross
 * opens System's rows (Options, Load Profile, Quit Game): two downs to Quit
 * Game, Cross. Its question, "Quit game & return to Title Screen?", shows OK
 * left of CANCEL with CANCEL chosen: left, then Cross. */
std::string loads_text()
{
    const int n = std::max(1, std::min(50, ps3_env_int("DES_ROUTE_LOADS", 5)));
    std::string t = "anchor\nwait 8\nshot load0\n";
    char b[512];
    for (int i = 1; i <= n; i++) {
        snprintf(b, sizeof b,
                 "press 0.15 start\nwait 1.5\npress 0.15 l2\nwait 1.0\npress 0.15 cross\nwait 1.0\n"
                 "press 0.15 down\nwait 0.6\npress 0.15 down\nwait 0.6\npress 0.15 cross\nwait 1.0\n"
                 "press 0.15 left\nwait 0.6\npress 0.15 cross\nwait 1\n"
                 "continue\nwait 8\nshot load%d\n", i);
        t += b;
    }
    return t;
}

/* The steps, or an empty list and a message when a line does not parse. */
bool parse(const std::string& text, std::vector<Step>& steps, std::string& err)
{
    std::istringstream in(text);
    std::string ln;
    int n = 0;
    while (std::getline(in, ln)) {
        n++;
        if (const size_t h = ln.find('#'); h != std::string::npos) ln.resize(h);
        std::istringstream w(ln);
        std::string op;
        if (!(w >> op)) continue;
        Step s;
        s.line = n;
        bool ok = true;
        if (op == "anchor") {
            s.op = Op::Anchor;
        } else if (op == "wait") {
            s.op = Op::Wait;
            ok = static_cast<bool>(w >> s.secs) && s.secs >= 0;
        } else if (op == "shot") {
            s.op = Op::Shot;
            ok = static_cast<bool>(w >> s.text) && s.text.find_first_of("/\\:") == std::string::npos;
        } else if (op == "hold") {
            s.op = Op::Hold;
            ok = static_cast<bool>(w >> s.secs >> s.stick[0] >> s.stick[1] >> s.stick[2] >> s.stick[3]) &&
                 s.secs > 0 && *std::max_element(s.stick, s.stick + 4) <= 255;
        } else if (op == "set") {
            s.op = Op::Set;
            ok = static_cast<bool>(w >> s.text) && s.text.find('=') != std::string::npos && s.text[0] != '=';
        } else if (op == "press") {
            s.op = Op::Press;
            ok = static_cast<bool>(w >> s.secs >> s.text) && s.secs > 0;
            for (size_t at = 0; ok && at <= s.text.size();) {
                const size_t plus = std::min(s.text.find('+', at), s.text.size());
                const unsigned bit = button_bit(s.text.substr(at, plus - at));
                ok = bit != 0;
                s.mask |= bit;
                at = plus + 1;
            }
        } else if (op == "continue") {
            s.op = Op::Continue;
        } else {
            ok = false;
        }
        if (!ok) {
            err = "line " + std::to_string(n) + ": " + ln;
            return false;
        }
        steps.push_back(s);
    }
    if (steps.empty()) err = "no steps";
    return !steps.empty();
}

bool bmp_complete(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    unsigned char h[6] = {};
    if (!f.read(reinterpret_cast<char*>(h), 6) || h[0] != 'B' || h[1] != 'M') return false;
    const uint32_t size = h[2] | (h[3] << 8) | (h[4] << 16) | (uint32_t(h[5]) << 24);
    std::error_code ec;
    const auto have = fs::file_size(p, ec);
    return !ec && have >= size;
}

void dump_dir(Route& r, const char* dir)
{
    if (!r.dump_dir_set) {
        const char* was = getenv("CELLMARK_DUMP_DIR");
        r.dump_dir_was = was ? was : "";
        r.dump_dir_set = true;
    }
    ps3_setenv("CELLMARK_DUMP_DIR", dir ? dir : r.dump_dir_was.c_str());
}

void shot_ask(Route& r, const std::string& name)
{
    std::error_code ec;
    r.shot.dir = r.out / (".shot_" + name);
    fs::remove_all(r.shot.dir, ec);
    fs::create_directories(r.shot.dir, ec);
    dump_dir(r, r.shot.dir.generic_string().c_str());
    ps3_setenv("CELLMARK_DUMP_NOW", "1");
    r.shot.on = true;
    r.shot.name = name;
    r.shot.asked = Clock::now();
    r.shot.g = r.g;
}

/* At each flip while a shot is out: its file, once the renderer has written
 * all of it. */
void shot_poll(Route& r)
{
    std::error_code ec;
    fs::path got;
    for (const auto& e : fs::directory_iterator(r.shot.dir, ec))
        if (e.path().extension() == ".bmp" && bmp_complete(e.path())) {
            got = e.path();
            break;
        }
    const bool late = std::chrono::duration<double>(Clock::now() - r.shot.asked).count() > kShotWait;
    if (got.empty() && !late) return;
    ps3_setenv("CELLMARK_DUMP_NOW", "");   /* the next shot's 1 is a change again */
    dump_dir(r, nullptr);
    if (!got.empty()) {
        const fs::path to = r.out / (r.shot.name + ".bmp");
        fs::remove(to, ec);
        fs::rename(got, to, ec);
        if (ec) fs::copy_file(got, to, fs::copy_options::overwrite_existing, ec);
    }
    if (!got.empty() && !ec) {
        r.shots++;
        say(r, "%s at %s", r.shot.name.c_str(), g_str(r, r.shot.g).c_str());
    } else {
        r.missing++;
        say(r, "%s: no frame saved within %.0f s", r.shot.name.c_str(), kShotWait);
    }
    fs::remove_all(r.shot.dir, ec);
    r.shot.on = false;
}

void hold_end(Route& r)
{
    ps3_setenv("PAD_STICK", "");
    Hold& h = r.hold;
    char rd[96] = "";
    if (h.want_ls != 0x8080u)
        snprintf(rd, sizeof rd, ", left stick read for %.2f s (%u frames)", h.read_g, h.read_frames);
    say(r, "hold %s: %.2f s wanted, %.2f s held (%u frames) from %s%s", h.spec.c_str(), h.want, h.got, h.frames,
        g_str(r, h.from_g).c_str(), rd);
    h.on = false;
}

/* At each flip while a hold is on: this frame's step counted, and the hold let
 * go once the frames since it began have been handed its time, less half a
 * step (it then ends between two frames). */
void hold_tick(Route& r, double step)
{
    Hold& h = r.hold;
    h.got += step;
    h.frames++;
    if (g_pad0_lstick == h.want_ls) {
        h.read_frames++;
        h.read_g += step;
    }
    if (h.got >= h.want - step / 2) hold_end(r);
}

void press_end(Route& r)
{
    s_buttons.store(0, std::memory_order_relaxed);
    say(r, "press %s: %.2f s wanted, %.2f s held (%u frames)", r.press.spec.c_str(), r.press.want, r.press.got,
        r.press.frames);
    r.press.on = false;
}

/* At each flip while a press is on: as hold_tick. */
void press_tick(Route& r, double step)
{
    r.press.got += step;
    r.press.frames++;
    if (r.press.got >= r.press.want - step / 2) press_end(r);
}

void summary(Route& r, const char* result)
{
    std::vector<float> v = r.iv;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v.empty() ? 0.0 : (double)v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))]; };
    const double med = pct(0.5), p99 = pct(0.99), p999 = pct(0.999);
    unsigned hitches = 0;
    float worst = 0;
    for (float x : r.iv) {
        if (x > 2.5 * med) hitches++;
        worst = std::max(worst, x);
    }
    const double wall = r.route_wall, game = r.g_on;
    FILE* f = fopen((r.out / "summary.json").string().c_str(), "w");
    if (!f) return;
    fprintf(f, "{\n  \"result\": \"%s\",\n  \"shots\": %d,\n  \"missing\": %d,\n  \"loads\": %d,\n", result, r.shots,
            r.missing, r.loads);
    fprintf(f, "  \"frames\": %zu,\n  \"seconds\": %.3f,\n  \"fps\": %.2f,\n", r.iv.size(), wall,
            wall > 0 ? r.iv.size() / wall : 0.0);
    fprintf(f, "  \"median_ms\": %.3f,\n  \"low_1pct_fps\": %.2f,\n  \"low_01pct_fps\": %.2f,\n", med,
            p99 > 0 ? 1000.0 / p99 : 0.0, p999 > 0 ? 1000.0 / p999 : 0.0);
    fprintf(f, "  \"hitches\": %u,\n  \"worst_ms\": %.3f,\n  \"game_over_real\": %.4f\n}\n", hitches, worst,
            wall > 0 ? game / wall : 0.0);
    fclose(f);
}

void finish(Route& r, const char* result)
{
    if (r.hold.on) hold_end(r);
    if (r.press.on) press_end(r);
    r.done = true;
    summary(r, result);
    say(r, "done: %s, %d shot(s) saved, %d missing", result, r.shots, r.missing);
    if (r.log) fclose(r.log);
    r.log = nullptr;
    if (r.quit) ps3_exit_now("DES_ROUTE");
}

/* Before the anchor: the opening cutscene's end, or the player's arrival
 * when none plays (regress_route.py's rule). */
void anchor_tick(Route& r)
{
    const bool cut = des_cutscene_playing() != 0;
    float x, y, z;
    const bool pos = des_player_xyz(&x, &y, &z) != 0;
    if (cut) {
        r.cut_seen = r.prev_cut = true;
        r.cut_last_g = r.g;
        return;
    }
    if (r.prev_cut) {   /* the first frame after a cutscene */
        r.prev_cut = false;
        r.cut_end_g = r.g;
    }
    if (pos && !r.pos_seen && !r.cut_seen) {
        r.pos_seen = true;
        r.pos_first_g = r.g;
    }
    if (r.cut_seen && r.g - r.cut_last_g >= kCutDone) {
        r.anchor_g = r.cut_end_g;
        say(r, "anchor: the opening cutscene ended, game %.2f s", r.anchor_g);
    } else if (!r.cut_seen && r.pos_seen && r.g - r.pos_first_g >= kNoCut) {
        r.anchor_g = r.pos_first_g;
        say(r, "anchor: the player appeared (no cutscene), game %.2f s", r.anchor_g);
    } else {
        return;
    }
    r.anchored = true;
    r.at = r.anchor_g;
    r.known_g = r.g;
    r.last_flip = Clock::now();
    if (pos) say(r, "  the player at %.2f %.2f %.2f", x, y, z);
}

/* At each flip of a continue step; true once the next level is anchored. */
bool continue_tick(Route& r)
{
    float x, y, z;
    const bool pos = des_player_xyz(&x, &y, &z) != 0;
    switch (r.cont) {
    case Cont::Off:
        r.cont = Cont::Going;
        r.cont_t0 = Clock::now();
        r.gone = 0;
        r.anchored = false;   /* the title and the load are not counted in the frame times */
        say(r, "continue: waiting for the level to go at %s", g_str(r, r.g).c_str());
        return false;
    case Cont::Going:
        r.gone = pos ? 0 : r.gone + 1;
        if (r.gone < kGoneFrames) break;
        s_level_opened.store(false, std::memory_order_relaxed);
        des_auto_continue_again();
        r.cont = Cont::Loading;
        say(r, "continue: the level has gone; pressing through the title");
        break;
    case Cont::Loading:
        if (!s_level_opened.load(std::memory_order_relaxed)) break;
        r.cont = Cont::Anchoring;
        r.cut_seen = r.pos_seen = r.prev_cut = false;
        say(r, "continue: the next level is loading");
        break;
    case Cont::Anchoring:
        anchor_tick(r);
        if (!r.anchored) break;
        r.cont = Cont::Off;
        r.loads++;
        say(r, "load %d: anchored after %.1f s", r.loads,
            std::chrono::duration<double>(Clock::now() - r.cont_t0).count());
        return true;
    }
    if (std::chrono::duration<double>(Clock::now() - r.cont_t0).count() > r.wait_wall) {
        say(r, "continue: no level after %.0f s", r.wait_wall);
        finish(r, "no_level");
    }
    return false;
}

}  // namespace

extern "C" void des_route_init(void)
{
    const char* e = getenv("DES_ROUTE");
    if (!e || !*e) return;
    Route* r = new Route;
    r->t0 = Clock::now();
    std::string text, err;
    if (!strcmp(e, "regress")) {
        text = regress_text();
    } else if (!strcmp(e, "look")) {
        text = look_text();
    } else if (!strcmp(e, "loads")) {
        text = loads_text();
    } else {
        std::ifstream f{fs::path(e)};   /* (UTF-8: des_utf8.manifest on Windows) */
        if (!f) {
            fprintf(stderr, "[route] DES_ROUTE: cannot read %s\n", e);
            delete r;
            return;
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!parse(text, r->steps, err)) {
        fprintf(stderr, "[route] DES_ROUTE=%s: %s\n", e, err.c_str());
        delete r;
        return;
    }
    const char* out = getenv("DES_ROUTE_OUT");
    r->out = fs::path(out && *out ? out : "des_route");
    std::error_code ec;
    fs::create_directories(r->out, ec);
    r->log = fopen((r->out / "route.txt").string().c_str(), "w");
    if (FILE* f = fopen((r->out / "route_steps.txt").string().c_str(), "w")) {
        fputs(text.c_str(), f);
        fclose(f);
    }
    r->quit = ps3_env_int("DES_ROUTE_QUIT", 0) != 0;
    r->wait_wall = env_secs("DES_ROUTE_WAIT", 600.0);
    if (!getenv("CELLMARK_DUMP_MINDRAWS")) ps3_setenv("CELLMARK_DUMP_MINDRAWS", "1");   /* never an empty present */
    say(*r, "DES_ROUTE=%s: %zu steps, into %s%s", e, r->steps.size(), r->out.generic_string().c_str(),
        r->quit ? "; the game ends after it" : "");
    s_route = r;
}

/* At each flip request (des_options.cpp's on_flip_request, after the frame
 * log): the game clock moved on by this frame's step. */
extern "C" void des_route_on_flip(void)
{
    Route* const rp = s_route;
    if (!rp || rp->done) return;
    Route& r = *rp;
    const double step = des_frame_step();
    const double counted = step > 0 && step < 1 ? step : 0;
    r.g += counted;
    if (r.shot.on) shot_poll(r);
    if (r.hold.on) hold_tick(r, step);
    if (r.press.on) press_tick(r, step);
    if (r.anchored) {
        const Clock::time_point now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - r.last_flip).count();
        r.last_flip = now;
        r.iv.push_back((float)ms);
        r.route_wall += ms / 1000.0;
        r.g_on += counted;
    }
    while (r.next < r.steps.size()) {
        const Step& s = r.steps[r.next];
        if (s.op == Op::Anchor) {
            if (!r.anchored) {
                anchor_tick(r);
                if (!r.anchored) {
                    if (std::chrono::duration<double>(Clock::now() - r.t0).count() > r.wait_wall) {
                        say(r, "no level after %.0f s", r.wait_wall);
                        finish(r, "no_level");
                    }
                    return;
                }
            }
            r.next++;
            continue;
        }
        if (s.op == Op::Wait) {
            r.at += s.secs;
            r.next++;
            continue;
        }
        if (s.op == Op::Continue) {   /* at its time, with nothing in flight; then until the next anchor */
            if (r.cont == Cont::Off && (r.g + 1e-9 < r.at || r.shot.on || r.hold.on || r.press.on)) return;
            if (!continue_tick(r)) return;
            r.next++;
            continue;
        }
        if (r.g + 1e-9 < r.at || r.shot.on) return;   /* not yet, or a shot is still being saved */
        switch (s.op) {
        case Op::Shot:
            shot_ask(r, s.text);
            break;
        case Op::Hold: {
            if (r.hold.on) hold_end(r);
            char spec[64];
            snprintf(spec, sizeof spec, "%u,%u,%u,%u", s.stick[0], s.stick[1], s.stick[2], s.stick[3]);
            ps3_setenv("PAD_STICK", spec);
            r.hold = Hold{};
            r.hold.on = true;
            r.hold.want = s.secs;
            r.hold.from_g = r.g;
            r.hold.want_ls = (s.stick[0] << 8) | s.stick[1];
            r.hold.spec = spec;
            r.at += s.secs;
            break;
        }
        case Op::Set: {
            const size_t eq = s.text.find('=');
            ps3_setenv(s.text.substr(0, eq).c_str(), s.text.c_str() + eq + 1);
            say(r, "set %s at %s", s.text.c_str(), g_str(r, r.g).c_str());
            break;
        }
        case Op::Press:
            if (r.press.on) press_end(r);
            r.press = Press{};
            r.press.on = true;
            r.press.want = s.secs;
            r.press.spec = s.text;
            s_buttons.store(s.mask, std::memory_order_relaxed);
            r.at += s.secs;
            break;
        default:
            break;
        }
        r.next++;
    }
    if (r.next >= r.steps.size() && !r.shot.on && !r.hold.on && !r.press.on)
        finish(r, r.missing ? "missing" : "ok");
}

/* For the pad (des_options.cpp's title_buttons, on every poll): what a press
 * step holds down, 0 with none. */
extern "C" unsigned des_route_buttons(void)
{
    return s_buttons.load(std::memory_order_relaxed);
}

/* For a continue step (des_options.cpp's g_fs_open_hook): a level's script
 * opening, as DES_AUTO_CONTINUE's end. */
extern "C" void des_route_on_open(const char* path)
{
    if (s_route && strstr(path, "/script/m") && strstr(path, ".luabnd"))
        s_level_opened.store(true, std::memory_order_relaxed);
}
