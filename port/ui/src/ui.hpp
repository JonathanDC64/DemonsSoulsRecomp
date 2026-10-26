// Adapted from Dusklight (CC0), src/dusk/ui/ui.hpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls.
#pragma once

#include <RmlUi/Core.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "nav_types.hpp"
#include "sound.hpp"

namespace ui {
class Document;

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using clock = std::chrono::steady_clock;

enum class DocumentScope : u8 {
    None,
    Prelaunch,
    Window,
    MenuBar,
    Overlay,
};

struct Toast {
    Rml::String type;
    Rml::String title;
    Rml::String content;
    clock::duration duration;
    Rml::String icon;  // RML for an icon left of the title and the message (an <img> or an <icon>), or none
};

struct Insets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    bool operator==(const Insets& other) const noexcept {
        return top == other.top && right == other.right && bottom == other.bottom &&
               left == other.left;
    }
};

// Loads nothing the host has not: the context must exist (ui::host::init).
bool initialize() noexcept;
void shutdown() noexcept;

void update() noexcept;

Document& push_document(
    std::unique_ptr<Document> doc, bool show = true, bool passive = false) noexcept;

namespace detail {
Document& pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure);
}

template <typename T, typename Configure, typename... Args>
T& pop_to_or_push(Configure&& configure, Args&&... args) {
    return static_cast<T&>(detail::pop_to_or_push_document(
        [](Document& document) { return dynamic_cast<T*>(&document) != nullptr; },
        [&]() -> std::unique_ptr<Document> {
            return std::make_unique<T>(std::forward<Args>(args)...);
        },
        [&](Document& document) {
            std::invoke(std::forward<Configure>(configure), static_cast<T&>(document));
        }));
}

void bring_document_to_front(Document& doc) noexcept;
void uncover_top_document() noexcept;
Document* find_document(DocumentScope scope) noexcept;
void close_all_documents() noexcept;
bool any_document_visible() noexcept;
bool is_prelaunch_open() noexcept;
bool game_obscured_below(const Document& doc) noexcept;
Document* top_document() noexcept;

std::string escape(std::string_view str) noexcept;
// escape() with each line break as <br/> (RML shows a line break as a space).
std::string escape_lines(std::string_view str) noexcept;
Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept;
Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept;
Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept;
void clear_children(Rml::Element* parent) noexcept;
void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept;
void set_display(Rml::Element* element, Rml::Style::Display display) noexcept;

NavCommand map_nav_event(const Rml::Event& event) noexcept;
Insets safe_area_insets(Rml::Context* context) noexcept;

void push_toast(Toast toast) noexcept;

// Asks "Quit?" in a window of its own (the menu's Quit tab, the start screen's Quit and the window's close button).
// Does nothing if it is already asking.
void request_quit_confirm();
std::deque<Toast>& get_toasts() noexcept;
// A line from the online server (its banners) for the thin bar at the top of the screen, which shows them one at a
// time, the oldest first. Plain text, never markup.
void push_ticker(Rml::String text) noexcept;
std::deque<Rml::String>& get_tickers() noexcept;
void show_menu_notification() noexcept;
bool consume_menu_notification_request() noexcept;

// The UI's size from the settings (Auto, or 50-200%).
void apply_scale() noexcept;

}  // namespace ui
