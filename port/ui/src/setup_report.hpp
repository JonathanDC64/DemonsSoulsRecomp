// Written for Demon's Souls. A problem report from any of the setup screens' errors (docs/74, report.hpp): a zip in the
// reports folder, then the issue form, filled in. The zip holds this PC's facts, setup's log, the failed step's log,
// the unlock's log after an RPCS3 failure, the host's own log and the journal, each with the player's name and folders
// hidden. Nothing of the game: the logs hold step names, counts, addresses and the tools' messages (docs/88 section 6).
#pragma once

#include "setup_protocol.hpp"

#include <string>

namespace ui::setup {

// The report for `failure` (its sentence, code, step and log). What to tell the player after: where the file is, or
// that it could not be saved.
std::string send_report(const Status& failure);

}  // namespace ui::setup
