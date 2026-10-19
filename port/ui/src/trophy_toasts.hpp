// Trophy pop-ups (docs/44): the trophies the sceNpTrophy HLE unlocks, shown as toasts on the overlay.
#pragma once

#include <string>

namespace ui::trophies {

// Sets the HLE's unlock hook (des_ui_early_init). From then on each unlock is queued, on the thread that unlocked.
void install();

// An unlock is queued: frame() wakes for it. Any thread.
bool pending();

// The render thread, in frame(): turns the queued unlocks into toasts while Settings, Interface, "Trophy pop-ups" is
// on. Off drops them; the HLE has saved the unlocks already.
void drain();

// Drops the queue (the UI could not start).
void discard();

// The DES_TROPHY_TEST knob (trophy_toasts.cpp). Each frame, before frame()'s fast path: one load while no value
// changed.
void poll_test_knob();

// For the toasts and the Trophies window (trophies_window.cpp):

// How many unlocks the hook has seen. Any thread; the Trophies window reads the list again when it moves.
unsigned unlock_count();

// A grade's CSS class ("platinum", "gold", "silver", "bronze") and label; "" for an unknown grade.
// (SCE_NP_TROPHY_GRADE_*: 1 platinum, 2 gold, 3 silver, 4 bronze.)
const char* grade_class(unsigned grade);
const char* grade_label(unsigned grade);

// A trophy's icon as an image source for <img> ("trophy-icon:<id>"; id -1 is the set's own icon), its PNG given to
// host::add_png; "" when the set has none. The render thread: each icon is read from the set once.
std::string icon_source(int id);

}  // namespace ui::trophies
