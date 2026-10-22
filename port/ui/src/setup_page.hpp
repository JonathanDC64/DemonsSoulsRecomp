// Written for Demon's Souls. The setup screens' shell (docs/89): the whole window, as the setup design. A rail on the
// left with the game's name, setup's steps (Your game, Build, Options, Play) and the version; the page on the right
// with a heading, the line under it, the page's own parts and its buttons, and the pad's buttons at the bottom. Only
// the parts scroll: the buttons stay in view. The page scales with the window's height (720 px high is 1x; host.hpp).
// A question (Stop the build?) opens over the page in the same document, the page dimmed behind it. The parts' looks
// are in setup.rcss.
//
// The pad: Left and Right move along a line of buttons, Up and Down to the next line, Cross presses, Circle goes back
// (back()) or answers a question with its last button; Start and F1 do nothing here. A button's action runs in
// update(), since it may rebuild the buttons it was pressed on.
#pragma once

#include "button.hpp"
#include "document.hpp"
#include "window.hpp"

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ui {

class SetupPage : public Document {
public:
    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;
    bool obscures_game() const override { return true; }
    bool scales_with_window() const override { return true; }

protected:
    enum class Look { Plain, Primary, Danger, Quiet };
    struct Action {
        Rml::String label;
        std::function<void()> onPressed;  // none: shown, but it cannot be pressed
        Look look = Look::Plain;
    };
    struct Hint {
        const char* icon;    // a pad button from the icon font ("cross"), or null for a label
        const char* label;   // the label when there is no icon ("L1 R1")
        const char* action;  // what it does
    };
    // A step on the rail, as it stands.
    struct RailStep {
        enum class State { Todo, Now, Busy, Done };
        const char* name;
        std::string sub;  // the line under it ("40%, about 3 minutes left"); empty: none
        State state = State::Todo;
    };
    using Buttons = std::vector<std::unique_ptr<Button>>;

    explicit SetupPage(const char* pageClass);

    void set_rail(const std::vector<RailStep>& steps);
    // The options were picked after a build (setup_preset, setup_options_window.hpp), and the rail's Options step:
    // done then, else `state`.
    static bool options_chosen();
    static RailStep options_step(RailStep::State state = RailStep::State::Todo);
    void set_heading(const std::string& title, const std::string& lede);
    // The page's own parts go here, under the heading. They scroll when they do not fit.
    Rml::Element* content() const { return mContent; }
    // The page's own buttons (Continue, Back, ...) go here, under the parts: they stay in view when the parts scroll.
    // footer_row() is the row for them, footer_end() a second one at its right end; set_buttons empties either. `first`:
    // straight under the heading, for a page with no parts.
    Rml::Element* footer_row(bool first = false);
    Rml::Element* footer_end() const { return mFooterEnd; }

    // `actions` as buttons in `parent`, in place of `buttons` (the old ones' listeners go before their elements). If
    // one of the old ones had the focus, the one in its place gets it.
    void set_buttons(Buttons& buttons, Rml::Element* parent, std::vector<Action> actions);
    // Scrolls the focused button into view on the next frame: a button made this frame has no place in the layout
    // until the frame is drawn, so focusing it scrolls nowhere.
    void reveal_focus() { mRevealFocus = true; }
    // One of the page's buttons has the focus. After a redraw, test this, not has_focus(): when the focused button
    // is removed, RmlUi leaves the focus on its parent, still in this document, and the next key only refocuses.
    bool button_focused() const;
    // Where the focus is among nav_lines(), as {line, index} ({-1, -1}: on none of them), and the focus put back on
    // the button now in that place, if there is one.
    std::pair<int, int> focus_place() const;
    bool focus_at(std::pair<int, int> place);
    // The row of the pad's buttons at the bottom of the page.
    void set_hints(std::initializer_list<Hint> hints);
    static std::vector<Button*> line(const Buttons& buttons);

    // A question over the page: a title, a line (RML) and its buttons, `focused` with the focus (the safe one). Circle
    // presses the last button. An action closes the question first, then runs.
    void ask(const std::string& title, const std::string& bodyRml, std::vector<Action> actions, size_t focused = 0);
    void set_question_body(const std::string& bodyRml);
    void set_question_actions(std::vector<Action> actions, size_t focused = 0);
    void close_question();
    bool asking() const { return mAsking; }

    // The parts (setup.rcss), each appended to `parent`.
    static Rml::Element* icon(Rml::Element* parent, const char* name);
    static Rml::Element* panel(Rml::Element* parent, bool stack = false);
    static Rml::Element* found(Rml::Element* parent, const char* mark, const std::string& key,
                               const std::string& value);
    static Rml::Element* callout(Rml::Element* parent, const char* kind, const std::string& title,
                                 const std::string& text);
    static Rml::Element* text(Rml::Element* parent, const char* tag, const std::string& text);
    static Rml::Element* actions_row(Rml::Element* parent);  // a row of buttons among the parts

    // The buttons as the pad moves through them: lines from the top, each from the left.
    virtual std::vector<std::vector<Button*>> nav_lines() const = 0;
    virtual void tick() {}                      // once a frame, after a pressed button's action
    virtual void back() = 0;                    // Circle (Esc)
    virtual bool scroll(int) { return false; }  // L1 and R1 (Page Up and Page Down)
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

    std::function<void()> mPending;

private:
    bool move_focus(const std::vector<std::vector<Button*>>& lines, Rml::Event& event, NavCommand cmd);

    Rml::Element* mShell = nullptr;
    Rml::Element* mRail = nullptr;
    Rml::Element* mTitle = nullptr;
    Rml::Element* mLede = nullptr;
    Rml::Element* mContent = nullptr;
    Rml::Element* mFooter = nullptr;
    Rml::Element* mFooterRow = nullptr;
    Rml::Element* mFooterEnd = nullptr;
    Rml::Element* mHints = nullptr;
    Rml::Element* mLayer = nullptr;
    Rml::Element* mQuestionTitle = nullptr;
    Rml::Element* mQuestionBody = nullptr;
    Rml::Element* mQuestionActions = nullptr;
    Buttons mQuestionButtons;
    std::vector<std::function<void()>> mQuestionActionsRun;  // each button's, the question closed first
    bool mAsking = false;
    std::pair<int, int> mQuestionFrom{-1, -1};  // where the focus was when the question opened
    bool mRevealFocus = false;
    CloseFallback mCloseFallback;
};

}  // namespace ui
