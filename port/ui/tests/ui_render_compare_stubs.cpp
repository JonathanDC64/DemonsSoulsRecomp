// What the UI's library takes from the game's side (des_boot: boot_main.cpp, ppu_hle.cpp, ppu_loader.cpp, the lift,
// des_hud.cpp, des_menu_mouse.cpp, des_prework.cpp), for ui_render_compare, which runs no game: inert, as the runtime's
// host_stubs.c is for what its modules take from the PPU loader.
#include <cstdint>
#include <cstdio>

extern "C" {

uint8_t* vm_base = nullptr;  // guest memory: none
// The lift's functions: none. (extern first: a const's linkage is the file's own otherwise.)
extern const uint64_t function_table[2];
extern const uint64_t function_table_count;
const uint64_t function_table[2] = {0, 0};
const uint64_t function_table_count = 0;
uint32_t g_last_hle_nid = 0;
const char* g_last_hle_name = nullptr;

void ps3_diag_report(FILE* out, unsigned sample_sec) {
    (void)out;
    (void)sample_sec;
}
unsigned ppu_boot_frames_presented(void) { return 0; }
long long ps3_ms_since_frame(void) { return -1; }

int des_hud_edit_view(float view[4]) {
    (void)view;
    return 0;
}
int des_hud_edit_place(int part, float* dx, float* dy, float size, float box[4]) {
    (void)part;
    (void)dx;
    (void)dy;
    (void)size;
    (void)box;
    return 0;
}

int des_menu_mouse_open(void) { return 0; }
void des_menu_mouse_view(float view[4]) { (void)view; }
int des_menu_mouse_hit(float x, float y, float box[4]) {
    (void)x;
    (void)y;
    (void)box;
    return 0;
}
void des_menu_mouse_click(float x, float y) {
    (void)x;
    (void)y;
}
void des_menu_mouse_release(void) {}
void des_menu_mouse_back(void) {}
void des_menu_mouse_drag(float x, float y) {
    (void)x;
    (void)y;
}
void des_menu_mouse_wheel(int notches, float x, float y) {
    (void)notches;
    (void)x;
    (void)y;
}

int des_prework_status(float* fraction, char* text, int cap) {
    (void)fraction;
    (void)text;
    (void)cap;
    return 0;
}
void des_prework_skip(void) {}

}  // extern "C"
