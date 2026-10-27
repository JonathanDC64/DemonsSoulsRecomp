// Written for Demon's Souls (docs/102, section 9). The HUD editor; see hud_editor.hpp.
#include "hud_editor.hpp"

#include "settings.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" int des_hud_edit_view(float view[4]);  // des_hud.cpp: the canvas the picture shows; 0 before any HUD
extern "C" int des_hud_edit_place(int part, float* dx, float* dy, float size, float box[4]);  // a part's box there
extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp: through the getenv cache

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/hud_editor.rcss" />
</head>
<body>
    <hud-bar>
        <hud-panel>
            <hud-hint>Drag a box to move it, or a corner to resize it. The eye hides or shows it.</hud-hint>
            <hud-note id="note">The HUD shows once you are in the game.</hud-note>
            <hud-actions>
                <button id="reset" class="overlay">Reset all</button>
                <button id="done" class="overlay primary">Done</button>
            </hud-actions>
        </hud-panel>
    </hud-bar>
    <hud-shield id="shield"></hud-shield>
</body>
</rml>
)RML";

// The parts, in des_hud.cpp's order (kParts there).
struct PartInfo {
    const char* label;
    const char* shown;
    const char* pos;
    const char* size;
};
constexpr PartInfo kInfo[] = {
    {"Health, magic, stamina", "DES_HUD_BARS", "DES_HUD_BARS_POS", "DES_HUD_BARS_SIZE"},
    {"Area name", "DES_HUD_AREA", "DES_HUD_AREA_POS", "DES_HUD_AREA_SIZE"},
    {"Equipment", "DES_HUD_EQUIP", "DES_HUD_EQUIP_POS", "DES_HUD_EQUIP_SIZE"},
    {"Souls", "DES_HUD_SOULS", "DES_HUD_SOULS_POS", "DES_HUD_SOULS_SIZE"},
};

constexpr float kSizeMin = 0.25f, kSizeMax = 3.f;  // des_hud.cpp takes 25 to 300 percent
constexpr float kPad = 6.f;        // px around a part's pieces
constexpr float kNearEdge = 14.f;  // dp: the eye or a corner would leave the window (hud_editor.rcss)

// The value in use: the environment's if it was set from outside (pinned), else the settings'.
std::string value_of(const char* key) {
    return settings::pinned(key) ? settings::pinned_value(key) : settings::get(key);
}

// The picture's shape: RSX_ASPECT's (w:h, WxH or a number, as the D3D12 backend reads it), else the game's 16:9.
float picture_aspect() {
    const char* v = std::getenv("RSX_ASPECT");
    float a = 0.f, b = 0.f;
    if (v && *v) {
        if (std::sscanf(v, "%f:%f", &a, &b) == 2 || std::sscanf(v, "%fx%f", &a, &b) == 2 ||
            std::sscanf(v, "%fX%f", &a, &b) == 2)
        {
            if (a > 0.f && b > 0.f) return a / b;
        } else if (std::sscanf(v, "%f", &a) == 1 && a > 0.f) {
            return a;
        }
    }
    return 16.f / 9.f;
}

// Where the backend shows the picture in the window (picture_rect_of).
void picture_rect(Rml::Context* context, float out[4]) {
    const Rml::Vector2i d = context->GetDimensions();
    picture_rect_of(float(d.x), float(d.y), out);
}

void set_px(Rml::Element* el, Rml::PropertyId id, float v) {
    el->SetProperty(id, Rml::Property(v, Rml::Unit::PX));
}

std::string pos_text(float dx, float dy) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.0f,%.0f", dx, dy);
    return buf;
}

std::string size_text(float size) {
    return std::to_string(int(std::lround(size * 100.f)));
}

}  // namespace

void picture_rect_of(float W, float H, float out[4]) {
    const float A = picture_aspect();
    float w = W, h = W / A;
    if (h > H) {
        h = H;
        w = H * A;
    }
    out[0] = (W - w) * 0.5f;
    out[1] = (H - h) * 0.5f;
    out[2] = out[0] + w;
    out[3] = out[1] + h;
}

HudEditor::HudEditor() : Document(kDocumentSource) {
    if (mDocument == nullptr) {
        return;
    }
    mShield = mDocument->GetElementById("shield");
    mNote = mDocument->GetElementById("note");
    mReset = mDocument->GetElementById("reset");
    mDone = mDocument->GetElementById("done");
    Rml::Element* bar = mDocument->QuerySelector("hud-bar");
    for (size_t i = 0; i < mParts.size(); i++) {
        Part& p = mParts[i];
        // Before the bar, so the bar and the shield stay on top.
        auto box = mDocument->CreateElement("hud-part");
        p.box = mDocument->InsertBefore(std::move(box), bar);
        append_text_element(p.box, "hud-name", kInfo[i].label);
        static const char* const kCorners[] = {"tl", "bl", "br"};
        for (const char* corner : kCorners) {
            append(p.box, "hud-grip")->SetClass(corner, true);
        }
        p.eye = append(p.box, "hud-eye");
        append(p.eye, "icon");
        listen(p.eye, Rml::EventId::Click, [this, i](Rml::Event& event) {
            toggle_shown(int(i));
            event.StopPropagation();
        });
    }
    read_settings();

    listen(Rml::EventId::Mousedown, [this](Rml::Event& event) {
        if (event.GetParameter<int>("button", 0) != 0) {
            return;
        }
        if (mDrag != Drag::None) {  // (the button went up outside the window)
            end_drag();
            return;
        }
        int part = -1, corner = -1;
        bool eye = false;
        for (Rml::Element* el = event.GetTargetElement(); el && part < 0; el = el->GetParentNode()) {
            const Rml::String& tag = el->GetTagName();
            if (tag == "hud-eye") {
                eye = true;
            } else if (tag == "hud-grip" && corner < 0) {
                corner = el->IsClassSet("tl") ? 0 : el->IsClassSet("br") ? 2 : 3;
            }
            for (size_t i = 0; i < mParts.size(); i++) {
                if (el == mParts[i].box) {
                    part = int(i);
                }
            }
        }
        if (part < 0 || eye) {
            return;
        }
        // (Not stopped: RmlUi then starts its drag, whose events reach the box wherever the mouse goes.)
        begin_drag(part, corner < 0 ? Drag::Move : Drag::Resize, corner,
            event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
    });
    listen(Rml::EventId::Drag, [this](Rml::Event& event) {
        if (mDrag != Drag::None) {
            drag_to(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
        }
    });
    listen(Rml::EventId::Dragend, [this](Rml::Event&) {
        if (mDrag != Drag::None) {
            end_drag();
        }
    });
    listen(Rml::EventId::Mouseup, [this](Rml::Event&) {  // (a press with no move: no drag)
        if (mDrag != Drag::None) {
            end_drag();
        }
    });
    listen(mReset, Rml::EventId::Click, [this](Rml::Event&) { reset(); });
    listen(mDone, Rml::EventId::Click, [this](Rml::Event&) { close(); });
}

void HudEditor::show() {
    read_settings();
    Document::show();
}

bool HudEditor::focus() {
    set_focus(mFocus);
    return true;
}

void HudEditor::set_focus(int index) {
    mFocus = index;
    Rml::Element* el = index == 0 ? mReset : mDone;
    if (el != nullptr) {
        el->Focus(true);
    }
}

void HudEditor::read_settings() {
    for (size_t i = 0; i < mParts.size(); i++) {
        Part& p = mParts[i];
        const PartInfo& k = kInfo[i];
        float x = 0.f, y = 0.f;
        if (std::sscanf(value_of(k.pos).c_str(), "%f,%f", &x, &y) != 2 || !std::isfinite(x) || !std::isfinite(y)) {
            x = y = 0.f;
        }
        p.dx = x;
        p.dy = y;
        const int pct = std::atoi(value_of(k.size).c_str());
        p.size = std::clamp(float(pct > 0 ? pct : 100) / 100.f, kSizeMin, kSizeMax);
        p.shown = value_of(k.shown) != "0";
        p.locked = settings::pinned(k.pos) || settings::pinned(k.size) || settings::pinned(k.shown);
    }
}

void HudEditor::update() {
    Document::update();
    if (mDocument == nullptr || !visible()) {
        return;
    }
    picture_rect(mDocument->GetContext(), mPic);
    mHud = des_hud_edit_view(mView) != 0;
    set_display(mNote, mHud ? Rml::Style::Display::None : Rml::Style::Display::Block);
    place_boxes();
}

float HudEditor::to_canvas_x(float px) const {
    const float w = mPic[2] - mPic[0];
    return w > 0.f ? px * (mView[2] - mView[0]) / w : 0.f;
}

float HudEditor::to_canvas_y(float py) const {
    const float h = mPic[3] - mPic[1];
    return h > 0.f ? py * (mView[3] - mView[1]) / h : 0.f;
}

// Each part's box over its pieces, as the game would draw them with the values here.
void HudEditor::place_boxes() {
    const float w = mView[2] - mView[0], h = mView[3] - mView[1];
    Rml::Context* context = mDocument->GetContext();
    const Rml::Vector2i win = context->GetDimensions();
    const float nearEdge = kNearEdge * context->GetDensityIndependentPixelRatio();
    for (size_t i = 0; i < mParts.size(); i++) {
        Part& p = mParts[i];
        float dx = p.dx, dy = p.dy, box[4];
        const bool known = mHud && w > 0.f && h > 0.f && des_hud_edit_place(int(i), &dx, &dy, p.size, box) != 0;
        set_display(p.box, known ? Rml::Style::Display::Block : Rml::Style::Display::None);
        if (!known) {
            continue;
        }
        // (Kept within the picture; the eye and the corners go inside the box on a side near the window's edge.)
        const float sx = (mPic[2] - mPic[0]) / w, sy = (mPic[3] - mPic[1]) / h;
        const float l = std::max(mPic[0], mPic[0] + (box[0] - mView[0]) * sx - kPad);
        const float t = std::max(mPic[1], mPic[1] + (box[1] - mView[1]) * sy - kPad);
        const float r = std::min(mPic[2], mPic[0] + (box[2] - mView[0]) * sx + kPad);
        const float b = std::min(mPic[3], mPic[1] + (box[3] - mView[1]) * sy + kPad);
        p.box->SetClass("edge-left", l < nearEdge);
        p.box->SetClass("edge-top", t < nearEdge);
        p.box->SetClass("edge-right", r > float(win.x) - nearEdge);
        p.box->SetClass("edge-bottom", b > float(win.y) - nearEdge);
        set_px(p.box, Rml::PropertyId::Left, std::round(l));
        set_px(p.box, Rml::PropertyId::Top, std::round(t));
        set_px(p.box, Rml::PropertyId::Width, std::round(r - l));
        set_px(p.box, Rml::PropertyId::Height, std::round(b - t));
        p.box->SetClass("off", !p.shown);
        p.box->SetClass("label-below", t < 2.f * nearEdge);  // (no room above it)
        p.box->SetClass("locked", p.locked);
        p.box->SetClass("dragged", mDrag != Drag::None && mDragPart == int(i));
        if (Rml::Element* icon = p.eye->GetFirstChild()) {
            icon->SetClass("eye-on", p.shown);
            icon->SetClass("eye-off", !p.shown);
        }
    }
}

void HudEditor::begin_drag(int part, Drag mode, int corner, float mx, float my) {
    Part& p = mParts[size_t(part)];
    float dx = p.dx, dy = p.dy, box[4];
    if (p.locked || !mHud || !des_hud_edit_place(part, &dx, &dy, p.size, box)) {
        return;
    }
    mDrag = mode;
    mDragPart = part;
    mCorner = corner;
    mStartMouse[0] = mx;
    mStartMouse[1] = my;
    mStartDx = dx;
    mStartDy = dy;
    mStartSize = p.size;
    std::copy(box, box + 4, mStartBox);
    mShield->SetClass("move", mode == Drag::Move);
    mShield->SetClass("nwse", mode == Drag::Resize && corner != 3);
    mShield->SetClass("nesw", mode == Drag::Resize && corner == 3);
    set_display(mShield, Rml::Style::Display::Block);
}

void HudEditor::drag_to(float mx, float my) {
    Part& p = mParts[size_t(mDragPart)];
    const float cx = to_canvas_x(mx - mStartMouse[0]), cy = to_canvas_y(my - mStartMouse[1]);
    float box[4];
    if (mDrag == Drag::Move) {
        float dx = std::round(mStartDx + cx), dy = std::round(mStartDy + cy);
        if (des_hud_edit_place(mDragPart, &dx, &dy, p.size, box)) {
            p.dx = dx;
            p.dy = dy;
            apply_live(mDragPart);
        }
        return;
    }
    // Resize about the opposite corner: the size from how far along the box's diagonal the dragged corner went.
    const float* b = mStartBox;
    const float fx = mCorner == 2 ? b[0] : b[2], fy = mCorner == 0 ? b[3] : b[1];  // the corner that stays
    const float gx = mCorner == 2 ? b[2] : b[0], gy = mCorner == 0 ? b[1] : b[3];  // the one dragged
    const float vx = gx - fx, vy = gy - fy, len2 = vx * vx + vy * vy;
    if (len2 <= 0.f) {
        return;
    }
    const float k = ((gx + cx - fx) * vx + (gy + cy - fy) * vy) / len2;
    const float size = std::clamp(std::round(mStartSize * k * 100.f) / 100.f, kSizeMin, kSizeMax);
    float zx = 0.f, zy = 0.f, at[4];
    if (!des_hud_edit_place(mDragPart, &zx, &zy, size, at)) {  // (no move: never cut back)
        return;
    }
    const float ax = mCorner == 2 ? at[0] : at[2], ay = mCorner == 0 ? at[3] : at[1];
    const float want_dx = std::round(fx - ax), want_dy = std::round(fy - ay);
    float dx = want_dx, dy = want_dy;
    if (!des_hud_edit_place(mDragPart, &dx, &dy, size, box) || dx != want_dx || dy != want_dy) {
        return;  // it would leave the screen: it stops growing that way
    }
    p.size = size;
    p.dx = dx;
    p.dy = dy;
    apply_live(mDragPart);
}

void HudEditor::end_drag() {
    const int part = mDragPart;
    mDrag = Drag::None;
    mDragPart = -1;
    set_display(mShield, Rml::Style::Display::None);
    if (part >= 0) {
        save(part);
    }
}

// While dragging, each move goes to the game only: the settings file is written once, when the drag ends.
void HudEditor::apply_live(int part) {
    const Part& p = mParts[size_t(part)];
    ps3_setenv(kInfo[part].pos, pos_text(p.dx, p.dy).c_str());
    ps3_setenv(kInfo[part].size, size_text(p.size).c_str());
}

void HudEditor::save(int part) {
    const Part& p = mParts[size_t(part)];
    settings::set(kInfo[part].pos, pos_text(p.dx, p.dy));
    settings::set(kInfo[part].size, size_text(p.size));
}

void HudEditor::toggle_shown(int part) {
    Part& p = mParts[size_t(part)];
    if (p.locked) {
        play_sound(kSoundWarning);
        return;
    }
    p.shown = !p.shown;
    settings::set(kInfo[part].shown, p.shown ? "1" : "0");
    play_sound(p.shown ? kSoundItemEnable : kSoundItemDisable);
}

void HudEditor::reset() {
    for (size_t i = 0; i < mParts.size(); i++) {
        Part& p = mParts[i];
        if (p.locked) {
            continue;
        }
        p.dx = p.dy = 0.f;
        p.size = 1.f;
        p.shown = true;
        settings::set(kInfo[i].pos, pos_text(0.f, 0.f));
        settings::set(kInfo[i].size, size_text(1.f));
        settings::set(kInfo[i].shown, "1");
    }
    play_sound(kSoundClick);
}

void HudEditor::close() {
    if (mDrag != Drag::None) {
        end_drag();
    }
    play_sound(kSoundWindowClose);
    pop();
}

bool HudEditor::handle_nav_command(Rml::Event&, NavCommand cmd) {
    switch (cmd) {
    case NavCommand::Cancel:
    case NavCommand::Menu:
        close();
        return true;
    case NavCommand::Left:
    case NavCommand::Right:
    case NavCommand::Up:
    case NavCommand::Down:
        set_focus(mFocus ^ 1);
        play_sound(kSoundItemFocus);
        return true;
    case NavCommand::Confirm:
        if (mFocus == 0) {
            reset();
        } else {
            close();
        }
        return true;
    default:
        return false;
    }
}

}  // namespace ui
