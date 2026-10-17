// From FFCCEOT-Recomp's app.cpp. Adapted for Demon's Souls.
#include "app.hpp"

namespace ui {

App& app() {
    static App instance;
    return instance;
}

}  // namespace ui
