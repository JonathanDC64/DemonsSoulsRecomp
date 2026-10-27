// Adapted from Dusklight (CC0), src/dusk/ui/overlay.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls: toasts
// and the "open the menu" hint (no spell radial).
#include "overlay.hpp"

#include "game_menu.hpp"
#include "host.hpp"
#include "input.hpp"
#include "settings.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/global.rcss" />
    <link type="text/rcss" href="res/rml/overlay.rcss" />
</head>
<body>
</body>
</rml>
)RML";

constexpr auto kMenuNotificationDuration = std::chrono::milliseconds(2500);
// A closing toast (or the menu hint) goes when its transition ends, and this long after its time at the latest: while
// one is up the UI stays awake (Overlay::busy), so a transition that never ends must not keep it so.
constexpr auto kCloseTimeout = std::chrono::milliseconds(1000);

// The bar at the top. A text that fits holds still for kTickerStillSeconds. A longer one holds at its start, scrolls
// to its end and holds there, twice (once if one pass is long), and stays at least kTickerStillSeconds.
constexpr float kTickerStillSeconds = 12.0f;
constexpr float kTickerHoldStart = 2.0f;
constexpr float kTickerHoldEnd = 2.5f;
constexpr float kTickerSpeed = 110.0f;     // dp a second: slow enough to read while playing
constexpr float kTickerLongPass = 30.0f;   // (seconds) a pass this long is shown once
constexpr float kTickerLayoutWait = 2.0f;  // (seconds) never laid out by then: dropped

Rml::Element* create_toast(Rml::Element* parent, const Toast& toast) {
    auto* elem = append(parent, "toast");
    if (!toast.type.empty()) {
        elem->SetClass(toast.type, true);
    }
    // With an icon: toast > toast-row > (toast-icon, toast-body > heading, message), then the progress bar.
    Rml::Element* body = elem;
    if (!toast.icon.empty()) {
        auto* row = append(elem, "toast-row");
        append(row, "toast-icon")->SetInnerRML(toast.icon);
        body = append(row, "toast-body");
    }
    {
        auto* heading = append(body, "heading");
        if (toast.title.starts_with("<")) {
            heading->SetInnerRML(toast.title);
        } else {
            append_text(append(heading, "toast-title"), toast.title);
        }
        if (toast.type == "warning") {
            auto* icon = append(heading, "icon");
            icon->SetClass("warning", true);
        }
    }
    {
        auto* message = append(body, "message");
        if (toast.content.starts_with("<")) {
            message->SetInnerRML(toast.content);
        } else {
            append_text(append(message, "toast-message-text"), toast.content);
        }
    }
    {
        auto* progress = append(elem, "progress");
        progress->SetAttribute("value", 1.f);
    }
    return elem;
}

// The glow's fade out (overlay.rcss), and a little more.
constexpr auto kGlowFade = std::chrono::milliseconds(200);
// The glow reaches this far round the part's box (px of the window: the game's boxes are its pieces' own).
constexpr float kGlowPad = 3.0f;

// The bar at the top: ticker > (ticker-label, ticker-track > ticker-text). The server's words, as text.
Rml::Element* create_ticker(Rml::Element* parent, const Rml::String& text) {
    auto* elem = append(parent, "ticker");
    append_text(append(elem, "ticker-label"), "Notice");
    append_text(append(append(elem, "ticker-track"), "ticker-text"), text);
    return elem;
}

// "Press F1 or (gamepad) L3 + R3 to open the menu" (the chord chosen in Settings, Interface).
Rml::Element* create_menu_notification(Rml::Element* parent) {
    auto* elem = append(parent, "toast");
    elem->SetClass("menu-notification", true);

    const Rml::String padButton = input::chord_label(settings::get_int("pad_menu_chord"));

    auto* message = append(elem, "message");
    auto* row = append(message, "row");
    auto* prefix = append(row, "notification-prefix");
    append_text(prefix, "Press ");
    append_text(append(prefix, "b"), "F1");
    append_text(prefix, " or");
    auto* icon = append(row, "icon");
    icon->SetClass("controller", true);
    append_text(append(append(row, "notification-button"), "b"), padButton);
    append_text(append(row, "notification-action"), "to open the menu");

    return elem;
}

void remove_element(Rml::Element*& elem) noexcept {
    if (elem == nullptr) {
        return;
    }
    if (auto* parent = elem->GetParentNode()) {
        parent->RemoveChild(elem);
    }
    elem = nullptr;
}

}  // namespace

Overlay::Overlay() : Document(kDocumentSource, true, DocumentScope::Overlay) {
    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mCurrentToast) {
            if (get_toasts().empty() ||
                clock::now() >= mCurrentToastStartTime + get_toasts().front().duration)
            {
                mCurrentToast->SetPseudoClass("done", true);
            }
        } else if (mMenuNotification != nullptr && event.GetTargetElement() == mMenuNotification &&
                   !mMenuNotification->HasAttribute("open"))
        {
            mMenuNotification->SetPseudoClass("done", true);
        } else if (mTicker != nullptr && event.GetTargetElement() == mTicker && !mTicker->HasAttribute("open")) {
            mTicker->SetPseudoClass("done", true);
        }
    });
}

void Overlay::show() {
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

void Overlay::update() {
    Document::update();
    if (mDocument == nullptr) {
        return;
    }
    // Without transitions (tests) nothing would mark an element done; treat it as done once closed.
    const bool animated = host::animations_enabled();

    if (mMenuNotification != nullptr) {
        if (clock::now() >= mMenuNotificationStartTime + kMenuNotificationDuration) {
            if (!animated || mMenuNotification->IsPseudoClassSet("done") ||
                !mMenuNotification->IsPseudoClassSet("opened") ||
                clock::now() >= mMenuNotificationStartTime + kMenuNotificationDuration + kCloseTimeout)
            {
                remove_element(mMenuNotification);
            } else {
                mMenuNotification->RemoveAttribute("open");
            }
        } else {
            mMenuNotification->SetAttribute("open", "");
            mMenuNotification->SetPseudoClass("opened", true);
            mMenuNotification->SetPseudoClass("done", false);
        }
    }
    if (consume_menu_notification_request()) {
        if (mMenuNotification == nullptr) {
            mMenuNotification = create_menu_notification(mDocument);
        }
        mMenuNotificationStartTime = clock::now();
    }

    auto& toasts = get_toasts();
    if (mCurrentToast == nullptr) {
        if (!toasts.empty()) {
            const auto& toast = toasts.front();
            mCurrentToast = create_toast(mDocument, toast);
            mCurrentToastStartTime = clock::now();
        }
    } else if (!toasts.empty()) {
        const auto& toast = toasts.front();
        const float duration = std::chrono::duration<float>(toast.duration).count();
        const float elapsed =
            std::chrono::duration<float>(clock::now() - mCurrentToastStartTime).count();
        const float ratio = duration > 0.0f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
        const auto remaining = 1.f - ratio;
        if (auto* progress = mCurrentToast->QuerySelector("progress")) {
            progress->SetAttribute("value", remaining);
        }
        if (remaining == 0.f) {
            if (!animated || mCurrentToast->IsPseudoClassSet("done") ||
                // Fallback for large gaps in time where we never actually opened it
                !mCurrentToast->IsPseudoClassSet("opened") ||
                clock::now() >= mCurrentToastStartTime + toast.duration + kCloseTimeout)
            {
                remove_element(mCurrentToast);
                toasts.pop_front();
            } else {
                mCurrentToast->RemoveAttribute("open");
            }
        } else {
            mCurrentToast->SetAttribute("open", "");
            mCurrentToast->SetPseudoClass("opened", true);
        }
    }
    update_ticker();
    update_glow();
}

void Overlay::update_glow() {
    float box[4];
    if (game_menu::glow(box)) {
        if (mGlow == nullptr) mGlow = append(mDocument, "game-glow");
        auto px = [](float v) { return Rml::Property(std::round(v), Rml::Unit::PX); };
        mGlow->SetProperty(Rml::PropertyId::Left, px(box[0] - kGlowPad));
        mGlow->SetProperty(Rml::PropertyId::Top, px(box[1] - kGlowPad));
        mGlow->SetProperty(Rml::PropertyId::Width, px(box[2] - box[0] + 2.0f * kGlowPad));
        mGlow->SetProperty(Rml::PropertyId::Height, px(box[3] - box[1] + 2.0f * kGlowPad));
        mGlow->SetAttribute("open", "");
        mGlowGone = clock::now();
    } else if (mGlow != nullptr) {
        if (!host::animations_enabled() || clock::now() >= mGlowGone + kGlowFade) {
            remove_element(mGlow);
        } else {
            mGlow->RemoveAttribute("open");
        }
    }
}

void Overlay::update_ticker() {
    auto& tickers = get_tickers();
    if (mTicker == nullptr) {
        if (tickers.empty()) {
            return;
        }
        mTicker = create_ticker(mDocument, tickers.front());
        mTickerText = mTicker->QuerySelector("ticker-text");
        mTickerStartTime = clock::now();
        mTickerTrackWidth = -1.0f;
        return;  // (its widths are known once it is laid out, at the next update)
    }
    const bool animated = host::animations_enabled();
    const float elapsed = std::chrono::duration<float>(clock::now() - mTickerStartTime).count();
    auto drop = [&] {
        remove_element(mTicker);
        mTickerText = nullptr;
        if (!tickers.empty()) {
            tickers.pop_front();
        }
    };

    // The plan, made again whenever the track's width changes (the window's size, the UI's scale).
    Rml::Element* track = mTickerText != nullptr ? mTickerText->GetParentNode() : nullptr;
    const float trackWidth = track != nullptr ? track->GetClientWidth() : 0.0f;
    if (trackWidth > 0.0f && trackWidth != mTickerTrackWidth && !tickers.empty()) {
        mTickerTrackWidth = trackWidth;
        Rml::Context* context = mTicker->GetContext();
        const float dp = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0f;
        const float textWidth = float(Rml::ElementUtilities::GetStringWidth(mTickerText, tickers.front()));
        mTickerOverflow = std::max(0.0f, textWidth - trackWidth);
        if (mTickerOverflow > 0.0f) {
            mTickerPassSeconds = kTickerHoldStart + mTickerOverflow / (kTickerSpeed * dp) + kTickerHoldEnd;
            mTickerPasses = mTickerPassSeconds > kTickerLongPass ? 1 : 2;
            mTickerSeconds = std::max(kTickerStillSeconds, mTickerPassSeconds * float(mTickerPasses));
        } else {
            mTickerPasses = 0;
            mTickerSeconds = kTickerStillSeconds;
        }
    }
    if (mTickerTrackWidth < 0.0f) {
        if (elapsed >= kTickerLayoutWait) {
            drop();
        }
        return;
    }

    // Where the text is: at its start, then moving left until its end shows, each pass; at its end after the last.
    float x = 0.0f;
    if (mTickerOverflow > 0.0f) {
        x = -mTickerOverflow;
        if (elapsed < mTickerPassSeconds * float(mTickerPasses)) {
            const float t = std::fmod(elapsed, mTickerPassSeconds);
            const float scroll = mTickerPassSeconds - kTickerHoldStart - kTickerHoldEnd;
            x = -mTickerOverflow * std::clamp((t - kTickerHoldStart) / scroll, 0.0f, 1.0f);
        }
    }
    mTickerText->SetProperty("transform", "translateX(" + std::to_string(std::round(x)) + "px)");

    if (elapsed >= mTickerSeconds) {
        if (!animated || mTicker->IsPseudoClassSet("done") || !mTicker->IsPseudoClassSet("opened") ||
            elapsed >= mTickerSeconds + std::chrono::duration<float>(kCloseTimeout).count())
        {
            drop();
        } else {
            mTicker->RemoveAttribute("open");
        }
    } else {
        mTicker->SetAttribute("open", "");
        mTicker->SetPseudoClass("opened", true);
    }
}

// Something is on screen, or still animating away.
bool Overlay::busy() const {
    return mCurrentToast != nullptr || mMenuNotification != nullptr || !get_toasts().empty() || mTicker != nullptr ||
           !get_tickers().empty() || mGlow != nullptr;
}

bool Overlay::handle_nav_command(Rml::Event&, NavCommand) {
    return false;
}

}  // namespace ui
