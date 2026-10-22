// The port's settings (see settings.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's settings.ini handling.
#include "settings.hpp"

#include "report.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache

namespace ui::settings {
namespace {

constexpr Def kDefs[] = {
#if DES_DEV
    {"DES_GOD", "0", Apply::Session},          // des_dev.cpp (developer builds only): the cheats, live, never saved
    {"DES_ATK", "1", Apply::Session},
    {"DES_SPEED", "1", Apply::Session},
#endif
    {"DES_FULLSCREEN", "0", Apply::Live},      // 1 = borderless fullscreen; des_ui.cpp's sync_fullscreen, each frame
    {"DES_FPS", "", Apply::Live},              // "" = the game's own 30 fps mode; read at each flip
    {"fps_custom", "90", Apply::UiOnly},       // Frame rate's Custom number, kept while a fixed rate is chosen
    {"DES_CUTFPS", "0", Apply::Live},          // des_options.cpp's cutfps_on, read at each flip
    {"DES_MOTION_BLUR", "1", Apply::Live},     // des_motion_blur, read each frame
    {"DES_VSYNC", "1", Apply::Live},           // RSX_VSYNC, live
    {"DES_SKIP_INTRO", "0", Apply::Live},      // 1 = no logos, 2 = the last logo only; read at each logo update
    {"DES_SKIP_INTRO_MOVIE", "0", Apply::Live},  // PS3_SAIL_SKIP, read as each movie opens
    {"DES_AUTO_CONTINUE", "0", Apply::Live},   // read at each pad poll, until the first level loads
    {"AUDIO_MUTE", "0", Apply::Live},
    {"DES_DOF", "1", Apply::Live},             // des_dof, read each frame
    {"RSX_AA", "0", Apply::Live},              // 0 = none, as on the PS3; 1 = FXAA before the HUD; per batch
    {"RSX_SCALE", "100", Apply::Live},         // the D3D12 backend remakes its render targets at the next batch
    {"RSX_OUTFILTER", "0", Apply::Live},       // 0 = bilinear, as before; 1 = area down, bicubic up; next present
    {"RSX_TEXFILTER", "0", Apply::Live},       // 0 = the game's filters; the backend's sampler key, per draw
    {"RSX_ANISO", "0", Apply::Live},           // 0 = the game's own level
    {"DES_ASPECT", "", Apply::Live},           // "" = 16:9; des_aspect.cpp, at start and at the next flip after a change
    {"DES_HUD", "", Apply::Live},              // "" = centred at 16:9, "stretch"; des_aspect.cpp, as DES_ASPECT
    // Online play (apply_network): the runtime registers its sockets and NP at boot, so all of these but the password
    // need a restart.
    {"DES_ONLINE", "1", Apply::Restart},       // "1" = online (the default), "0" = offline
    {"DES_NET_SERVER", kNetServers[0].key, Apply::Restart},  // a kNetServers key
    {"DES_NET_CUSTOM", "", Apply::Restart},    // Custom's address: IPv4 or a host name
    {"DES_NET_NAME", "", Apply::Restart},      // the online name (PS3_NP_ONLINE_ID); "" = no NP identity
    {"DES_NET_P2P_PORT", "3658", Apply::Restart},  // PS3_NET_P2P_PORT, 1024-65535
    {"DES_NET_PASSWORD", "", Apply::Live},     // matchmaking password; a hash of it goes to the server; "" = everyone
    {"DES_NET_TENDENCY", "", Apply::Live},     // des_qwc_local: "" = the server's mixed in (the game's), "own" = yours only
    {"DES_NET_RATING_POLL", "", Apply::Live},  // des_net_grade_poll: seconds between rating checks; "" = the game's 300
    {"DES_NET_MESSAGES", "1", Apply::Live},    // des_net_show: 0 hides other players' messages
    {"DES_NET_BLOODSTAINS", "1", Apply::Live}, // des_net_show: 0 hides other players' bloodstains
    {"DES_NET_GHOSTS", "1", Apply::Live},      // des_net_show: 0 hides wandering ghosts
    {"DES_NET_SYNC", "", Apply::Live},         // des_net_sync: position packets a second; "" = every 6th frame
    {"ui_scale", "0", Apply::UiOnly},          // 0 = Auto, else percent
    {"show_start_screen", "1", Apply::UiOnly},
    {"menu_hint", "1", Apply::UiOnly},
    {"trophy_popups", "1", Apply::UiOnly},     // trophy_toasts.cpp; off hides the toast, the unlock is still kept
    {"trophies_show_hidden", "0", Apply::UiOnly},  // trophies_window.cpp: hidden trophies listed with their names
    {"pad_menu_chord", "0", Apply::UiOnly},    // 0 = L3 + R3, 1 = Select + Start
    {"PAD_MAP", "", Apply::Live},              // cellPad, gamepad buttons (controls_window.cpp)
    {"PAD_KEYS", "", Apply::Live},             // cellPad, changes to the keyboard and mouse layout (kKeysLayout)
    {"keys_layout", "souls", Apply::UiOnly},   // the layout PAD_KEYS changes; a file without this line is older
    {"DES_MOUSE_LOOK", "0", Apply::Live},      // mouse look (des_mouse_look; port/ui/src/mouse.cpp), read each frame
    {"DES_MOUSE_SENS", "0.09", Apply::Live},   // mouse look's degrees per mouse count
    {"DES_MOUSE_INVERT_X", "0", Apply::Live},
    {"DES_MOUSE_INVERT_Y", "0", Apply::Live},
    {"DES_MOUSE_FLICK", "15", Apply::Live},    // lock-on switching: degrees of mouse movement for a flick, 0 = off
};

std::map<std::string, std::string, std::less<>> sValues;
std::set<std::string, std::less<>> sPinned;
std::map<std::string, std::string, std::less<>> sPinnedValues;
std::string sPath;
bool sPathResolved = false;

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void make_dirs(const std::string& file) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(report::widen(file)).parent_path(), ec);
}

// The environment variables a setting drives (DES_VSYNC also sets RSX_VSYNC, which the D3D12 backend reads live;
// DES_SKIP_INTRO_MOVIE sets PS3_SAIL_SKIP, which cellSail reads as each movie opens; PAD_KEYS gives cellPad the
// layout with the changes after it).
void apply_env(std::string_view key, const std::string& value) {
    const std::string k(key);
    // (an unset option stays unset; DES_FPS "" is the game's mode, PAD_MAP "" the game's buttons, PAD_KEYS "" the
    // layout as it is, the two online rows' "" the game's behaviour again, and DES_ASPECT's and DES_HUD's "" 16:9 and
    // a centred HUD again)
    if (value.empty() && key != "DES_FPS" && key != "PAD_MAP" && key != "PAD_KEYS" && key != "DES_NET_TENDENCY" &&
        key != "DES_NET_RATING_POLL" && key != "DES_ASPECT" && key != "DES_HUD")
        return;
    if (key == "DES_NET_PASSWORD") return;  // (only its hash is used, in PS3_HTTP_HEADER: apply_match_group)
    if (key == "PAD_KEYS") {
        ps3_setenv("PAD_KEYS", keys_with_layout(value).c_str());
        return;
    }
    ps3_setenv(k.c_str(), value.c_str());
    if (key == "DES_VSYNC") ps3_setenv("RSX_VSYNC", value == "0" ? "0" : "1");
    if (key == "DES_SKIP_INTRO_MOVIE") ps3_setenv("PS3_SAIL_SKIP", value == "0" ? "" : "ds_advertise.pam");
}

}  // namespace

const char* const kKeysLayout =
    "ls_up=w,ls_down=s,ls_left=a,ls_right=d,walk=lalt,"
    "circle=space,l3=c,r1=mouse1,r2=shift+mouse1,l1=mouse2,l2=lctrl|shift+mouse2,r3=q|mouse3,"
    "cross=e,square=r,triangle=f,"
    "up=up|wheelup,down=down|wheeldown,right=right|shift+wheelup,left=left|shift+wheeldown,"
    "start=esc,select=g|tab";

std::string keys_with_layout(const std::string& changes) {
    return changes.empty() ? std::string(kKeysLayout) : std::string(kKeysLayout) + "," + changes;
}

const Def* find(std::string_view key) {
    for (const Def& d : kDefs) {
        if (key == d.key) return &d;
    }
    return nullptr;
}

bool harness_run() {
    const char* kind = std::getenv("DES_KIND");
    return env_set("PS3_PIDFILE") && !(kind && std::strcmp(kind, "play") == 0);
}

bool unattended_run() {
    return harness_run() || env_set("PAD_FILE") || env_set("EXIT_AFTER_MS");
}

bool scripted_run() {
    // DES_AUTO_CONTINUE counts only from the command line: from settings.ini it waits for Play instead
    // (des_ui_start_screen_up).
    const char* ac = std::getenv("DES_AUTO_CONTINUE");
    const bool auto_continue = pinned("DES_AUTO_CONTINUE") && ac && *ac && std::strcmp(ac, "0") != 0;
    return unattended_run() || auto_continue;
}

bool no_user_input() {
    const char* v = std::getenv("PS3_NO_USER_INPUT");  // a number, as the runtime reads it: 0 or unset is off
    return v && std::atoi(v) != 0;
}

const std::string& path() {
    if (sPathResolved) return sPath;
    sPathResolved = true;
    if (const char* p = std::getenv("DES_SETTINGS"); p && *p) {
        sPath = p;
    } else if (const auto& dir = report::data_dir(); !dir.empty()) {
        sPath = report::narrow((dir / L"settings.ini").wstring());  // (empty in a test run: no settings kept)
    }
    return sPath;
}

std::string describe() {
    std::string out;
    for (const Def& d : kDefs) {
        // (an issue report leaves out the online name and the custom server's address: only whether one is set)
        const bool hide = std::strcmp(d.key, "DES_NET_NAME") == 0 || std::strcmp(d.key, "DES_NET_CUSTOM") == 0 ||
                          std::strcmp(d.key, "DES_NET_PASSWORD") == 0;
        const auto shown = [hide](const std::string& v) { return hide && !v.empty() ? std::string("(set)") : v; };
        const std::string value = get(d.key);
        out += std::string(d.key) + " = " + shown(value);
        if (pinned(d.key)) out += "    # set in the environment: " + shown(pinned_value(d.key));
        else if (value != d.def && !hide) out += std::string("    # changed; default: ") + (*d.def ? d.def : "(none)");
        out += '\n';
    }
    return out;
}

namespace {

// Reads settings.ini, if there is one, and applies the options it sets that the environment does not.
void load_file() {
    const std::string& file = path();
    if (file.empty()) return;
    FILE* f = report::open_file(report::widen(file), "r");
    if (!f) return;
    char line[512];
    int applied = 0;
    bool layout_line = false;
    while (std::fgets(line, sizeof line, f)) {
        const std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
        if (key == "PS3_SAIL_MOVIES") {  // the older "Intro movies" row: off skipped every movie
            if (sValues.count("DES_SKIP_INTRO_MOVIE")) continue;
            key = "DES_SKIP_INTRO_MOVIE";
            value = value == "0" ? "1" : "0";
        }
        const Def* d = find(key);
        if (!d || d->apply == Apply::Session) continue;  // (a per-session value is never read from the file)
        if (key == "keys_layout") layout_line = true;
        sValues[key] = value;
        if (d->apply != Apply::UiOnly && !sPinned.count(key) && !value.empty()) {
            apply_env(key, value);
            applied++;
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "[ui] settings: %s (%d option%s applied)\n", file.c_str(), applied, applied == 1 ? "" : "s");
    // A file from before the Souls layout has no keys_layout line, and its PAD_KEYS is the old Keyboard page's: changes
    // to the old keys, one a button, 0x00 for none. Read as changes to the layout, an old start=0x00 would leave Esc
    // unbound and an old cross=0x20 put Space on Circle and Cross, so the keys start from the layout again
    // (load_and_apply_env sets PAD_KEYS after this).
    if (!layout_line && !get("PAD_KEYS").empty()) {
        std::fprintf(stderr, "[ui] settings: PAD_KEYS from the old Keyboard page (%s) dropped: the keys are the Souls "
                     "layout again\n", get("PAD_KEYS").c_str());
        sValues["PAD_KEYS"] = "";
    }
}

std::string sNetStatus = "Off";
bool sNetOnline = false;  // apply_network went online: the matchmaking group can then change live
bool sHeaderPinned = false;  // PS3_HTTP_HEADER came from the environment (it wins over the password)

// SHA-256 (FIPS 180-4) of a string, as 64 lowercase hex digits: the matchmaking group the server sees instead of the
// password. (Here rather than from ps3recomp: CI builds the UI on its own.)
std::string sha256_hex(const std::string& text) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = text;
    const uint64_t bits = (uint64_t)text.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (8 * i));
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const auto* b = (const unsigned char*)m.data() + off + 4 * i;
            w[i] = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    char hex[9];
    for (uint32_t v : h) {
        std::snprintf(hex, sizeof hex, "%08x", v);
        out += hex;
    }
    return out;
}

// A setting's value in use: the environment's when it was set there, else settings.ini's or the default.
std::string effective(std::string_view key) {
    return pinned(key) ? pinned_value(key) : get(key);
}

// Sets a runtime knob the network rows drive, unless the environment set it when the game started: that value wins,
// as for the rows' own variables. (load_and_apply_env runs once, before anything here has set these.)
void set_net_knob(const char* name, const std::string& value) {
    if (env_set(name)) {
        const bool hide = std::strcmp(name, "PS3_NP_ONLINE_ID") == 0;  // (the name stays out of the log)
        std::fprintf(stderr, "[ui] network: %s=%s from the environment wins\n", name,
                     hide ? "(set)" : std::getenv(name));
        return;
    }
    ps3_setenv(name, value.c_str());
}

// The header for a server that reads it (dessego's X-DeS-Options; others ignore it), "" for none: group=<hash> puts
// signs, invasions and the Old Monk in the password's group. The password itself never leaves the PC.
std::string net_options_header() {
    std::string options;
    const std::string password = trim(effective("DES_NET_PASSWORD"));
    if (!password.empty()) options += "group=" + sha256_hex("des-match-group:" + password).substr(0, 32);
    return options.empty() ? std::string() : "X-DeS-Options: " + options;
}

// The network rows -> the runtime's knobs, once at start (the sockets and NP are set up after this, at boot):
//   online: PS3_NET_ONLINE=1, PS3_NET_HOSTS=<the six names>=<server>, PS3_NP_ONLINE_ID=<name>,
//           PS3_NET_P2P_PORT=<port>; online needs a valid online name (this row's, or PS3_NP_ONLINE_ID's);
//   offline: none of them is set. Not even to "0": the runtime counts any value as on.
// PSNR_SERVER is never set here (no row yet). Fails closed: with no valid server address and no PS3_NET_HOSTS from the
// environment, the game stays offline rather than reach whoever owns the original names now.
// A developer build's cheat session wins over this: des_dev_boot runs after des_ui_early_init (which calls
// load_and_apply_env) in ps3_title_options_init, and clears PS3_NET_ONLINE, PSNR_SERVER and PS3_NET_HOSTS when a cheat
// is set at start (docs/55 section 6). So nothing here checks DES_DEV_SESSION; the Network tab reads it to say why.
void apply_network() {
    const std::string online = effective("DES_ONLINE");
    if (online.empty() || online == "0") {
        sNetStatus = "Off: Online is off";
        return;
    }
    if (harness_run() && !pinned("DES_ONLINE")) {  // (deslib.sh sets DES_ONLINE=0 for test runs as well)
        sNetStatus = "Off: a test run (DES_ONLINE not set)";
        std::fprintf(stderr, "[ui] network: off in a test run unless DES_ONLINE=1 is given\n");
        return;
    }
    const std::string address = net_server_address();
    if (address.empty() && !env_set("PS3_NET_HOSTS")) {
        const NetServer* s = net_server(effective("DES_NET_SERVER"));
        sNetStatus = s ? "Off: no valid server address" : "Off: unknown server " + effective("DES_NET_SERVER");
        std::fprintf(stderr, "[ui] network: off, %s (DES_NET_SERVER=%s)\n",
                     s ? "no valid server address" : "unknown server", effective("DES_NET_SERVER").c_str());
        return;
    }
    // No online name, no sign-in: the runtime's NP stays signed out, and the game then asks for the network over and
    // over after Continue (seen with Online on and no name: the net-start dialog in a loop, never the level). So
    // without a valid name, from this row or the environment, the session stays offline.
    const std::string name = effective("DES_NET_NAME");
    if (!net_name_ok(name) && !env_set("PS3_NP_ONLINE_ID")) {
        sNetStatus = name.empty() ? "Off: no online name" : "Off: the online name is not valid";
        std::fprintf(stderr, "[ui] network: off, %s (Settings > Network)\n",
                     name.empty() ? "no online name" : "the online name is not valid (3-16 letters, digits, - or _)");
        return;
    }
    const bool hosts_pinned = env_set("PS3_NET_HOSTS");
    set_net_knob("PS3_NET_ONLINE", "1");
    if (!address.empty()) set_net_knob("PS3_NET_HOSTS", net_hosts(address));
    if (net_name_ok(name)) set_net_knob("PS3_NP_ONLINE_ID", name);
    int port = std::atoi(effective("DES_NET_P2P_PORT").c_str());
    if (port < kNetP2pPortMin || port > kNetP2pPortMax) port = kNetP2pPortDefault;
    set_net_knob("PS3_NET_P2P_PORT", std::to_string(port));
    const std::string header = net_options_header();
    sHeaderPinned = env_set("PS3_HTTP_HEADER");
    if (!header.empty()) set_net_knob("PS3_HTTP_HEADER", header);
    sNetOnline = true;
    // (what is in use, the environment's values included)
    const NetServer* s = net_server(effective("DES_NET_SERVER"));
    sNetStatus = "Online (" + std::string(hosts_pinned        ? "PS3_NET_HOSTS from the environment"
                                          : s && *s->address ? s->name
                                                             : address) + ")";
    std::fprintf(stderr, "[ui] network: online, server %s, P2P port %s, %s%s\n",
                 hosts_pinned ? "from PS3_NET_HOSTS" : address.c_str(), std::getenv("PS3_NET_P2P_PORT"),
                 env_set("PS3_NP_ONLINE_ID") ? "with an online name" : "no online name",
                 header.empty() ? "" : ", a matchmaking password");
}

// A changed password, online: the next request has the new group (cellHttp reads PS3_HTTP_HEADER for each one), and
// dessego moves a sign already down along at its owner's next checkSosData. A PS3_HTTP_HEADER from the environment
// wins.
void apply_match_group() {
    if (!sNetOnline || sHeaderPinned) return;
    const std::string header = net_options_header();
    ps3_setenv("PS3_HTTP_HEADER", header.c_str());  // ("" unsets it)
    std::fprintf(stderr, "[ui] network: matchmaking password %s\n", header.empty() ? "cleared" : "changed");
}

// logs\settings.txt: describe(), kept current for the crash and hang dialog. It is a process of its own that reads
// no settings, and settings.ini alone has neither the defaults nor what the environment set (and a player who never
// changed a setting has none).
void write_in_use() {
    const auto& dir = report::data_dir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir / L"logs", ec);
    if (FILE* f = report::open_file(dir / L"logs" / L"settings.txt", "wb")) {
        const std::string s = describe();
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    }
}

}  // namespace

void load_and_apply_env() {
    for (const Def& d : kDefs) {
        if (d.apply == Apply::UiOnly) continue;
        if (env_set(d.key) || (std::strcmp(d.key, "DES_VSYNC") == 0 && env_set("RSX_VSYNC"))) {
            sPinned.insert(d.key);
            const char* v = std::getenv(d.key);
            if (!(v && *v)) v = std::getenv("RSX_VSYNC");  // DES_VSYNC pinned through RSX_VSYNC
            sPinnedValues[d.key] = v ? v : "";
        }
    }
    load_file();
    // The keyboard and mouse layout applies with or without settings.ini; a PAD_KEYS from the environment changes it
    // as the setting would.
    const std::string keys = pinned("PAD_KEYS") ? pinned_value("PAD_KEYS") : get("PAD_KEYS");
    ps3_setenv("PAD_KEYS", keys_with_layout(keys).c_str());
    std::fprintf(stderr, "[ui] keyboard and mouse: the Souls layout%s%s\n", keys.empty() ? "" : ", changed by ",
                 keys.c_str());
    apply_network();
    write_in_use();
}

std::string get(std::string_view key) {
    if (const auto it = sValues.find(key); it != sValues.end()) return it->second;
    const Def* d = find(key);
    return d ? d->def : "";
}

int get_int(std::string_view key) {
    return std::atoi(get(key).c_str());
}

bool is_default(std::string_view key) {
    const Def* d = find(key);
    return d && get(key) == d->def;
}

void set(std::string_view key, const std::string& value) {
    const Def* d = find(key);
    if (!d) return;
    sValues[std::string(key)] = value;
    if ((d->apply == Apply::Live || d->apply == Apply::Session) && !pinned(key))
        apply_env(key, value.empty() ? d->def : value);
    if (key == "DES_NET_PASSWORD" && !pinned(key)) apply_match_group();
    save();
    write_in_use();
}

bool pinned(std::string_view key) {
    return sPinned.count(key) != 0;
}

std::string pinned_value(std::string_view key) {
    const auto it = sPinnedValues.find(key);
    return it == sPinnedValues.end() ? std::string() : it->second;
}

const NetServer* net_server(std::string_view key) {
    for (const NetServer& s : kNetServers) {
        if (key == s.key) return &s;
    }
    return nullptr;
}

bool net_address_ok(std::string_view a) {
    // Letters, digits, '.' and '-' only (so nothing can break PS3_NET_HOSTS's "name=address&&" form), at most a host
    // name's 253 characters, no empty label and no label starting or ending with '-'. A dotted IPv4 address passes as
    // it is; a host name is looked up by the runtime.
    if (a.empty() || a.size() > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i <= a.size(); ++i) {
        const char c = i < a.size() ? a[i] : '.';
        if (c == '.') {
            if (label == 0 || a[i - 1] == '-') return false;
            label = 0;
        } else if (std::isalnum((unsigned char)c) || c == '-') {
            if (c == '-' && label == 0) return false;
            if (++label > 63) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool net_name_ok(std::string_view n) {
    if (n.size() < 3 || n.size() > 16) return false;
    for (const char c : n) {
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    }
    return true;
}

std::string net_server_address() {
    const NetServer* s = net_server(effective("DES_NET_SERVER"));
    if (!s) return {};
    const std::string a = *s->address ? std::string(s->address) : trim(effective("DES_NET_CUSTOM"));
    return net_address_ok(a) ? a : std::string();
}

std::string net_hosts(const std::string& address) {
    std::string out;
    for (const char* h : kNetHosts) {
        if (!out.empty()) out += "&&";
        out += std::string(h) + "=" + address;
    }
    return out;
}

const std::string& net_status() {
    return sNetStatus;
}

void save() {
    const std::string& file = path();
    if (file.empty()) return;
    make_dirs(file);
    const std::string tmp = file + ".tmp";
    FILE* f = report::open_file(report::widen(tmp), "w");
    if (!f) return;
    std::fprintf(f,
                 "# Demon's Souls PC port settings (key = value), written by the in-game menu.\n"
                 "# The option keys are the environment variables of the same name; a variable set in the\n"
                 "# environment wins over this file.\n");
    for (const Def& d : kDefs) {
        if (d.apply != Apply::Session) std::fprintf(f, "%s = %s\n", d.key, get(d.key).c_str());
    }
    std::fclose(f);
    report::replace_file(report::widen(tmp), report::widen(file));
}

}  // namespace ui::settings
