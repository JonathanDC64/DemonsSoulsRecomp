// Trophy pop-ups (docs/44): the trophies the sceNpTrophy HLE unlocks, shown as toasts on the overlay.
#pragma once

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

}  // namespace ui::trophies
