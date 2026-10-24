// Written for Demon's Souls (docs/102, section 9). The HUD editor (Settings, Game, Edit HUD): the game's picture with a
// box over each part of the HUD. Drag a box to move its part, a corner to resize it (its shape kept), the eye to hide
// or show it; Reset puts all four back. Each part stays on screen. The parts' boxes and the canvas come from the game
// (des_hud.cpp: des_hud_edit_view, des_hud_edit_place), the moves go to the HUD's settings (DES_HUD_<PART>_POS and
// _SIZE, and DES_HUD_<PART> for the eye).
#pragma once

#include "document.hpp"

#include <array>

namespace ui {

class HudEditor : public Document {
public:
    HudEditor();

    void show() override;
    void update() override;
    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    enum class Drag { None, Move, Resize };
    struct Part {
        Rml::Element* box = nullptr;
        Rml::Element* eye = nullptr;
        float dx = 0.f, dy = 0.f;  // DES_HUD_<PART>_POS
        float size = 1.f;          // DES_HUD_<PART>_SIZE / 100
        bool shown = true;         // DES_HUD_<PART>
        bool locked = false;       // its move or size set from outside (a pinned setting): left alone
    };

    void read_settings();
    void apply_live(int part);  // while dragging: to the game only
    void save(int part);        // when the drag ends: to the settings
    void place_boxes();
    void begin_drag(int part, Drag mode, int corner, float mx, float my);
    void drag_to(float mx, float my);
    void end_drag();
    void toggle_shown(int part);
    void reset();
    void close();
    void set_focus(int index);
    float to_canvas_x(float px) const;
    float to_canvas_y(float py) const;

    std::array<Part, 4> mParts;
    Rml::Element* mShield = nullptr;  // over everything while dragging: holds the drag's cursor
    Rml::Element* mNote = nullptr;
    Rml::Element* mReset = nullptr;
    Rml::Element* mDone = nullptr;
    int mFocus = 1;  // 0 Reset, 1 Done (keyboard and gamepad)

    // The picture in the window (px) and the canvas it shows (the game's units), from the last update.
    float mPic[4] = {};
    float mView[4] = {0.f, 0.f, 1280.f, 720.f};
    bool mHud = false;

    Drag mDrag = Drag::None;
    int mDragPart = -1;
    int mCorner = 0;  // the corner dragged: 0 top left, 2 bottom right, 3 bottom left
    float mStartMouse[2] = {};
    float mStartDx = 0.f, mStartDy = 0.f, mStartSize = 1.f;
    float mStartBox[4] = {};  // the part's box when the drag began, canvas units
};

}  // namespace ui
