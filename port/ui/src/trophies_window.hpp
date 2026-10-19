// The Trophies window (docs/44, section 5.4), opened from the menu bar and the start screen: the trophy set's summary
// (how many are unlocked, the percentage, the count per grade) over a list of every trophy with its icon, name,
// description, grade and unlock date. A hidden trophy shows as "Hidden trophy" until it is unlocked; pressing its row
// reveals it, and "Show hidden trophies" reveals them all.
#pragma once

#include "window.hpp"

#include <memory>

namespace ui {

class TrophiesWindow : public Window {
public:
    TrophiesWindow();

    void update() override;

    struct State;  // the snapshot and what is revealed, shared with the rows (trophies_window.cpp)

private:
    void build_tab(Rml::Element* content);
    void render_summary();

    std::shared_ptr<State> mState;
    Rml::Element* mSummary = nullptr;
    unsigned mSeenUnlocks = 0;
    unsigned long long mNextRetryAt = 0;
};

}  // namespace ui
