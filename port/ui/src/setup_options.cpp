// The setup screens' presets (see setup_options.hpp).
#include "setup_options.hpp"

#include <cstdlib>

namespace ui::setup {
namespace {

// The resolutions the Render resolution row offers (settings_window.cpp), as the preset page names them.
std::string scale_name(int scale, const char* more = "") {
    return std::to_string(scale) + "% (" + scale_label(scale) + more + ")";
}

}  // namespace

const char* preset_key(Preset preset) {
    switch (preset) {
    case Preset::Original: return "original";
    case Preset::Enhanced: return "enhanced";
    default: return "custom";
    }
}

const char* preset_name(Preset preset) {
    switch (preset) {
    case Preset::Original: return "Original";
    case Preset::Enhanced: return "Enhanced";
    default: return "Custom";
    }
}

bool preset_from_key(std::string_view key, Preset& out) {
    for (const Preset p : {Preset::Original, Preset::Enhanced, Preset::Custom}) {
        if (key != preset_key(p)) continue;
        out = p;
        return true;
    }
    return false;
}

int matched_scale(int height) {
    int best = 100;
    for (const int scale : {150, 200})
        if (height > 0 && 720 * scale / 100 <= height) best = scale;
    return best;
}

// Both presets write the same keys, so picking one after the other leaves nothing of the first. The ones neither
// changes (Texture filtering, HUD, cutscene frame rate, V-sync) go back to their defaults with them.
Settings preset_settings(Preset preset, int displayHeight) {
    if (preset == Preset::Custom) return {};
    const bool e = preset == Preset::Enhanced;
    return {
        {"DES_FPS", e ? "60" : ""},
        {"RSX_SCALE", std::to_string(e ? matched_scale(displayHeight) : 100)},
        {"RSX_AA", "0"},  // (Enhanced too: FXAA softens the picture, and many players turn it off; Settings has it)
        {"RSX_ANISO", e ? "16" : "0"},
        {"RSX_OUTFILTER", e ? "1" : "0"},
        {"DES_FULLSCREEN", e ? "1" : "0"},
        {"DES_MOTION_BLUR", "1"},
        {"DES_DOF", "1"},
        {"DES_ASPECT", ""},
        {"RSX_TEXFILTER", "0"},
        {"DES_HUD", ""},
        {"DES_CUTFPS", "0"},
        {"DES_VSYNC", "1"},
    };
}

std::vector<Change> preset_changes(int displayHeight) {
    const int matched = matched_scale(displayHeight);
    return {
        {"Frame rate", "30 fps", "60 fps"},
        {"Render resolution", scale_name(100), scale_name(matched, displayHeight > 0 ? ", your display" : "")},
        {"Anisotropic filtering", "The game's own (4x)", "16x"},
        {"Scaling filter", "Bilinear", "Sharp"},
        {"Display mode", "Window", "Fullscreen"},
        {"Motion blur, depth of field", "On", "On"},
        {"Aspect ratio", "16:9", "16:9"},
    };
}

std::string scale_label(int scale) {
    switch (scale) {
    case 100: return "720p";
    case 150: return "1080p";
    case 200: return "1440p";
    case 300: return "4K";
    default: return std::to_string(scale) + "%";
    }
}

std::string summary(Preset preset, const std::function<std::string(std::string_view)>& get) {
    const std::string fps = get("DES_FPS");
    std::string s = std::string(preset_name(preset)) + ": ";
    if (fps.empty() || fps == "30") s += "30 fps";
    else if (fps == "unlocked") s += "unlocked frame rate";
    else s += fps + " fps";
    s += ", " + scale_label(std::atoi(get("RSX_SCALE").c_str()));
    const std::string aspect = get("DES_ASPECT");
    if (aspect == "auto") s += ", the display's shape";
    else if (aspect == "32:9") s += ", 32:9";
    else if (!aspect.empty()) s += ", 21:9";
    s += get("DES_FULLSCREEN") == "1" ? ", fullscreen" : ", window";
    s += get("DES_MOUSE_LOOK") == "1" ? ", keyboard and mouse" : ", gamepad";
    s += get("DES_ONLINE") == "0" ? ", online off." : ", online on.";
    return s;
}

}  // namespace ui::setup
