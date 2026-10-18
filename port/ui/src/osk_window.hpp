// The game's on-screen keyboard (cellOskDialog, e.g. the character's name), drawn by the menus: the prompt, the text,
// and a grid of keys to pick with the D-pad and Cross. A physical keyboard types into it as well.
#pragma once

#include "button.hpp"
#include "window.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ui {

class OskWindow : public WindowSmall {
public:
    struct Props {
        Rml::String prompt;   // UTF-8
        std::u16string text;  // the initial text
        unsigned limit = 16;  // the most characters the game takes
        std::function<void(bool ok, const std::u16string& text)> onDone;
    };

    explicit OskWindow(Props props);
    ~OskWindow() override;

    void update() override;
    bool focus() override;

    // The physical keyboard (des_ui.cpp): a key going down (true = used up) and a typed character.
    bool on_key(int vk);
    void on_char(unsigned c);
    // The pads' buttons each frame (CELL_PAD_CTRL_*), for the buttons the menus do not use: Square, Triangle, Select.
    void on_pad(unsigned buttons);

    // The game stopped the dialog first: close without answering.
    void close_silently();

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    struct Key {
        Rml::String chars;  // a character key: its character, then its shifted one
        int action = 0;     // or one of the actions (osk_window.cpp)
        Button* button = nullptr;
        int row = 0;
    };

    void build_keys();
    void press(const Key& key);
    void type(char16_t c);
    void backspace();
    void finish(bool ok);
    void refresh();
    void move(int dx, int dy);

    Props mProps;
    std::u16string mText;
    bool mShift = false;
    bool mDone = false;
    std::vector<Key> mKeys;
    std::vector<std::unique_ptr<Button>> mButtons;
    int mCursor = 0;              // the focused key
    unsigned mPadPrev = 0;        // the pad's buttons last frame (Square, Triangle, Select)
    bool mPadPrimed = false;
    Rml::Element* mTextElem = nullptr;
    Rml::Element* mCountElem = nullptr;
};

// The keyboard that is up, or null.
OskWindow* active_osk() noexcept;

}  // namespace ui
