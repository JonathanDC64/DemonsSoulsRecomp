// Written for Demon's Souls (docs/69 section 6). The privacy note (PRIVACY.md at the top of the repository, embedded)
// and its question at start: before the game goes online with a server that keeps what the note says
// (NetServer::privacy), the player accepts this version of the note or plays offline. The answer is asked once per
// version: a note with a higher version asks again.
#pragma once

#include <string>
#include <vector>

namespace ui::privacy {

// One part of the note, Markdown taken out: a heading, a list item or a paragraph (lines joined).
struct Part {
    enum class Kind { Heading, Item, Text };
    Kind kind;
    std::string text;
};
// The note's parts after its title and version line, and its version ("Version N" in it; 1 when it has none).
const std::vector<Part>& parts();
int version();

// At start (settings.cpp's apply_network): the game goes online with a server the note is about, and the player has
// not accepted this version. Until the answer nothing may reach the server: the start waits (des_ui_privacy_wait,
// before the game's code runs), and the banners are not fetched (net_notices.cpp).
void set_needed(bool needed);
bool unanswered();
// The start waits for the answer: the frame hook shows PrivacyWindow.
bool asking();
// The game's thread: marks the start as waiting for the answer.
void begin_asking();
// Blocks until the answer. `wake` runs every quarter second until then (the frame hook may be asleep).
void wait_answer(void (*wake)());
// Yes: this version is accepted, kept in settings.ini. No: this run is offline (ps3_net_lock_offline and the network
// knobs cleared), and the next start asks again. Either lets the start go on. Once; later calls do nothing.
void answer(bool accept);

}  // namespace ui::privacy
