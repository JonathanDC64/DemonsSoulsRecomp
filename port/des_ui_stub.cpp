// The wrapper UI's interface (ui/include/des_ui.h) for a build without the UI (-DDES_UI=OFF; the default off
// Windows until the UI runs there, docs/76): no start screen, menu, settings.ini, log file or problem reports, and
// mouse look has no mouse. Everything the game itself does is unchanged.
#include "des_ui.h"

extern "C" {

void (*g_des_ui_sound)(int id) = nullptr;

void des_ui_early_init(void) {}

int des_ui_launch(int*, char***) {
    return -1;  // (no setup screens: the game, as before)
}

void des_ui_setup_only_init(void) {}

void des_mouse_take(int* dx, int* dy) {
    *dx = 0;
    *dy = 0;
}

void des_mouse_refresh(void) {}

int des_ui_start_screen_up(void) {
    return 0;
}

}  // extern "C"
