// Written for Demon's Souls (docs/32).
// The port's settings, as the UI edits them: settings.ini (key = value), keyed by the options' own environment
// variable names (DES_FPS, DES_VSYNC, ...) plus a few of the UI's own (ui_scale, show_start_screen, ...).
//
// At startup a value from settings.ini becomes its environment variable only where that variable is not set already:
// an explicit environment variable always wins (so every test script keeps its options), and the UI shows such a row
// as set from outside. Test harness runs (tools/des scripts other than play.sh) neither read nor write the file
// unless DES_SETTINGS names one.
#pragma once

#include <string>
#include <string_view>

namespace ui::settings {

enum class Apply {
    Live,     // applied at once (a live knob)
    Restart,  // read when the game starts
    UiOnly,   // the UI's own
    Session,  // live, but never read from or written to settings.ini: each start begins at the default (the
              // developer build's cheats, docs/55)
};

struct Def {
    const char* key;
    const char* def;  // default value ("" = unset)
    Apply apply;
};

// The file's location (UTF-8): DES_SETTINGS, else settings.ini in the data folder (report::data_dir(): DES_DATA_DIR,
// <exe dir>\data when portable.txt is there, else %APPDATA%\DemonsSoulsRecomp\DemonsSouls). Empty when the settings
// are not kept (a test harness run).
const std::string& path();

// The settings in use, as settings.ini lines, with the values the environment set noted (for an issue report).
std::string describe();

// Reads settings.ini and sets the environment variables it holds that are not set yet. Once, at startup, before the
// game's options are read.
void load_and_apply_env();

const Def* find(std::string_view key);
std::string get(std::string_view key);  // the value, or its default
int get_int(std::string_view key);
bool is_default(std::string_view key);

// Stores the value, applies it if it is live or per session (and not pinned) and saves settings.ini (where a
// per-session value is never written).
void set(std::string_view key, const std::string& value);

// Stores the value and saves settings.ini, without applying it: for the setup screens, which run before the game. The
// game, started after them, reads the file; a value put in this process's environment would reach it as one set from
// outside, and its row would show as pinned. (Display mode still takes effect at once: the window follows the stored
// value each frame, des_ui.cpp's sync_fullscreen.)
void store(std::string_view key, const std::string& value);

// The variable was set in the environment when the game started (not by settings.ini): it wins over the file.
bool pinned(std::string_view key);
std::string pinned_value(std::string_view key);

// The keyboard and mouse layout (docs/41), in cellPad's PAD_KEYS form: movement on WASD, the attacks on the mouse
// buttons, Space to roll and dash, and so on, as in the Souls games on PC. The PAD_KEYS setting, and a PAD_KEYS set in
// the environment, hold changes to it only: cellPad gets the layout followed by them, and an action named there loses
// the layout's bindings.
extern const char* const kKeysLayout;
std::string keys_with_layout(const std::string& changes);

// True for a test harness run (PS3_PIDFILE set by tools/des, and not a play.sh session) and for any run whose input
// is scripted (DES_AUTO_CONTINUE, PAD_FILE, EXIT_AFTER_MS): no start screen there.
bool harness_run();
bool scripted_run();
// scripted_run() without DES_AUTO_CONTINUE: nobody plays the run. The close button closes at once only there; a
// play session with DES_AUTO_CONTINUE on its command line skips the start screen but still asks. The window opens
// maximized (DES_MAXIMIZED) only outside it, unless DES_MAXIMIZED is given.
bool unattended_run();

// PS3_NO_USER_INPUT=1 (test runs; tools/des/deslib.sh sets it): the user's pads, keys and mouse do nothing. The
// runtime reads the same variable (ps3_knob.h) for the game's pad and the window.
bool no_user_input();

// ---- Online play (the Network tab; docs/46 A5) ----

// This project's own server: the sign-in and psnr reach it by this name, and its TLS certificate is checked for it.
// An empty name keeps online off with it ("not available yet").
inline constexpr const char* kNetProjectHost = "server.boletaria.org";

// The game servers the Server row offers; edit this table to add, change or drop one. The key is what settings.ini
// stores (DES_NET_SERVER); the first entry, the project's server, is the default. A stored key that is not in the
// table (The Archstones' or Custom's in a release build) reads as the default. Custom uses DES_NET_CUSTOM.
struct NetServer {
    const char* key;
    const char* name;
    const char* address;  // IPv4 or host name; "" for Custom, or for a server with no address yet
    const char* note;     // the Server row's help text for it
    bool signin;          // may run this project's server: the runtime signs in to it (a server without it: no harm)
    bool psnr;            // runs its matchmaking (psnr) on kNetPsnrPort: co-op and invasions go through it
    bool tls;             // the sign-in, the game's requests and psnr go over TLS, with a certificate for address
    bool privacy;         // keeps what PRIVACY.md says: the player accepts the note before going online (privacy.hpp)
};
inline constexpr NetServer kNetServers[] = {
    {"project", "Demon's Souls Recomp", kNetProjectHost, "This project's own server.", true, true, true, true},
#if DES_DEV
    // Developer builds only. The address is the one its setup page publishes for emulators (thearchstones.com,
    // "serverinfo"). Not this project's server: no sign-in (it would get the install's key and name for nothing).
    {"archstones", "The Archstones", "206.189.232.242",
     "The community server RPCS3 players use, run by a volunteer. Developer builds only.", false, false, false,
     false},
#endif
    {"local", "Local test server", "127.0.0.1", "A server on this PC, for testing.", true, false, false, false},
#if DES_DEV
    // Developer builds only, for now (0.1.0): with no Custom choice, a settings.ini that names a server of its own
    // (a repacked portable download) cannot send players there. Bringing it back to players needs a question at
    // start that custom_chosen_here's mark answers.
    {"custom", "Custom", "", "The address in Custom server, below. Developer builds only.", true, false, false,
     false},
#endif
};
inline constexpr const char* kNetCustomKey = "custom";

// The six host names the game looks up for its servers (every server's setup guide redirects these six). Online,
// PS3_NET_HOSTS maps each of them to the chosen server's address, and the runtime answers no other name.
inline constexpr const char* kNetHosts[] = {
    "ds-eu-c.scej-online.jp", "ds-eu-g.scej-online.jp", "c.demons-souls.com",
    "g.demons-souls.com",     "cmnap.scej-online.jp",   "demons-souls.scej-online.jp",
};

// This project's server (its docs/identity.md): the sign-in on the bootstrap port, psnr on its own.
inline constexpr int kNetSigninPort = 18000;
inline constexpr int kNetPsnrPort = 36100;
// Which builds play together: a build only matches others with the same number. Raise it when a change means a
// build can no longer play with older ones.
inline constexpr int kNetNetworkVersion = 1;

inline constexpr int kNetP2pPortDefault = 3658;  // the PS3's own P2P port
inline constexpr int kNetP2pPortMin = 1024;
inline constexpr int kNetP2pPortMax = 65535;

const NetServer* net_server(std::string_view key);  // null for a key not in the table
bool net_address_ok(std::string_view address);       // an IPv4 address or a host name, nothing else
bool net_name_ok(std::string_view name);             // 3-16 letters, digits, '-' or '_' (a PSN online ID)
// An online name for a player who has none: a word from the game and four digits ("Stonefang-4821"), which
// net_name_ok accepts. Setup offers it, and the Network tab falls back to it rather than leave the name empty.
std::string suggest_net_name();
// The chosen server's address (DES_NET_SERVER, DES_NET_CUSTOM for Custom), or "" when there is none or it is not
// a valid address.
std::string net_server_address();
// A Custom server address is used only once it was chosen on this PC: typed into Custom server or picked in Server
// (choose_custom_here), or set in the environment. One that came in someone else's settings.ini (a portable download
// repacked to name a server of its own) leaves online off until then. The mark (net_custom_here) is a hash of this
// PC's id and the address, so a settings.ini copied to another PC does not carry it.
bool custom_chosen_here();
bool choose_custom_here();  // true when it marked an address that was not chosen here before
// apply_network left online off at start because the Custom server was not chosen on this PC (des_ui.cpp says so).
bool net_custom_held();
// PS3_NET_HOSTS for an address: each of kNetHosts mapped to it, joined with "&&" (RPCS3's IP/Host switch form).
std::string net_hosts(const std::string& address);
// Where the install's sign-in key is kept (PS3_NET_KEY_FILE): beside settings.ini, so updates keep it. "" when no
// data folder is kept (a test run).
std::string net_key_file();
// What load_and_apply_env decided about the network at start, for the Network tab ("Online (Local test server)", "Off:
// Online is off", ...). A developer build's cheat session clears the network after this (des_dev_boot); the tab
// says so from DES_DEV_SESSION.
const std::string& net_status();
// This run went offline after the start (the privacy note declined, privacy.hpp; the player's data deleted,
// net_notices.cpp): the network knobs are cleared (the runtime is locked offline as well), and net_status says why.
void net_off(const std::string& why);

void save();

}  // namespace ui::settings
