// Adapted from Dusklight (CC0), src/dusk/ui/overlay.hpp, by way of FFCCEOT-Recomp; adapted for Demon's Souls. Only its toasts and the "open the menu" hint are kept (the
// FPS counter, shader progress, speedrun timer and mods list belong to Dusklight's own features).
#pragma once

#include "document.hpp"

#include <chrono>

namespace ui {

class Overlay : public Document {
public:
    Overlay();

    void show() override;
    void update() override;
    bool busy() const;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void update_ticker();

    Rml::Element* mCurrentToast = nullptr;
    Rml::Element* mMenuNotification = nullptr;
    clock::time_point mCurrentToastStartTime;
    clock::time_point mMenuNotificationStartTime;

    // The bar at the top (a banner from the online server): its text holds still if it fits, else scrolls.
    Rml::Element* mTicker = nullptr;
    Rml::Element* mTickerText = nullptr;
    clock::time_point mTickerStartTime;
    float mTickerTrackWidth = -1.0f;  // (px) what the plan below was made for; -1: not laid out yet
    float mTickerOverflow = 0.0f;     // (px) how much wider the text is than its track
    float mTickerPassSeconds = 0.0f;  // one pass of a long text: hold, scroll to its end, hold
    int mTickerPasses = 0;
    float mTickerSeconds = 0.0f;      // how long it stays (before it slides away)
};

}  // namespace ui
