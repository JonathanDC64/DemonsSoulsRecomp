// The game's on-screen keyboard, drawn by the menus (see osk_window.hpp). The window is the small modal window, with
// the prompt, the text and a counter, the keys in rows, and a line on the buttons. The D-pad moves between keys (to
// the nearest key in the next row), Cross types the key, Circle deletes, Triangle is a space, Square is Shift, Start
// is Done and Select is Cancel. On the keyboard, typing goes straight into the text; Enter is Done, Esc is Cancel.
#include "osk_window.hpp"

#include "ui.hpp"

#include <windows.h>

#include <cmath>

namespace ui {
namespace {

OskWindow* sActive = nullptr;

enum Action { kNone, kShift, kSpace, kDelete, kCancel, kDone };

// CELL_PAD_CTRL_* (cellPad.h)
constexpr unsigned kSelect = 1u << 0, kTriangle = 1u << 12, kSquare = 1u << 15;

// Each row: the keys, each its character and its shifted one.
constexpr const char* kRows[] = {
    "1!2@3#4$5%6^7&8*9(0)-_",
    "qQwWeErRtTyYuUiIoOpP'\"",
    "aAsSdDfFgGhHjJkKlL.:,;",
    "zZxXcCvVbBnNmM??&&//",
};

std::string utf8(const std::u16string& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), int(s.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), int(s.size()), out.data(), n,
                            nullptr, nullptr);
    return out;
}

float centre_x(Rml::Element* e) {
    return e->GetAbsoluteOffset(Rml::BoxArea::Border).x + e->GetOffsetWidth() * 0.5f;
}

}  // namespace

OskWindow* active_osk() noexcept {
    return sActive;
}

OskWindow::OskWindow(Props props) : WindowSmall("modal"), mProps(std::move(props)), mText(mProps.text) {
    mRoot->SetClass("osk", true);
    if (mText.size() > mProps.limit) mText.resize(mProps.limit);

    auto* header = append(mDialog, "modal-header");
    append_text(append(header, "modal-title"), mProps.prompt.empty() ? Rml::String("Enter Text") : mProps.prompt);

    auto* field = append(mDialog, "osk-field");
    mTextElem = append(field, "osk-text");
    mCountElem = append(field, "osk-count");

    build_keys();

    auto* hint = append(mDialog, "osk-hint");
    append_text(hint, "Cross types, Circle deletes, Triangle: space, Square: Shift, Start: Done, Select: Cancel. "
                      "Or type on the keyboard: Enter is Done, Esc is Cancel.");

    refresh();
    sActive = this;
    play_sound(kSoundWindowOpen);
}

OskWindow::~OskWindow() {
    if (sActive == this) sActive = nullptr;
}

void OskWindow::build_keys() {
    auto* keys = append(mDialog, "osk-keys");
    int r = 0;
    for (const char* row : kRows) {
        auto* rowElem = append(keys, "osk-row");
        for (const char* c = row; c[0] && c[1]; c += 2) {
            mKeys.push_back({Rml::String(c, 2), kNone, nullptr, r});
        }
        for (Key& key : mKeys) {
            if (key.row != r || key.button) continue;
            auto button = std::make_unique<Button>(rowElem, Rml::String(1, key.chars[0]));
            button->root()->SetClass("osk-key", true);
            key.button = button.get();
            mButtons.push_back(std::move(button));
        }
        r++;
    }
    struct Special {
        const char* label;
        Action action;
        const char* cls;
    };
    constexpr Special kSpecial[] = {
        {"Shift", kShift, "osk-wide"}, {"Space", kSpace, "osk-space"}, {"Delete", kDelete, "osk-wide"},
        {"Cancel", kCancel, "osk-wide"}, {"Done", kDone, "osk-wide"},
    };
    auto* rowElem = append(keys, "osk-row");
    for (const Special& s : kSpecial) {
        auto button = std::make_unique<Button>(rowElem, Rml::String(s.label));
        button->root()->SetClass("osk-key", true);
        button->root()->SetClass(s.cls, true);
        mKeys.push_back({"", s.action, button.get(), r});
        mButtons.push_back(std::move(button));
    }
    for (size_t i = 0; i < mKeys.size(); i++) {
        mKeys[i].button->on_pressed([this, i] {
            mCursor = int(i);
            press(mKeys[i]);
        });
    }
}

void OskWindow::press(const Key& key) {
    switch (key.action) {
    case kShift:
        mShift = !mShift;
        play_sound(kSoundItemChange);
        refresh();
        return;
    case kSpace: type(u' '); return;
    case kDelete: backspace(); return;
    case kCancel: finish(false); return;
    case kDone: finish(true); return;
    default: type(char16_t(static_cast<unsigned char>(key.chars[mShift ? 1 : 0]))); return;
    }
}

void OskWindow::type(char16_t c) {
    if (mDone) return;
    if (mText.size() >= mProps.limit) {
        play_sound(kSoundWarning);
        return;
    }
    mText.push_back(c);
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::backspace() {
    if (mDone || mText.empty()) return;
    mText.pop_back();
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::finish(bool ok) {
    if (mDone) return;
    mDone = true;
    play_sound(ok ? kSoundClick : kSoundWindowClose);
    if (sActive == this) sActive = nullptr;
    if (mProps.onDone) mProps.onDone(ok, mText);
    pop();
}

void OskWindow::close_silently() {
    if (mDone) return;
    mDone = true;
    if (sActive == this) sActive = nullptr;
    pop();
}

void OskWindow::refresh() {
    set_text_content(mTextElem, utf8(mText) + "_");
    set_text_content(mCountElem, std::to_string(mText.size()) + " / " + std::to_string(mProps.limit));
    for (Key& key : mKeys) {
        if (key.action == kNone) key.button->set_text(Rml::String(1, key.chars[mShift ? 1 : 0]));
        if (key.action == kShift) key.button->set_selected(mShift);
    }
}

void OskWindow::move(int dx, int dy) {
    const Key& at = mKeys[size_t(mCursor)];
    int next = mCursor;
    if (dx != 0) {
        const int i = mCursor + dx;
        if (i >= 0 && i < int(mKeys.size()) && mKeys[size_t(i)].row == at.row) next = i;
    } else {
        const int row = at.row + dy;
        const float x = centre_x(at.button->root());
        float best = 1e9f;
        for (size_t i = 0; i < mKeys.size(); i++) {
            if (mKeys[i].row != row) continue;
            const float d = std::fabs(centre_x(mKeys[i].button->root()) - x);
            if (d < best) {
                best = d;
                next = int(i);
            }
        }
    }
    if (next != mCursor) {
        mCursor = next;
        mKeys[size_t(mCursor)].button->focus();
        play_sound(kSoundItemFocus);
    }
}

bool OskWindow::focus() {
    return mKeys[size_t(mCursor)].button->focus();
}

bool OskWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    (void)event;
    switch (cmd) {
    case NavCommand::Up: move(0, -1); return true;
    case NavCommand::Down: move(0, 1); return true;
    case NavCommand::Left: move(-1, 0); return true;
    case NavCommand::Right: move(1, 0); return true;
    case NavCommand::Cancel: backspace(); return true;  // Circle
    case NavCommand::Menu: finish(true); return true;   // Start
    case NavCommand::Next:
    case NavCommand::Previous: return true;
    default: return false;
    }
}

bool OskWindow::on_key(int vk) {
    switch (vk) {
    case VK_RETURN: finish(true); return true;
    case VK_ESCAPE: finish(false); return true;
    case VK_BACK: backspace(); return true;
    case VK_F1: return true;  // not Done by accident
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_TAB: return false;  // the grid
    default: return true;  // the character comes as on_char
    }
}

void OskWindow::on_char(unsigned c) {
    if (c < 0x20 || c == 0x7F || c > 0xFFFF || (c >= 0xD800 && c <= 0xDFFF)) return;
    type(char16_t(c));
}

void OskWindow::on_pad(unsigned buttons) {
    const unsigned pressed = mPadPrimed ? buttons & ~mPadPrev : 0;
    mPadPrev = buttons;
    mPadPrimed = true;
    if (pressed & kTriangle) type(u' ');
    if (pressed & kSquare) {
        mShift = !mShift;
        play_sound(kSoundItemChange);
        refresh();
    }
    if (pressed & kSelect) finish(false);
}

void OskWindow::update() {
    for (const auto& button : mButtons) button->update();
    WindowSmall::update();
}

}  // namespace ui
