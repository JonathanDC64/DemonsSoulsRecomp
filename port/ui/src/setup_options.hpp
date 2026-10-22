// Written for Demon's Souls. The setup screens' presets (docs/91): Original, the game as it was on the PS3, and
// Enhanced, sharper and smoother; each a list of settings (settings.hpp keys) written when the player picks it. Custom
// starts from the settings in use. Pure tables and arithmetic, so the screens and a test read the same values.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui::setup {

enum class Preset { Original, Enhanced, Custom };

// The preset as the setup_preset setting keeps it ("original", "enhanced", "custom") and as the screens name it.
const char* preset_key(Preset preset);
const char* preset_name(Preset preset);
// The preset a setup_preset value names; false for none ("": the options were not picked yet).
bool preset_from_key(std::string_view key, Preset& out);

using Settings = std::vector<std::pair<const char*, std::string>>;

// The render resolution (RSX_SCALE) that matches a display `height` pixels tall: the largest of 100, 150 and 200 whose
// picture (720 * scale / 100) still fits. Capped at 200: 300 on a 4K display costs much more GPU time for a picture
// few can tell from 200 (docs/91). 100 when the height is not known (0).
int matched_scale(int height);

// The settings a preset writes, every key it touches with its value. Custom: none (it keeps what is there). The
// display's height picks Enhanced's render resolution.
Settings preset_settings(Preset preset, int displayHeight);

// One line of "What changes" (Original against Enhanced), as the preset page shows it.
struct Change {
    const char* name;
    std::string original, enhanced;
};
std::vector<Change> preset_changes(int displayHeight);

// A render resolution (RSX_SCALE) as the screens name it: "720p", "1080p", "1440p", "4K", else "175%".
std::string scale_label(int scale);

// The options as Ready to play sums them up, `get` giving each setting's value:
// "Enhanced: 60 fps, 1440p, fullscreen, gamepad, online on."
std::string summary(Preset preset, const std::function<std::string(std::string_view)>& get);

}  // namespace ui::setup
