// Adapted from Dusklight (CC0), src/dusk/ui/ui.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls.
#include "ui.hpp"

#include "host.hpp"
#include "input.hpp"
#include "prelaunch.hpp"
#include "settings.hpp"
#include "window.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <utility>

namespace ui {
namespace {

bool sInitialized = false;
std::vector<std::unique_ptr<Document>> sDocumentStack;
// Documents that don't participate in the focus stack
std::vector<std::unique_ptr<Document>> sPassiveDocuments;

std::deque<Toast> sToasts;
bool sMenuNotificationRequested = false;
}  // namespace

bool initialize() noexcept {
    if (sInitialized) {
        return true;
    }
    if (!host::is_initialized()) {
        return false;
    }
    apply_scale();
    sInitialized = true;
    return true;
}

void shutdown() noexcept {
    sDocumentStack.clear();
    sPassiveDocuments.clear();
    input::reset_input_state();
    input::release_input_block();
    sInitialized = false;
}

Document& push_document(std::unique_ptr<Document> doc, bool show, bool passive) noexcept {
    Document& ret = *doc;
    if (passive) {
        sPassiveDocuments.push_back(std::move(doc));
    } else {
        sDocumentStack.push_back(std::move(doc));
    }
    if (show) {
        ret.show();
    }
    input::sync_input_block();
    return ret;
}

Document& detail::pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure) {
    Document* destination = nullptr;
    size_t destinationIndex = 0;
    for (size_t i = sDocumentStack.size(); i > 0; --i) {
        auto& document = *sDocumentStack[i - 1];
        if (!document.closed() && !document.pending_close() && matches(document)) {
            destination = &document;
            destinationIndex = i - 1;
            break;
        }
    }

    if (destination != nullptr) {
        std::vector<Document*> closing;
        for (size_t i = sDocumentStack.size(); i > destinationIndex + 1; --i) {
            closing.push_back(sDocumentStack[i - 1].get());
        }
        for (auto* document : closing) {
            if (!document->closed() && !document->pending_close()) {
                if (document->visible()) {
                    document->hide(true);
                } else {
                    document->force_hide(true);
                }
            }
        }
        configure(*destination);
    } else {
        auto document = create();
        configure(*document);
        if (auto* current = top_document()) {
            current->cover();
        }
        destination = &push_document(std::move(document), false);
    }

    destination->show();
    destination->focus();
    input::sync_input_block();
    return *destination;
}

void bring_document_to_front(Document& doc) noexcept {
    const auto it = std::ranges::find_if(
        sDocumentStack, [&doc](const auto& entry) { return entry.get() == &doc; });
    if (it == sDocumentStack.end() || std::next(it) == sDocumentStack.end()) {
        return;
    }
    auto entry = std::move(*it);
    sDocumentStack.erase(it);
    sDocumentStack.push_back(std::move(entry));
}

void uncover_top_document() noexcept {
    if (auto* doc = top_document()) {
        doc->uncover();
    }
    input::sync_input_block();
}

Document* find_document(DocumentScope scope) noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (!doc->closed() && doc->scope() == scope) {
            return doc.get();
        }
    }
    return nullptr;
}

void close_all_documents() noexcept {
    for (auto& doc : sDocumentStack) {
        if (!doc->closed()) {
            doc->force_hide(!doc->permanent());
        }
    }
    input::sync_input_block();
}

bool any_document_visible() noexcept {
    return std::any_of(sDocumentStack.begin(), sDocumentStack.end(),
        [](const auto& doc) { return doc && doc->visible() && !doc->pending_close(); });
}

bool is_prelaunch_open() noexcept {
    return std::any_of(sDocumentStack.begin(), sDocumentStack.end(), [](const auto& doc) {
        const auto* prelaunch = dynamic_cast<const Prelaunch*>(doc.get());
        return prelaunch != nullptr && prelaunch->active();
    });
}

bool game_obscured_below(const Document& doc) noexcept {
    for (const auto& entry : sDocumentStack) {
        if (entry.get() == &doc) {
            break;
        }
        if (entry->active() && entry->obscures_game()) {
            return true;
        }
    }
    return false;
}

Document* top_document() noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (doc->active()) {
            return doc.get();
        }
    }
    return nullptr;
}

void update() noexcept {
    if (!host::is_initialized()) {
        return;
    }

    input::update_input();
    const auto update_documents = [](auto& documents) {
        const size_t count = documents.size();
        for (size_t i = 0; i < count && i < documents.size(); ++i) {
            Document* doc = documents[i].get();
            if (doc != nullptr && !doc->closed()) {
                doc->update();
            }
        }
    };
    update_documents(sDocumentStack);
    update_documents(sPassiveDocuments);

    // Remove closed documents
    {
        const auto [first, last] =
            std::ranges::remove_if(sDocumentStack, [](const auto& doc) { return doc->closed(); });
        sDocumentStack.erase(first, last);
    }
    {
        const auto [first, last] = std::ranges::remove_if(
            sPassiveDocuments, [](const auto& doc) { return doc->closed(); });
        sPassiveDocuments.erase(first, last);
    }

    // Keep focus on the highest active document.
    if (host::context() != nullptr) {
        for (auto& doc : std::views::reverse(sDocumentStack)) {
            if (doc->active() && (doc->has_focus() || doc->focus())) {
                break;
            }
        }
    }

    input::sync_input_block();
}

std::string escape(std::string_view str) noexcept {
    std::string result;
    result.reserve(str.size());
    for (const char c : str) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

std::string escape_lines(std::string_view str) noexcept {
    std::string result;
    size_t at = 0;
    while (at <= str.size()) {
        const size_t end = std::min(str.find('\n', at), str.size());
        if (at > 0) result += "<br/>";
        result += escape(str.substr(at, end - at));
        at = end + 1;
    }
    return result;
}

Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateElement(tag));
}

Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateTextNode(text));
}

Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept {
    auto* element = append(parent, tag);
    append_text(element, text);
    return element;
}

void clear_children(Rml::Element* parent) noexcept {
    if (parent == nullptr) {
        return;
    }
    while (parent->GetNumChildren() > 0) {
        parent->RemoveChild(parent->GetFirstChild());
    }
}

void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return;
    }
    if (!text.empty() && parent->GetNumChildren() == 1) {
        if (auto* element = dynamic_cast<Rml::ElementText*>(parent->GetFirstChild())) {
            // RmlUi only dirties layout when the node's text changes.
            element->SetText(text);
            return;
        }
    }
    clear_children(parent);
    if (!text.empty()) {
        append_text(parent, text);
    }
}

void set_display(Rml::Element* element, Rml::Style::Display display) noexcept {
    const Rml::Property value{display};
    const auto* current = element->GetLocalProperty(Rml::PropertyId::Display);
    if (current == nullptr || *current != value) {
        element->SetProperty(Rml::PropertyId::Display, value);
    }
}

NavCommand map_nav_event(const Rml::Event& event) noexcept {
    const auto key = static_cast<Rml::Input::KeyIdentifier>(
        event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
    switch (key) {
    case Rml::Input::KeyIdentifier::KI_UP:
        return NavCommand::Up;
    case Rml::Input::KeyIdentifier::KI_DOWN:
        return NavCommand::Down;
    case Rml::Input::KeyIdentifier::KI_LEFT:
        return NavCommand::Left;
    case Rml::Input::KeyIdentifier::KI_RIGHT:
        return NavCommand::Right;
    case Rml::Input::KeyIdentifier::KI_ESCAPE:
        return NavCommand::Cancel;
    case Rml::Input::KeyIdentifier::KI_RETURN:
    case Rml::Input::KeyIdentifier::KI_NUMPADENTER:
        return NavCommand::Confirm;
    case Rml::Input::KeyIdentifier::KI_F1:
        return event.GetParameter<int>("shift_key", 0) ? NavCommand::None : NavCommand::Menu;
    case Rml::Input::KeyIdentifier::KI_NEXT:
        return NavCommand::Next;
    case Rml::Input::KeyIdentifier::KI_PRIOR:
        return NavCommand::Previous;
    default:
        return NavCommand::None;
    }
}

// The game's window has no safe area (no notch, no TV overscan to avoid).
Insets safe_area_insets(Rml::Context*) noexcept {
    return {};
}

void push_toast(Toast toast) noexcept {
    sToasts.push_back(std::move(toast));
}

std::deque<Toast>& get_toasts() noexcept {
    return sToasts;
}

void show_menu_notification() noexcept {
    sMenuNotificationRequested = true;
}

bool consume_menu_notification_request() noexcept {
    const bool requested = sMenuNotificationRequested;
    sMenuNotificationRequested = false;
    return requested;
}


// The UI scale setting: 0 is Auto (the display's scale), otherwise a percentage of it.
void apply_scale() noexcept {
    const int userScale = settings::get_int("ui_scale");
    host::set_ui_scale(userScale > 0 ? static_cast<float>(userScale) / 100.0f : 0.0f);
}

}  // namespace ui
