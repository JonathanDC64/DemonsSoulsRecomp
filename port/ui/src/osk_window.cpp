// The game's on-screen keyboard, drawn by the menus (see osk_window.hpp). The window is the small modal window, with
// the prompt, the text and a counter, the keys in rows, and a row of the pad's buttons and what they do. The buttons
// are the PS3 keyboard's: the D-pad moves between keys (to the nearest key in the next row), Cross types the key,
// Square deletes, Triangle is a space, Select is Shift, L1/R1 move the cursor, Start is Done and Circle is Cancel.
// Square and Triangle repeat while held. On the keyboard, typing goes straight into the text; Enter is Done, Esc is
// Cancel, Home/End move the cursor.
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
// A held Square or Triangle: the first repeat, then the rest (ms).
constexpr unsigned long long kRepeatDelay = 400, kRepeatEvery = 60;

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

// One entry of the button row: the pad button (an icon from the icon font, or a label such as START) and its action.
void add_hint(Rml::Element* row, const char* icon, const char* label, const char* action) {
    auto* item = append(row, "osk-hint-item");
    if (icon) {
        append(item, "icon")->SetClass(Rml::String("pad-") + icon, true);
    } else {
        append_text(append(item, "osk-pill"), label);
    }
    append_text(append(item, "osk-hint-action"), action);
}

}  // namespace

OskWindow* active_osk() noexcept {
    return sActive;
}

OskWindow::OskWindow(Props props) : WindowSmall("modal"), mProps(std::move(props)), mText(mProps.text) {
    mRoot->SetClass("osk", true);
    if (mText.size() > mProps.limit) mText.resize(mProps.limit);
    mCaret = mText.size();

    auto* header = append(mDialog, "modal-header");
    append_text(append(header, "modal-title"), mProps.prompt.empty() ? Rml::String("Enter Text") : mProps.prompt);

    auto* field = append(mDialog, "osk-field");
    mTextElem = append(field, "osk-text");
    mCountElem = append(field, "osk-count");

    build_keys();
    build_hints();

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
        {"Shift", kShift, "osk-wide"}, {"Space", kSpace, "osk-space"}, {"Backspace", kDelete, "osk-wide"},
        {"Back", kCancel, "osk-wide"}, {"Enter", kDone, "osk-wide"},
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

void OskWindow::build_hints() {
    auto* row = append(mDialog, "osk-hints");
    // As the PS3's keyboard shows them (and RPCS3's), with the cursor keys added.
    add_hint(row, "circle", nullptr, "Back");
    add_hint(row, "triangle", nullptr, "Space");
    add_hint(row, "square", nullptr, "Backspace");
    add_hint(row, nullptr, "SELECT", "Shift");
    add_hint(row, nullptr, "START", "Enter");
    add_hint(row, nullptr, "L1 R1", "Cursor");
    append_text(append(mDialog, "osk-hint"), "Or type on the keyboard: Enter to finish, Esc to go back.");
}

void OskWindow::press(const Key& key) {
    switch (key.action) {
    case kShift: toggle_shift(); return;
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
    mText.insert(mCaret++, 1, c);
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::backspace() {
    if (mDone || mCaret == 0) return;
    mText.erase(--mCaret, 1);
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::delete_forward() {
    if (mDone || mCaret >= mText.size()) return;
    mText.erase(mCaret, 1);
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::move_caret(int d) {
    const size_t to = d < 0 ? (mCaret > 0 ? mCaret - 1 : 0) : (mCaret < mText.size() ? mCaret + 1 : mCaret);
    if (to == mCaret) return;
    mCaret = to;
    play_sound(kSoundItemFocus);
    refresh();
}

void OskWindow::toggle_shift() {
    mShift = !mShift;
    play_sound(kSoundItemChange);
    refresh();
}

void OskWindow::finish(bool ok) {
    if (mDone) return;
    mDone = true;
    play_sound(ok ? kSoundClick : kSoundWindowClose);
    if (sActive == this) sActive = nullptr;
    if (mProps.onDone) mProps.onDone(ok, mText);
    close();
}

void OskWindow::close_silently() {
    if (mDone) return;
    mDone = true;
    if (sActive == this) sActive = nullptr;
    close();
}

// Not pop(): the game opened the keyboard, not the document below, which was never covered. pop() would uncover it,
// and the menu bar remembers being shown from the last time a window covered it, so it came back up.
void OskWindow::close() {
    hide(true);
    if (Document* below = top_document(); below && below->visible()) below->focus();
}

void OskWindow::refresh() {
    mTextElem->SetInnerRML(escape(utf8(mText.substr(0, mCaret))) + "<osk-caret/>" + escape(utf8(mText.substr(mCaret))));
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
    case NavCommand::Cancel: finish(false); return true;    // Circle
    case NavCommand::Menu: finish(true); return true;       // Start
    case NavCommand::Next: move_caret(1); return true;      // R1, R2
    case NavCommand::Previous: move_caret(-1); return true; // L1, L2
    default: return false;
    }
}

bool OskWindow::on_key(int vk) {
    switch (vk) {
    case VK_RETURN: finish(true); return true;
    case VK_ESCAPE: finish(false); return true;
    case VK_BACK: backspace(); return true;
    case VK_DELETE: delete_forward(); return true;
    case VK_HOME: while (mCaret > 0) move_caret(-1); return true;
    case VK_END: while (mCaret < mText.size()) move_caret(1); return true;
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
    const unsigned long long now = GetTickCount64();
    const unsigned pressed = mPadPrimed ? buttons & ~mPadPrev : 0;  // a button held as the window opened waits
    mPadPrev = buttons;
    mPadPrimed = true;
    if (pressed & kSelect) toggle_shift();
    const auto repeat = [&](unsigned bit, unsigned long long& next, auto&& act) {
        if (!(buttons & bit)) {
            next = 0;
        } else if (pressed & bit) {
            act();
            next = now + kRepeatDelay;
        } else if (next != 0 && now >= next) {
            act();
            next = now + kRepeatEvery;
        }
    };
    repeat(kSquare, mSquareNext, [this] { backspace(); });
    repeat(kTriangle, mTriangleNext, [this] { type(u' '); });
}

void OskWindow::update() {
    for (const auto& button : mButtons) button->update();
    WindowSmall::update();
}

}  // namespace ui
