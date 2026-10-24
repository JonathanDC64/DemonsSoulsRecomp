// The UI's connection to the game (see host.hpp): the RmlUi context, the embedded files and fonts, the scale, and the
// renderer that draws the documents over the finished game frame just before Present (renderer.hpp): RmlUi's
// renderer on the game's D3D12 device and queue (render_d3d12.cpp), or on Windows, for one release, D3D11On12 as
// before (render_d3d11.cpp, DES_UI_RENDER=d3d11). Images are decoded with stb_image. Adapted for Demon's Souls from
// FFCCEOT-Recomp's host.cpp (GL3 in an SDL window).
#include "host.hpp"

#include "portable.hpp"
#include "renderer.hpp"
#include "ui_assets.hpp"

#ifdef _WIN32
#include "RmlUi_Platform_Win32.h"
#else
#include "mouse.hpp"

#include <SDL.h>

#include <chrono>
#endif

#include <algorithm>
#include <cmath>
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

void log_message(Rml::Log::Type type, const Rml::String& message) {
    if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "[ui] %s\n", message.c_str());
}

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

// ---- What differs by platform: RmlUi's system interface (time, cursor, clipboard, IME), the window's DPI and the
// renderers there are ----

void* sWindow = nullptr;  // the game's window: an HWND on Windows, an SDL_Window* elsewhere

#ifdef _WIN32

// RmlUi's Win32 system interface, with its warnings and errors printed and the two diagonal resize cursors it lacks
// (the HUD editor's corners).
class System final : public SystemInterface_Win32 {
public:
    explicit System(void* window) : mWindow(static_cast<HWND>(window)) { SetWindow(mWindow); }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        log_message(type, message);
        return true;
    }
    void SetMouseCursor(const Rml::String& name) override {
        HCURSOR cursor = nullptr;
        if (name == "nwse-resize") cursor = LoadCursor(nullptr, IDC_SIZENWSE);
        else if (name == "nesw-resize") cursor = LoadCursor(nullptr, IDC_SIZENESW);
        if (!cursor || !mWindow) {
            SystemInterface_Win32::SetMouseCursor(name);
            return;
        }
        SetCursor(cursor);
        SetClassLongPtrA(mWindow, GCLP_HCURSOR, (LONG_PTR)cursor);  // (as the base does)
    }

private:
    HWND mWindow = nullptr;
};

unsigned dpi_of_window() {
    using DpiFn = UINT(WINAPI*)(HWND);  // (Windows 10 1607; by name, as the build targets older headers)
    static const auto get_dpi =
        reinterpret_cast<DpiFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    return sWindow && get_dpi ? get_dpi(static_cast<HWND>(sWindow)) : 0;
}

// D3D12, or D3D11On12 with DES_UI_RENDER=d3d11; the other one if the one asked for cannot start.
std::unique_ptr<Renderer> make_renderer(void* device, void* queue) {
    const char* v = std::getenv("DES_UI_RENDER");
    const bool old = v && same_name(v, "d3d11");
    std::unique_ptr<Renderer> r = old ? d3d11::create(device, queue, find_png) : d3d12::create(device, queue, find_png);
    bool used_old = old;
    if (!r) {
        std::fprintf(stderr, "[ui] the menus' %s renderer could not start: trying %s\n", old ? "D3D11" : "D3D12",
                     old ? "D3D12" : "D3D11");
        r = old ? d3d12::create(device, queue, find_png) : d3d11::create(device, queue, find_png);
        used_old = !old;
    }
    if (r && used_old) std::fprintf(stderr, "[ui] the menus draw through D3D11On12\n");
    return r;
}

#else

// RmlUi's system interface: the time, its warnings and errors printed, and the cursor's shape (set on the window's
// thread, mouse_sdl.cpp). The clipboard is RmlUi's own (within the game).
class System final : public Rml::SystemInterface {
public:
    explicit System(void*) {}
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        log_message(type, message);
        return true;
    }
    void SetMouseCursor(const Rml::String& name) override {
        int shape = SDL_SYSTEM_CURSOR_ARROW;
        if (name == "pointer") shape = SDL_SYSTEM_CURSOR_HAND;
        else if (name == "text") shape = SDL_SYSTEM_CURSOR_IBEAM;
        else if (name == "move" || name.rfind("rmlui-scroll", 0) == 0) shape = SDL_SYSTEM_CURSOR_SIZEALL;
        else if (name == "resize" || name == "nwse-resize") shape = SDL_SYSTEM_CURSOR_SIZENWSE;
        else if (name == "nesw-resize") shape = SDL_SYSTEM_CURSOR_SIZENESW;
        else if (name == "cross") shape = SDL_SYSTEM_CURSOR_CROSSHAIR;
        else if (name == "unavailable") shape = SDL_SYSTEM_CURSOR_NO;
        mouse::set_cursor(shape);
    }

private:
    const std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
};

unsigned dpi_of_window() {  // (what the window's display reports)
    float dpi = 0.f;
    const int display = sWindow ? SDL_GetWindowDisplayIndex(static_cast<SDL_Window*>(sWindow)) : 0;
    if (display < 0 || SDL_GetDisplayDPI(display, nullptr, &dpi, nullptr) != 0 || dpi < 48.f || dpi > 480.f) return 0;
    return unsigned(dpi + 0.5f);
}

std::unique_ptr<Renderer> make_renderer(void* device, void* queue) { return d3d12::create(device, queue, find_png); }

#endif

// ---- The same everywhere ----

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

struct State {
    EmbeddedFiles files;
    std::unique_ptr<System> system;
    std::unique_ptr<Renderer> renderer;
    Rml::Context* rml = nullptr;
    float ui_scale = 0.0f;
    bool follow_height = false;
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

void shot(void* back_buffer, unsigned index, int width, int height, const char* path) {
    std::vector<unsigned char> rgba;
    const bool ok = s.renderer->read_back(back_buffer, index, width, height, rgba) &&
                    write_bmp(path, rgba.data(), width, height, size_t(width) * 4);
    std::fprintf(stderr, "[ui] DES_UI_SHOT: %s %s\n", ok ? "wrote" : "could not write", path);
}

}  // namespace

bool init(void* d3d12_device, void* d3d12_queue, void* hwnd, int width, int height) {
    if (s.initialised) return true;
    if (!d3d12_device || !d3d12_queue) return false;
    s.renderer = make_renderer(d3d12_device, d3d12_queue);
    if (!s.renderer) {
        std::fprintf(stderr, "[ui] the menu's renderer could not start: the menu is unavailable\n");
        return false;
    }
    sWindow = hwnd;
    s.system = std::make_unique<System>(hwnd);
    Rml::SetFileInterface(&s.files);
    Rml::SetSystemInterface(s.system.get());
    Rml::SetRenderInterface(s.renderer->rml());
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
void* window() { return sWindow; }

void set_ui_scale(float scale) { s.ui_scale = std::clamp(scale, 0.0f, 4.0f); }
void set_hdr(float scale, bool srgb) {
    if (s.renderer) s.renderer->set_hdr(scale, srgb);
}
void set_follow_height(bool on) { s.follow_height = on; }

unsigned window_dpi() { return dpi_of_window(); }

// After a change of the dp ratio. RmlUi works a length out again only where it is written in dp: one taken from a
// variable (var(--space-md), most of the theme's sizes) keeps its old size. A document given a new copy of its style
// sheet works every property out again, the variables' too.
static void restyle_documents() {
    static const Rml::StyleSheetContainer kNothing;
    for (int i = 0; i < s.rml->GetNumDocuments(); ++i) {
        Rml::ElementDocument* doc = s.rml->GetDocument(i);
        if (const Rml::StyleSheetContainer* sheet = doc ? doc->GetStyleSheetContainer() : nullptr)
            doc->SetStyleSheetContainer(sheet->CombineStyleSheetContainer(kNothing));
    }
}

void render(void* back_buffer, unsigned index, int width, int height, const char* shot_path) {
    if (!s.initialised) return;
    if (s.rml->GetDimensions() != Rml::Vector2i{width, height}) s.rml->SetDimensions({width, height});
    // Dusklight's (and FFCCEOT-Recomp's) rule: the display's scale (its DPI), times the UI scale setting. The
    // window's size does not scale the UI: a larger window gives the documents more room, as for any program.
    // Except while a full-window page is open (set_follow_height, the setup screens): then the height scales it too,
    // in quarter steps rounded down, so the page always has at least 720 dp of height (1080p 1.5, 1440p 2) and a
    // window drag lays it out again only every 180 px. Text is drawn at whole pixel sizes, so no step blurs it.
    const unsigned dpi = window_dpi();
    float base = dpi ? float(dpi) / 96.0f : 1.0f;
    if (s.follow_height && height > 0)
        base = std::max(base, std::min(4.0f, std::floor(float(height) / 720.0f * 4.0f) / 4.0f));
    if (const float ratio = (s.ui_scale > 0.0f ? s.ui_scale : 1.0f) * base;
        ratio != s.rml->GetDensityIndependentPixelRatio()) {
        s.rml->SetDensityIndependentPixelRatio(ratio);
        restyle_documents();
    }
    s.rml->Update();
    if (!s.renderer->begin_frame(back_buffer, index, width, height)) return;
    s.rml->Render();
    s.renderer->end_frame();
    if (shot_path) shot(back_buffer, index, width, height, shot_path);
}

void screenshot(void* back_buffer, unsigned index, int width, int height, const char* shot_path) {
    if (s.initialised) shot(back_buffer, index, width, height, shot_path);
}

bool capture(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba) {
    return s.initialised && width > 0 && height > 0 && s.renderer->read_back(back_buffer, index, width, height, rgba);
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
