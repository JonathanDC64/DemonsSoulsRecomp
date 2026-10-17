// The UI's connection to the game (see host.hpp). RmlUi's stock D3D11 renderer draws on a D3D11On12 device that shares
// the game's D3D12 device and command queue; each swap chain back buffer is wrapped as a D3D11 render target, and the
// UI is composited over the finished game frame (premultiplied alpha) just before Present. Adapted for Demon's Souls
// from FFCCEOT-Recomp's host.cpp.
#include "RmlUi_Platform_Win32.h"
#include "RmlUi_Renderer_DX11.h"

#include "host.hpp"
#include "ui_assets.hpp"

#include <d3d11on12.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ui::host {
namespace {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

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

// RmlUi's Win32 system interface (time, cursor, clipboard, IME), with its warnings and errors printed.
class System final : public SystemInterface_Win32 {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "[ui] %s\n", message.c_str());
        return true;
    }
};

struct Wrapped {
    void* source = nullptr;  // the ID3D12Resource it wraps
    ID3D11Resource* resource = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};

struct State {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11On12Device* on12 = nullptr;
    Wrapped buffers[8];
    EmbeddedFiles files;
    std::unique_ptr<System> system;
    std::unique_ptr<RenderInterface_DX11> renderer;
    Rml::Context* rml = nullptr;
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

Wrapped* wrap(void* back_buffer, unsigned index) {
    if (index >= std::size(s.buffers) || !back_buffer) return nullptr;
    Wrapped& w = s.buffers[index];
    if (w.source == back_buffer && w.rtv) return &w;
    release(w.rtv);
    release(w.resource);
    w.source = nullptr;
    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = s.on12->CreateWrappedResource(static_cast<IUnknown*>(back_buffer), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                               D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&w.resource));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[ui] CreateWrappedResource failed (0x%08lX)\n", (unsigned long)hr);
        return nullptr;
    }
    hr = s.device->CreateRenderTargetView(w.resource, nullptr, &w.rtv);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[ui] CreateRenderTargetView failed (0x%08lX)\n", (unsigned long)hr);
        release(w.resource);
        return nullptr;
    }
    w.source = back_buffer;
    return &w;
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

// Copies the (acquired) wrapped back buffer to a staging texture; read_staging writes it once the work has run.
ID3D11Texture2D* copy_to_staging(Wrapped& w, int width, int height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(s.device->CreateTexture2D(&desc, nullptr, &staging))) return nullptr;
    s.context->CopyResource(staging, w.resource);
    return staging;
}

void read_staging(ID3D11Texture2D* staging, int width, int height, const char* path) {
    if (!staging) return;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(s.context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        const bool ok = write_bmp(path, static_cast<const unsigned char*>(map.pData), width, height, map.RowPitch);
        s.context->Unmap(staging, 0);
        std::fprintf(stderr, "[ui] DES_UI_SHOT: %s %s\n", ok ? "wrote" : "could not write", path);
    }
    staging->Release();
}

}  // namespace

bool init(void* d3d12_device, void* d3d12_queue, void* hwnd, int width, int height) {
    if (s.initialised) return true;
    if (!d3d12_device || !d3d12_queue) return false;
    IUnknown* queues[] = {static_cast<IUnknown*>(d3d12_queue)};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // No debug layer, ever (the D3D12 debug layer must stay off).
    HRESULT hr = D3D11On12CreateDevice(static_cast<IUnknown*>(d3d12_device), D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       UINT(std::size(levels)), queues, 1, 0, &s.device, &s.context, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[ui] D3D11On12CreateDevice failed (0x%08lX): the menu is unavailable\n", (unsigned long)hr);
        return false;
    }
    if (FAILED(s.device->QueryInterface(IID_PPV_ARGS(&s.on12)))) {
        std::fprintf(stderr, "[ui] no ID3D11On12Device: the menu is unavailable\n");
        shutdown();
        return false;
    }
    s.renderer = std::make_unique<RenderInterface_DX11>(s.device);
    s.system = std::make_unique<System>();
    s.system->SetWindow(static_cast<HWND>(hwnd));
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
    if (s.rml) {
        Rml::Shutdown();
        s.rml = nullptr;
    }
    for (Wrapped& w : s.buffers) {
        release(w.rtv);
        release(w.resource);
        w.source = nullptr;
    }
    s.renderer.reset();
    s.system.reset();
    release(s.on12);
    release(s.context);
    release(s.device);
    s.initialised = false;
}

bool is_initialized() { return s.initialised; }
Rml::Context* context() { return s.rml; }

void set_ui_scale(float scale) { s.ui_scale = std::clamp(scale, 0.0f, 4.0f); }

void render(void* back_buffer, unsigned index, int width, int height, const char* shot_path) {
    if (!s.initialised) return;
    if (s.rml->GetDimensions() != Rml::Vector2i{width, height}) s.rml->SetDimensions({width, height});
    // 1 dp = 1 px at 720p (the back buffer is the PS3's 1280x720), times the UI scale setting.
    s.rml->SetDensityIndependentPixelRatio((s.ui_scale > 0.0f ? s.ui_scale : 1.0f) * float(height) / 720.0f);
    s.rml->Update();
    Wrapped* w = wrap(back_buffer, index);
    if (!w) return;
    s.on12->AcquireWrappedResources(&w->resource, 1);
    s.renderer->SetViewport(width, height);
    s.renderer->BeginFrame();
    s.rml->Render();
    s.renderer->EndFrame(w->rtv);
    ID3D11Texture2D* staging = shot_path ? copy_to_staging(*w, width, height) : nullptr;
    s.on12->ReleaseWrappedResources(&w->resource, 1);
    s.context->Flush();  // submits to the game's queue, ahead of its Present
    read_staging(staging, width, height, shot_path);
}

void screenshot(void* back_buffer, unsigned index, int width, int height, const char* shot_path) {
    if (!s.initialised) return;
    Wrapped* w = wrap(back_buffer, index);
    if (!w) return;
    s.on12->AcquireWrappedResources(&w->resource, 1);
    ID3D11Texture2D* staging = copy_to_staging(*w, width, height);
    s.on12->ReleaseWrappedResources(&w->resource, 1);
    s.context->Flush();
    read_staging(staging, width, height, shot_path);
}

void set_input_type(InputType) {}

bool animations_enabled() {
    static const bool enabled = !std::getenv("DES_UI_NO_ANIM");
    return enabled;
}

}  // namespace ui::host
