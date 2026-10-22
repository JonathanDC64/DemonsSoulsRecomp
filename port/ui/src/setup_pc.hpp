// Written for Demon's Souls. What the setup screens ask of this PC (docs/88 section 2a): where a disc dump is likely
// to be, how much room the data folder's disk has, about how long setup takes here, and whether the PC runs on its
// battery. One small function for each platform where the facts differ (the usual folders, the memory, the power);
// the rest is portable code.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ui::setup {

// The one title the port supports (tools/setup/des_setup/game_table.json). The engine checks the rest.
inline constexpr const char* kTitleId = "BLUS30443";

// Setup's peak disk use from scratch: about 2.5 GB during the compile and link, and it asks for 4 GB free (docs/52
// 10.2, after docs/45). Inferred from measured parts; a whole run on a player's PC has not been measured.
inline constexpr unsigned long long kNeedBytes = 4ull << 30;

// A folder that holds PS3_GAME/PARAM.SFO.
struct Place {
    std::filesystem::path folder;
    std::string title;    // PARAM.SFO's TITLE_ID ("BLUS30443")
    std::string version;  // its APP_VER ("01.00")
};
// Disc dumps in the usual places, the supported title first, up to 4. The places: the user's Downloads, Documents and
// Desktop, and the drives (Windows: the fixed and removable drives' roots; Linux: the home folder and the mounted
// drives). In each: the place itself, its folders, and the folders of the ones with a usual name for a folder of
// games (PS3, Games, RPCS3 and the like), two such levels at most. Nothing deeper. A sleeping disk wakes up for it, so
// it runs off the UI's thread.
std::vector<Place> likely_places();

// The free space for setup's data folder (on its nearest folder that exists), and a name for its disk: "C:", or the
// folder where there are no drive letters.
struct Space {
    bool known = false;
    unsigned long long free = 0;
    std::string where;
};
Space data_space();

// About how long a setup from scratch takes on this PC, in minutes: a range, from the build's CPU time measured on
// one PC, the jobs this PC's threads and memory allow, and cores up to 2 times slower (4 on a laptop) (setup_pc.cpp;
// docs/88 2a).
struct Estimate {
    int lowMinutes = 0, highMinutes = 0;
    unsigned jobs = 1;
};
Estimate setup_time();

// The PC's power: whether it has a battery of its own (a laptop), and whether it runs on it now (not plugged in).
// Both false when it cannot tell: a desktop has no battery to ask.
struct Power {
    bool battery = false;
    bool onBattery = false;
};
Power power();

// The display the UI's window is on (else the primary one): its mode in pixels and its refresh rate. Zeros when it
// cannot tell. For the options page's hints and the matched render resolution (docs/91).
struct Display {
    int width = 0, height = 0, hz = 0;
};
Display display();

}  // namespace ui::setup
