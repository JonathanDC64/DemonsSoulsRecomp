// Written for Demon's Souls. The Players window (docs/69, "Players you met"), opened from Settings > Network: the
// players met online lately, the latest first, each with how and when, and the players blocked. Pressing a row offers
// Block (or Unblock) and Report. The list, blocks and reports are the online server's (its docs/social.md), through
// the runtime's ps3_net_players calls (ps3recomp's docs/ONLINE.md, "Sign-in"), which never wait on the network here.
#pragma once

#include "window.hpp"

#include <memory>

namespace ui {

class PlayersWindow : public Window {
public:
    PlayersWindow();

    void update() override;

    struct State;  // the list and what the rows show, shared with them (players_window.cpp)

private:
    void build_tab(Rml::Element* content);
    void render_summary();

    std::shared_ptr<State> mState;
    Rml::Element* mSummary = nullptr;
    unsigned long long mNextAskAt = 0;  // no sign-in yet, or the last ask failed: ask again from then
};

}  // namespace ui
