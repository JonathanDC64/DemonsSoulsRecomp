// Written for Demon's Souls. The UI's connection to the game off Windows (host.hpp; host.cpp is Windows'): RmlUi
// draws through its renderer on the game's D3D12 device and queue (render_d3d12.cpp; the D3D12 backend runs on vkd3d
// there, which has no D3D11On12), over the finished game frame just before Present. The embedded files, the fonts, the
// BMP writer and the PNG store are host.cpp's (the two hosts are to become one, docs/76). Images are decoded with
// stb_image.
#include "host.hpp"

#include "mouse.hpp"
#include "render_d3d12.hpp"
#include "ui_assets.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui::host {
namespace {

// "res/rml/../fonts/x.ttf" -> "res/fonts/x.ttf"
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(pos, slash - pos);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        pos = slash + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

// Serves the embedded UI files to RmlUi.
class EmbeddedFiles final : public Rml::FileInterface {
    struct Handle {
        const Asset* asset;
        size_t pos;
    };

public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const Asset* a = find_asset(normalize(path));
        if (!a) return 0;
        return reinterpret_cast<Rml::FileHandle>(new Handle{a, 0});
    }
    void Close(Rml::FileHandle file) override { delete reinterpret_cast<Handle*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<Handle*>(file);
        const size_t n = std::min(size, f->asset->size - f->pos);
        std::memcpy(buffer, f->asset->data + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<Handle*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->pos) : long(f->asset->size);
        const long p = base + offset;
        if (p < 0 || size_t(p) > f->asset->size) return false;
        f->pos = size_t(p);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<Handle*>(file)->pos; }
};

// RmlUi's system interface: the time, its warnings and errors printed, and the cursor's shape (set on the window's
// thread, mouse_sdl.cpp). The clipboard is RmlUi's own (within the game).
class System final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "[ui] %s\n", message.c_str());
        return true;
    }
    void SetMouseCursor(const Rml::String& name) override {
        int shape = SDL_SYSTEM_CURSOR_ARROW;
        if (name == "pointer") shape = SDL_SYSTEM_CURSOR_HAND;
        else if (name == "text") shape = SDL_SYSTEM_CURSOR_IBEAM;
        else if (name == "move" || name.rfind("rmlui-scroll", 0) == 0) shape = SDL_SYSTEM_CURSOR_SIZEALL;
        else if (name == "resize") shape = SDL_SYSTEM_CURSOR_SIZENWSE;
        else if (name == "cross") shape = SDL_SYSTEM_CURSOR_CROSSHAIR;
        else if (name == "unavailable") shape = SDL_SYSTEM_CURSOR_NO;
        mouse::set_cursor(shape);
    }

private:
    const std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
};

// add_png's images, by source.
std::mutex sPngLock;
std::map<std::string, std::vector<uint8_t>, std::less<>> sPngs;

bool find_png(const Rml::String& source, std::vector<unsigned char>& file) {
    std::lock_guard<std::mutex> lock(sPngLock);
    const auto it = sPngs.find(source);
    if (it == sPngs.end()) return false;
    file = it->second;
    return true;
}

struct State {
    EmbeddedFiles files;
    std::unique_ptr<System> system;
    std::unique_ptr<d3d12::Renderer> renderer;
    Rml::Context* rml = nullptr;
    SDL_Window* window = nullptr;
    float ui_scale = 0.0f;
    bool initialised = false;
};
State s;

void load_fonts() {
    struct Font {
        const char* file;
        const char* family;
        Rml::Style::FontWeight weight;
        bool fallback;
    };
    for (const Font& f : {Font{"res/fonts/AlegreyaSans-Regular.ttf", "Alegreya Sans", Rml::Style::FontWeight::Normal, true},
                          Font{"res/fonts/AlegreyaSans-Bold.ttf", "Alegreya Sans", Rml::Style::FontWeight::Bold, false},
                          Font{"res/fonts/Cinzel.ttf", "Cinzel", Rml::Style::FontWeight::Auto, false},
                          Font{"res/fonts/MaterialSymbolsRounded-Regular.ttf", "Material Symbols Rounded",
                               Rml::Style::FontWeight::Auto, false}}) {
        const Asset* a = find_asset(f.file);
        if (!a || !Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(a->data), a->size}, f.family,
                                     Rml::Style::FontStyle::Normal, f.weight, f.fallback))
            std::fprintf(stderr, "[ui] cannot load font %s\n", f.file);
    }
}

// 32-bit BMP, bottom-up, from RGBA rows.
bool write_bmp(const char* path, const unsigned char* rgba, int width, int height, size_t pitch) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const unsigned image = unsigned(width) * unsigned(height) * 4u;
    unsigned char header[54] = {'B', 'M'};
    auto put32 = [&](int at, unsigned v) {
        header[at] = (unsigned char)v;
        header[at + 1] = (unsigned char)(v >> 8);
        header[at + 2] = (unsigned char)(v >> 16);
        header[at + 3] = (unsigned char)(v >> 24);
    };
    put32(2, 54 + image);
    put32(10, 54);
    put32(14, 40);
    put32(18, unsigned(width));
    put32(22, unsigned(height));
    header[26] = 1;
    header[28] = 32;
    put32(34, image);
    std::fwrite(header, 1, sizeof header, f);
    std::vector<unsigned char> row(size_t(width) * 4);
    for (int y = height - 1; y >= 0; y--) {
        const unsigned char* src = rgba + size_t(y) * pitch;
        for (int x = 0; x < width; x++) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

void shot(void* back_buffer, int width, int height, const char* path) {
    std::vector<unsigned char> rgba;
    const bool ok = s.renderer->read_back(back_buffer, width, height, rgba) &&
                    write_bmp(path, rgba.data(), width, height, size_t(width) * 4);
    std::fprintf(stderr, "[ui] DES_UI_SHOT: %s %s\n", ok ? "wrote" : "could not write", path);
}

// The display's scale, as Windows' DPI over 96: what the display reports, else 1.
float display_scale() {
    const unsigned dpi = window_dpi();
    return dpi ? float(dpi) / 96.f : 1.f;
}

}  // namespace

unsigned window_dpi() {  // (what the window's display reports)
    float dpi = 0.f;
    const int display = s.window ? SDL_GetWindowDisplayIndex(s.window) : 0;
    if (display < 0 || SDL_GetDisplayDPI(display, nullptr, &dpi, nullptr) != 0 || dpi < 48.f || dpi > 480.f) return 0;
    return unsigned(dpi + 0.5f);
}

bool init(void* d3d12_device, void* d3d12_queue, void* hwnd, int width, int height) {
    if (s.initialised) return true;
    s.renderer = d3d12::Renderer::create(d3d12_device, d3d12_queue, find_png);
    if (!s.renderer) {
        std::fprintf(stderr, "[ui] the menu's renderer could not start: the menu is unavailable\n");
        return false;
    }
    s.system = std::make_unique<System>();
    s.window = static_cast<SDL_Window*>(hwnd);
    Rml::SetFileInterface(&s.files);
    Rml::SetSystemInterface(s.system.get());
    Rml::SetRenderInterface(s.renderer.get());
    if (!Rml::Initialise()) {
        shutdown();
        return false;
    }
    load_fonts();
    s.rml = Rml::CreateContext("main", {std::max(width, 1), std::max(height, 1)});
    if (!s.rml) {
        shutdown();
        return false;
    }
    s.initialised = true;
    return true;
}

void shutdown() {
    if (s.rml || s.initialised) {
        Rml::Shutdown();
        s.rml = nullptr;
    }
    s.renderer.reset();
    s.system.reset();
    s.initialised = false;
}

void release_buffers() {
    if (s.initialised) s.renderer->release_back_buffers();
}

bool is_initialized() { return s.initialised; }
Rml::Context* context() { return s.rml; }

void set_ui_scale(float scale) { s.ui_scale = std::clamp(scale, 0.0f, 4.0f); }

void render(void* back_buffer, unsigned, int width, int height, const char* shot_path) {
    if (!s.initialised) return;
    if (s.rml->GetDimensions() != Rml::Vector2i{width, height}) s.rml->SetDimensions({width, height});
    // As on Windows: the display's scale times the UI scale setting; the window's size gives more room, not a larger UI.
    s.rml->SetDensityIndependentPixelRatio((s.ui_scale > 0.0f ? s.ui_scale : 1.0f) * display_scale());
    s.rml->Update();
    s.renderer->begin_frame(width, height);
    s.rml->Render();
    s.renderer->end_frame(back_buffer);
    if (shot_path) shot(back_buffer, width, height, shot_path);
}

void screenshot(void* back_buffer, unsigned, int width, int height, const char* shot_path) {
    if (s.initialised) shot(back_buffer, width, height, shot_path);
}

bool capture(void* back_buffer, unsigned, int width, int height, std::vector<unsigned char>& rgba) {
    return s.initialised && width > 0 && height > 0 && s.renderer->read_back(back_buffer, width, height, rgba);
}

void set_input_type(InputType) {}

bool animations_enabled() {
    static const bool enabled = !std::getenv("DES_UI_NO_ANIM");
    return enabled;
}

void add_png(const std::string& source, const void* png, size_t size) {
    if (!png || !size) return;
    const auto* b = static_cast<const uint8_t*>(png);
    std::lock_guard<std::mutex> lock(sPngLock);
    sPngs[source].assign(b, b + size);
}

}  // namespace ui::host
