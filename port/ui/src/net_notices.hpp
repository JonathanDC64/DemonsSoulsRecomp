// Written for Demon's Souls (ps3recomp's docs/ONLINE.md, "Sign-in").
// What the online server has to tell the player: a refused sign-in (a ban, maintenance, an old build, with the
// server's own words) and refused co-op as toasts, and the server's banners in the thin bar at the top. The runtime
// keeps the newest message (ps3_net_signin_notice) and each new banner (ps3_net_signin_banner); the frame hook picks
// them up and the UI shows them, one after another.
#pragma once

namespace ui::net_notices {

// The frame hook, before the UI's sleep check: takes a new message from the runtime, if there is one. Cheap.
void poll();
// A message waits to be shown (it wakes the UI).
bool pending();
// The waiting message, into a toast (the UI is awake).
void drain();

}  // namespace ui::net_notices
