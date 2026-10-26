// Written for Demon's Souls (ps3recomp's docs/ONLINE.md, "Sign-in").
// What the online sign-in has to tell the player, as a toast: a refused sign-in (a ban, maintenance, an old build,
// with the server's own words) or refused co-op. The runtime keeps the newest message (ps3_net_signin_notice); the
// frame hook picks it up and the UI shows it.
#pragma once

namespace ui::net_notices {

// The frame hook, before the UI's sleep check: takes a new message from the runtime, if there is one. Cheap.
void poll();
// A message waits to be shown (it wakes the UI).
bool pending();
// The waiting message, into a toast (the UI is awake).
void drain();

}  // namespace ui::net_notices
