// Adapted from Dusklight (CC0), src/dusk/ui/menu_bar.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: the
// menu bar over the game has Settings and Quit (no report-an-issue).
#include "menu_bar.hpp"

#include "app.hpp"
#include "host.hpp"
#include "modal.hpp"
#include "settings_window.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <RmlUi/Core.h>

#include <cmath>

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/tabbing.rcss" />
    <link type="text/rcss" href="res/rml/popup.rcss" />
</head>
<body>
    <popup id="popup" />
</body>
</rml>
)RML";

bool sQuitConfirmOpen = false;

}  // namespace

// The Quit tab, the window's close button and the start screen's Quit all ask the same question.
void request_quit_confirm() {
    if (sQuitConfirmOpen) {
        return;
    }
    sQuitConfirmOpen = true;
    const auto dismiss = [](Modal& modal) {
        sQuitConfirmOpen = false;
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Quit Game",
        .bodyText = "Return to the desktop? Progress since the game last saved will be lost.",
        .actions =
            {
                ModalAction{
                    .label = "Cancel",
                    .onPressed =
                        [dismiss](Modal& modal) {
                            play_sound(kSoundWindowClose);
                            dismiss(modal);
                        },
                },
                ModalAction{
                    .label = "Quit Game",
                    .onPressed =
                        [dismiss](Modal& modal) {
                            play_sound(kSoundClick);
                            dismiss(modal);
                            if (app().quit) {
                                app().quit();
                            }
                        },
                },
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "question-mark",
    }));
}

MenuBar::MenuBar()
    : Document(kDocumentSource, false, DocumentScope::MenuBar),
      mRoot(mDocument->GetElementById("popup")) {
    mTabBar = std::make_unique<TabBar>(mRoot, TabBar::Props{
                                                  .onClose =
                                                      [this] {
                                                          play_sound(kSoundMenuClose);
                                                          hide(false);
                                                      },
                                                  .autoSelect = false,
                                              });

    // Hide document after transition completion
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });

    build_tabs();
}

void MenuBar::build_tabs() {
    mTabBar->add_tab("Settings", [this] { push(std::make_unique<SettingsWindow>()); });

    mTabBar->add_tab("Quit", [this] {
        mTabBar->set_active_tab(-1);
        request_quit_confirm();
    });
}

void MenuBar::show() {
    Document::show();
    mRoot->SetAttribute("open", "");
    mTabBar->set_active_tab(-1);
    if (!mTabBar->focus_tab(mFocusedTabTitle)) {
        mTabBar->focus();
    }
}

void MenuBar::hide(bool close) {
    mFocusedTabTitle = mTabBar->focused_tab_title();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
    }
    if (!host::animations_enabled()) {
        // No transition will end (tests turn them off): hide right away.
        Document::hide(close);
    }
}

void MenuBar::update() {
    update_safe_area();
    Document::update();
}

void MenuBar::update_safe_area() noexcept {
    if (mDocument == nullptr || mTabBar == nullptr) {
        return;
    }

    Rml::Context* context = mDocument->GetContext();
    Insets safeInsets = safe_area_insets(context);
    safeInsets = {
        0.0f,
        std::round(safeInsets.right),
        0.0f,
        std::round(safeInsets.left),
    };
    if (safeInsets == mTabBarPadding) {
        return;
    }

    mTabBarPadding = safeInsets;
    auto* tabBar = mTabBar->root();
    tabBar->SetProperty(
        Rml::PropertyId::PaddingRight, Rml::Property(safeInsets.right, Rml::Unit::PX));
    tabBar->SetProperty(
        Rml::PropertyId::PaddingLeft, Rml::Property(safeInsets.left, Rml::Unit::PX));
    if (auto* close = tabBar->QuerySelector("close")) {
        close->SetProperty(Rml::PropertyId::Right,
            Rml::Property(safeInsets.right + 8.0f * context->GetDensityIndependentPixelRatio(),
                Rml::Unit::PX));
    }
}

bool MenuBar::visible() const {
    return mRoot->HasAttribute("open");
}

bool MenuBar::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel && visible()) {
        play_sound(kSoundMenuClose);
        hide(false);
        return true;
    }
    return Document::handle_nav_command(event, cmd);
}

bool MenuBar::focus() {
    return mTabBar->focus();
}

void MenuBar::refresh_tabs() {
    auto* menuBar = static_cast<MenuBar*>(find_document(DocumentScope::MenuBar));
    if (menuBar == nullptr) {
        return;
    }
    const auto focusedTitle = menuBar->mTabBar->focused_tab_title();
    if (!focusedTitle.empty()) {
        menuBar->mFocusedTabTitle = focusedTitle;
    }
    menuBar->mTabBar->clear_tabs();
    menuBar->build_tabs();
    if (menuBar->visible() && !menuBar->mTabBar->focus_tab(menuBar->mFocusedTabTitle)) {
        menuBar->mTabBar->focus();
    }
}

}  // namespace ui
