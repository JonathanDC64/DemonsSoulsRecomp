// The setup screens' shell (see setup_page.hpp).
#include "setup_page.hpp"

#include "host.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ui {
namespace {

const Rml::String kSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/setup.rcss" />
</head>
<body class="setup-page">
    <setup-backdrop/>
    <setup-vignette/>
    <setup-glow/>
    <setup-shell id="shell">
        <setup-rail>
            <setup-brand>
                <setup-brand-name>Demon's Souls</setup-brand-name>
                <setup-brand-rule/>
            </setup-brand>
            <setup-rail-steps id="rail"/>
            <setup-rail-space/>
            <setup-rail-foot id="foot"/>
        </setup-rail>
        <setup-main>
            <setup-content id="content">
                <setup-title id="title"/>
                <setup-lede id="lede"/>
            </setup-content>
            <setup-footer id="footer">
                <setup-actions id="actions"/>
                <setup-actions id="actions-end"/>
            </setup-footer>
            <setup-main-space/>
            <setup-hints id="hints"/>
        </setup-main>
    </setup-shell>
    <setup-layer id="layer">
        <setup-question>
            <setup-question-title id="question-title"/>
            <setup-question-text id="question-body"/>
            <setup-actions id="question-actions"/>
        </setup-question>
    </setup-layer>
</body>
</rml>
)RML";

const char* look_class(int look) {
    static const char* const kClasses[] = {"", "primary", "danger", "quiet"};
    return kClasses[look];
}

bool move_to(Button* button) {
    if (!button->focus()) return false;
    play_sound(kSoundItemFocus);
    return true;
}

}  // namespace

SetupPage::SetupPage(const char* pageClass)
    : Document(kSource, false, DocumentScope::Window), mShell(mDocument->GetElementById("shell")),
      mRail(mDocument->GetElementById("rail")), mTitle(mDocument->GetElementById("title")),
      mLede(mDocument->GetElementById("lede")), mContent(mDocument->GetElementById("content")),
      mFooter(mDocument->GetElementById("footer")), mFooterRow(mDocument->GetElementById("actions")),
      mFooterEnd(mDocument->GetElementById("actions-end")), mHints(mDocument->GetElementById("hints")),
      mLayer(mDocument->GetElementById("layer")),
      mQuestionTitle(mDocument->GetElementById("question-title")),
      mQuestionBody(mDocument->GetElementById("question-body")),
      mQuestionActions(mDocument->GetElementById("question-actions")) {
    mDocument->SetClass(pageClass, true);
    mDocument->GetElementById("foot")->SetInnerRML("Version " + escape(kVersion) + "<br/>ps3recomp " +
                                                   escape(kPs3recompVersion));
    // As WindowSmall: the page is hidden once its fade has ended.
    listen(mShell, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mShell && !mShell->HasAttribute("open") && Document::visible())
            Document::hide(mPendingClose);
    });
    play_sound(kSoundWindowOpen);
}

void SetupPage::show() {
    Document::show();
    mCloseFallback.cancel();
    mShell->SetAttribute("open", "");
}

void SetupPage::hide(bool close) {
    mShell->RemoveAttribute("open");
    mPendingClose = close;
    if (!host::animations_enabled()) {
        Document::hide(close);  // (no transition will end: tests turn them off)
    } else {
        mCloseFallback.start();
    }
}

bool SetupPage::visible() const {
    return mShell->HasAttribute("open");
}

void SetupPage::update() {
    if (std::exchange(mRevealFocus, false)) {  // (the last frame's layout has the buttons made before it)
        Rml::Context* context = mDocument->GetContext();
        Rml::Element* focused = context ? context->GetFocusElement() : nullptr;
        if (focused && focused->GetOwnerDocument() == mDocument)
            focused->ScrollIntoView({Rml::ScrollAlignment::Adaptive, Rml::ScrollAlignment::Nearest,
                                     Rml::ScrollBehavior::Smooth, Rml::ScrollParentage::Closest});
    }
    if (mPending) {
        auto action = std::exchange(mPending, {});
        action();
    }
    tick();
    // A line over the buttons while the parts scroll behind them (the last frame's layout).
    mFooter->SetClass("edge", mContent->GetScrollHeight() > mContent->GetClientHeight() + 1.f);
    for (const auto& buttons : nav_lines())
        for (Button* button : buttons) button->update();
    for (const auto& button : mQuestionButtons) button->update();
    if (!mShell->HasAttribute("open") && Document::visible() && mCloseFallback.due()) Document::hide(mPendingClose);
    Document::update();
}

bool SetupPage::button_focused() const {
    Rml::Context* context = mDocument->GetContext();
    Rml::Element* focused = context ? context->GetFocusElement() : nullptr;
    if (!focused || focused->GetOwnerDocument() != mDocument) return false;
    for (const auto& buttons : nav_lines())
        for (Button* button : buttons)
            if (button->contains(focused)) return true;
    for (const auto& button : mQuestionButtons)
        if (button->contains(focused)) return true;
    return false;
}

std::pair<int, int> SetupPage::focus_place() const {
    Rml::Context* context = mDocument->GetContext();
    Rml::Element* focused = context ? context->GetFocusElement() : nullptr;
    if (!focused || focused->GetOwnerDocument() != mDocument) return {-1, -1};
    const auto lines = nav_lines();
    for (int l = 0; l < int(lines.size()); ++l)
        for (int i = 0; i < int(lines[size_t(l)].size()); ++i)
            if (lines[size_t(l)][size_t(i)]->contains(focused)) return {l, i};
    return {-1, -1};
}

bool SetupPage::focus_at(std::pair<int, int> place) {
    const auto lines = nav_lines();
    if (place.first < 0 || place.first >= int(lines.size())) return false;
    const auto& line = lines[size_t(place.first)];
    return place.second >= 0 && place.second < int(line.size()) && line[size_t(place.second)]->focus();
}

bool SetupPage::focus() {
    if (mAsking) {
        for (const auto& button : mQuestionButtons)
            if (button->focus()) return true;
        return false;
    }
    for (const auto& buttons : nav_lines())
        for (Button* button : buttons)
            if (button->focus()) return true;
    return false;
}

void SetupPage::set_rail(const std::vector<RailStep>& steps) {
    clear_children(mRail);
    static const char* const kStates[] = {"", "now", "busy", "done"};
    for (size_t i = 0; i < steps.size(); ++i) {
        const RailStep& s = steps[i];
        auto* row = append(mRail, "setup-rail-step");
        if (*kStates[int(s.state)]) row->SetClass(kStates[int(s.state)], true);
        auto* mark = append(row, "setup-rail-mark");
        if (s.state == RailStep::State::Done) icon(mark, "setup-tick");
        else append_text(mark, std::to_string(i + 1));
        auto* words = append(row, "setup-rail-text");
        text(words, "setup-rail-name", s.name);
        if (!s.sub.empty()) text(words, "setup-rail-sub", s.sub);
    }
}

bool SetupPage::options_chosen() {
    return !settings::get("setup_preset").empty();
}

SetupPage::RailStep SetupPage::options_step(RailStep::State state) {
    return {"Options", "", options_chosen() ? RailStep::State::Done : state};
}

void SetupPage::set_heading(const std::string& title, const std::string& lede) {
    set_text_content(mTitle, title);
    set_text_content(mLede, lede);
    set_display(mLede, lede.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
}

void SetupPage::set_buttons(Buttons& buttons, Rml::Element* parent, std::vector<Action> actions) {
    Rml::Context* context = mDocument ? mDocument->GetContext() : nullptr;
    Rml::Element* focused = context ? context->GetFocusElement() : nullptr;
    int had = -1;
    for (int i = 0; focused && i < int(buttons.size()); ++i)
        if (buttons[size_t(i)]->contains(focused)) had = i;
    buttons.clear();  // (their listeners go with them, before their elements)
    clear_children(parent);
    for (Action& a : actions) {
        auto button = std::make_unique<Button>(parent, a.label);
        button->root()->SetClass("setup-btn", true);
        if (*look_class(int(a.look))) button->root()->SetClass(look_class(int(a.look)), true);
        if (a.onPressed) {
            button->on_pressed([this, fn = std::move(a.onPressed)] {
                play_sound(kSoundClick);
                mPending = fn;
            });
        } else {
            button->set_disabled(true);
        }
        buttons.push_back(std::move(button));
    }
    if (had >= 0 && !mAsking && !(had < int(buttons.size()) && buttons[size_t(had)]->focus())) focus();
    if (had >= 0) reveal_focus();
}

void SetupPage::set_hints(std::initializer_list<Hint> hints) {
    clear_children(mHints);
    for (const Hint& h : hints) {
        auto* item = append(mHints, "setup-hint");
        if (h.icon) append(item, "icon")->SetClass(Rml::String("pad-") + h.icon, true);
        else text(item, "setup-hint-label", h.label);
        text(item, "setup-hint-action", h.action);
    }
}

std::vector<Button*> SetupPage::line(const Buttons& buttons) {
    std::vector<Button*> out;
    for (const auto& b : buttons) out.push_back(b.get());
    return out;
}

void SetupPage::ask(const std::string& title, const std::string& bodyRml, std::vector<Action> actions,
                    size_t focused) {
    set_text_content(mQuestionTitle, title);
    set_question_body(bodyRml);
    if (!mAsking) mQuestionFrom = focus_place();  // (a second question keeps the first one's)
    mAsking = true;
    mLayer->SetAttribute("open", "");
    set_question_actions(std::move(actions), focused);
    play_sound(kSoundWindowOpen);
}

void SetupPage::set_question_body(const std::string& bodyRml) {
    mQuestionBody->SetInnerRML(bodyRml);
}

void SetupPage::set_question_actions(std::vector<Action> actions, size_t focused) {
    mQuestionButtons.clear();
    mQuestionActionsRun.clear();
    clear_children(mQuestionActions);
    for (Action& a : actions) {
        auto button = std::make_unique<Button>(mQuestionActions, a.label);
        button->root()->SetClass("setup-btn", true);
        if (*look_class(int(a.look))) button->root()->SetClass(look_class(int(a.look)), true);
        // The question closes first; the action may open another.
        mQuestionActionsRun.push_back([this, fn = std::move(a.onPressed)] {
            close_question();
            if (fn) fn();
        });
        button->on_pressed([this, run = mQuestionActionsRun.back()] {
            play_sound(kSoundClick);
            mPending = run;
        });
        mQuestionButtons.push_back(std::move(button));
    }
    if (!mQuestionButtons.empty()) mQuestionButtons[std::min(focused, mQuestionButtons.size() - 1)]->focus();
}

void SetupPage::close_question() {
    if (!mAsking) return;
    mAsking = false;
    mLayer->RemoveAttribute("open");
    mQuestionButtons.clear();
    mQuestionActionsRun.clear();
    clear_children(mQuestionActions);
    if (!focus_at(mQuestionFrom)) focus();  // (back on the button that opened it)
}

Rml::Element* SetupPage::icon(Rml::Element* parent, const char* name) {
    auto* e = append(parent, "icon");
    e->SetClass(name, true);
    return e;
}

Rml::Element* SetupPage::panel(Rml::Element* parent, bool stack) {
    auto* e = append(parent, "setup-panel");
    if (stack) e->SetClass("stack", true);
    return e;
}

Rml::Element* SetupPage::found(Rml::Element* parent, const char* mark, const std::string& key,
                               const std::string& value) {
    auto* row = append(parent, "setup-found");
    icon(row, mark);
    auto* words = append(row, "setup-found-text");
    text(words, "setup-found-key", key);
    if (!value.empty()) text(words, "setup-found-value", value);
    return row;
}

Rml::Element* SetupPage::callout(Rml::Element* parent, const char* kind, const std::string& title,
                                 const std::string& line) {
    static const struct {
        const char* kind;
        const char* mark;
    } kMarks[] = {{"err", "setup-err"}, {"warn", "setup-warn"}, {"info", "setup-info"}};
    const char* mark = "setup-info";
    for (const auto& m : kMarks)
        if (!std::strcmp(kind, m.kind)) mark = m.mark;
    auto* box = append(parent, "setup-callout");
    box->SetClass(kind, true);
    icon(box, mark);
    auto* body = append(box, "setup-callout-body");
    text(body, "setup-callout-title", title);
    if (!line.empty()) text(body, "setup-callout-text", line);
    return body;
}

Rml::Element* SetupPage::text(Rml::Element* parent, const char* tag, const std::string& words) {
    return append_text_element(parent, tag, words);
}

Rml::Element* SetupPage::actions_row(Rml::Element* parent) {
    return append(parent, "setup-actions");
}

Rml::Element* SetupPage::footer_row(bool first) {
    mFooter->SetClass("first", first);
    return mFooterRow;
}

bool SetupPage::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (mAsking) {
        switch (cmd) {
        case NavCommand::Cancel:  // the last button: the way out
            play_sound(kSoundWindowClose);
            if (!mQuestionActionsRun.empty()) mPending = mQuestionActionsRun.back();
            return true;
        case NavCommand::Menu:
            return true;
        case NavCommand::Left:
        case NavCommand::Right:
        case NavCommand::Up:
        case NavCommand::Down:
            return move_focus({line(mQuestionButtons)}, event, cmd);
        default:
            return false;
        }
    }
    switch (cmd) {
    case NavCommand::Cancel:
        play_sound(kSoundWindowClose);
        back();
        return true;
    case NavCommand::Menu:
        return true;  // (Start and F1: nothing to show or hide here)
    case NavCommand::Next:
    case NavCommand::Previous:
        return scroll(cmd == NavCommand::Next ? 1 : -1);
    case NavCommand::Left:
    case NavCommand::Right:
    case NavCommand::Up:
    case NavCommand::Down:
        return move_focus(nav_lines(), event, cmd);
    default:
        return false;
    }
}

bool SetupPage::move_focus(const std::vector<std::vector<Button*>>& lines, Rml::Event& event, NavCommand cmd) {
    auto* target = event.GetTargetElement();
    int at = -1, index = 0;
    for (int l = 0; at < 0 && l < int(lines.size()); ++l) {
        for (int i = 0; i < int(lines[size_t(l)].size()); ++i) {
            if (!lines[size_t(l)][size_t(i)]->contains(target)) continue;
            at = l;
            index = i;
            break;
        }
    }
    if (at < 0) return focus();
    if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
        const auto& here = lines[size_t(at)];
        const int d = cmd == NavCommand::Right ? 1 : -1;
        for (int i = index + d; i >= 0 && i < int(here.size()); i += d)
            if (move_to(here[size_t(i)])) return true;
        return true;
    }
    const int d = cmd == NavCommand::Down ? 1 : -1;
    for (int l = at + d; l >= 0 && l < int(lines.size()); l += d) {
        const auto& next = lines[size_t(l)];
        if (next.empty()) continue;
        // The button in the same place in that line first, then the others.
        if (move_to(next[std::min(size_t(index), next.size() - 1)])) return true;
        for (Button* button : next)
            if (move_to(button)) return true;
    }
    return true;
}

}  // namespace ui
