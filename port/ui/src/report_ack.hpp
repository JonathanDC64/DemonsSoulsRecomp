// Written for Demon's Souls. The question before a player's first problem report (docs/74): GitHub cannot attach the
// report's zip from a link, and most reports arrived without it, so the first report on a PC says where the file goes
// and asks the player to tick "I'll attach the report file to my issue" before it goes on.
#pragma once

#include <functional>

namespace ui {

// Runs `then` at once when the player has answered before on this PC (a marker in the data folder, report.hpp's
// data_dir(), so the game, its crash dialog and setup share it). Otherwise shows the question: Continue waits for the
// tick, keeps the answer and runs `then`; Cancel runs nothing.
void with_zip_ack(std::function<void()> then);

}  // namespace ui
