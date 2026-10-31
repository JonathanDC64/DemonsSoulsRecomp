// ui_render_compare: the proof for render_gpu.cpp, the menus' renderer on ps3recomp's GPU layer. The menus' pages are
// drawn on one D3D12 device by today's renderer (render_d3d12.cpp, ui::d3d12::create) and by render_gpu.cpp twice: over
// the same D3D12 objects (ui::gpu::create_d3d12, what DES_UI_RENDER=gpu runs), and on a device of the layer's own with
// the back buffers named as its textures (ui::gpu::create); and by today's renderer once more, a control. One stream
// of RmlUi calls feeds all four (a tee), and their pictures are compared byte for byte after every frame, on 8-bit
// (SDR) and FP16 scRGB (HDR) back buffers; at each page's end their read_backs too. Each renderer's D3D12 command list
// calls are counted per page as well (ui_render_compare_hooks.c): they should be the same.
//
//   ui_render_compare [--warp] [--frames N] [--bmp DIR] [--scratch DIR] [--page NAME] [--probe X Y] [--verbose]
//
// --warp: WARP, D3D12's software adapter, instead of the default GPU. --frames: frames a page (24). --bmp: each page's
// last picture as a BMP file there (today's renderer's, and another's where it differs). --scratch: the folder for
// the settings and data the UI writes (default %TEMP%\ui_render_compare), so the player's are never touched. With
// PS3_VFS_ROOT naming the game's folder, as for a run, also a page with the game's models (docs/112).
// --page: only the pages whose names hold NAME. --probe: one pixel's bytes in each frame, from every renderer.
// --verbose: the first bytes that differ in each frame.
// Exit code 0: every picture and count the same; 1: one differs; 2: it could not run.
#include "host.hpp"
#include "renderer.hpp"
#include "settings_window.hpp"
#include "setup_screens.hpp"
#include "souls_model.hpp"
#include "ui.hpp"

#include "gpu.h"
#include "ui_render_compare_hooks.h"

#include <RmlUi/Core.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// settings.cpp and report.cpp set variables through the game's getenv cache (boot_main.cpp); here, the process's.
extern "C" void ps3_setenv(const char* name, const char* value) {
    _putenv_s(name, value ? value : "");
}

namespace {

// The last is today's renderer once more, a control: where it differs from the first, the device draws the same
// calls differently (some GPUs do, by one step at a few pixels), so a difference there is not render_gpu.cpp's.
constexpr int kRenderers = 4, kControl = 3;
const char* const kRendererName[kRenderers] = {"d3d12", "gpu", "gpu-dev", "d3d12-again"};

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---- The device, and the test's own commands (backgrounds, read backs), on the renderers' queue ----

struct Device {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;

    bool create(bool warp, std::string& name) {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        IDXGIAdapter1* adapter = nullptr;
        if (warp) {
            factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        } else {
            IDXGIFactory6* f6 = nullptr;
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f6)))) {
                f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
                f6->Release();
            }
            if (!adapter) factory->EnumAdapters1(0, &adapter);
        }
        factory->Release();
        if (!adapter) return false;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        char buf[256];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, buf, sizeof buf, nullptr, nullptr);
        name = buf;
        const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        adapter->Release();
        if (FAILED(hr)) return false;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
               SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    void destroy() {
        wait();
        release(fence);
        release(list);
        release(alloc);
        release(queue);
        release(dev);
    }
    void wait() {
        if (fence && fence->GetCompletedValue() < value) fence->SetEventOnCompletion(value, nullptr);
    }
    ID3D12GraphicsCommandList* begin() {
        wait();
        alloc->Reset();
        list->Reset(alloc, nullptr);
        return list;
    }
    void submit() {
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++value);
    }
    ID3D12Resource* texture(int w, int h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = UINT64(w);
        d.Height = UINT(h);
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    ID3D12Resource* buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
};

void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

uint16_t to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int e = int((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) return uint16_t(sign);
    if (e >= 31) return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | uint32_t(e) << 10 | (m + 0x1000u) >> 13);
}

// One size and format of back buffer: one for each renderer, the background copied into them before each frame, and
// a readback buffer for each.
struct Targets {
    int w = 0, h = 0;
    bool hdr = false;
    ID3D12Resource* background = nullptr;
    ID3D12Resource* target[kRenderers] = {};
    ID3D12Resource* readback[kRenderers] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    std::vector<uint8_t> bg;  // the background's bytes, as read back

    bool create(Device& d, int width, int height, bool half) {
        w = width;
        h = height;
        hdr = half;
        const DXGI_FORMAT format = half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        background = d.texture(w, h, format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (int i = 0; i < kRenderers; i++) {
            target[i] = d.texture(w, h, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            if (!target[i]) return false;
        }
        const D3D12_RESOURCE_DESC desc = target[0]->GetDesc();
        UINT rows = 0;
        UINT64 row = 0;
        d.dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row, &bytes);
        for (int i = 0; i < kRenderers; i++) {
            readback[i] = d.buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!readback[i]) return false;
        }
        // The background: a gradient with a checker, so the UI's blending over a picture shows (HDR: up to 1.5).
        ID3D12Resource* upload = d.buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!background || !upload) return false;
        uint8_t* p = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (int y = 0; y < h; y++) {
            uint8_t* row_p = p + size_t(fp.Footprint.RowPitch) * size_t(y);
            for (int x = 0; x < w; x++) {
                const float r = float(x) / float(w), g = float(y) / float(h);
                const float b = ((x / 32 + y / 32) % 2) ? 0.75f : 0.25f;
                if (half) {
                    const uint16_t v[4] = {to_half(r * 1.5f), to_half(g), to_half(b), to_half(1.f)};
                    std::memcpy(row_p + size_t(x) * 8, v, 8);
                } else {
                    const uint8_t v[4] = {uint8_t(r * 255.f), uint8_t(g * 255.f), uint8_t(b * 255.f), 255};
                    std::memcpy(row_p + size_t(x) * 4, v, 4);
                }
            }
        }
        upload->Unmap(0, nullptr);
        ID3D12GraphicsCommandList* l = d.begin();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = background;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(l, background, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.submit();
        d.wait();
        upload->Release();
        std::vector<uint8_t> all[kRenderers];
        fill(d);
        read(d, all);
        bg = std::move(all[0]);
        return true;
    }
    void destroy(Device& d) {
        d.wait();
        release(background);
        for (int i = 0; i < kRenderers; i++) {
            release(target[i]);
            release(readback[i]);
        }
    }
    void fill(Device& d) {
        ID3D12GraphicsCommandList* l = d.begin();
        for (ID3D12Resource* t : target) {
            barrier(l, t, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(t, background);
            barrier(l, t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
        d.submit();
    }
    // Every renderer's picture as it is now, rows packed: w * h * (4 or 8) bytes each.
    void read(Device& d, std::vector<uint8_t> (&out)[kRenderers]) {
        ID3D12GraphicsCommandList* l = d.begin();
        for (int i = 0; i < kRenderers; i++) {
            barrier(l, target[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = readback[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            src.pResource = target[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l, target[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        }
        d.submit();
        d.wait();
        const size_t row = size_t(w) * (hdr ? 8 : 4);
        for (int i = 0; i < kRenderers; i++) {
            out[i].resize(row * size_t(h));
            uint8_t* p = nullptr;
            const D3D12_RANGE all{0, size_t(bytes)};
            if (FAILED(readback[i]->Map(0, &all, reinterpret_cast<void**>(&p)))) {
                out[i].clear();
                continue;
            }
            for (int y = 0; y < h; y++) std::memcpy(out[i].data() + row * size_t(y), p + size_t(fp.Footprint.RowPitch) * size_t(y), row);
            const D3D12_RANGE none{0, 0};
            readback[i]->Unmap(0, &none);
        }
    }
};

// ---- One stream of RmlUi calls to every renderer ----

// Each renderer's handle for one of RmlUi's (geometry, a texture, a filter, a shader).
struct Handles {
    uintptr_t h[kRenderers] = {};
};

class Tee final : public ui::Renderer, public Rml::RenderInterface {
public:
    std::vector<std::unique_ptr<ui::Renderer>> renderers;
    void* targets[kRenderers] = {};       // this frame's back buffer of each
    std::function<void(Tee&)> before_end;  // draws between the context's Render and end_frame (the models page)
    int mismatches = 0;                    // calls whose results differed between the renderers

    Rml::RenderInterface* rml() override { return this; }
    bool begin_frame(void*, unsigned index, int width, int height) override {
        bool ok = true;
        each([&](int i, ui::Renderer& r) { ok = r.begin_frame(targets[i], index, width, height) && ok; });
        return ok;
    }
    void end_frame() override {
        if (before_end) before_end(*this);
        each([&](int, ui::Renderer& r) { r.end_frame(); });
    }
    bool read_back(void*, unsigned index, int width, int height, std::vector<unsigned char>& rgba) override {
        return renderers[0]->read_back(targets[0], index, width, height, rgba);
    }
    void release_back_buffers() override {
        each([&](int, ui::Renderer& r) { r.release_back_buffers(); });
    }
    void set_hdr(float scale, bool srgb) override {
        each([&](int, ui::Renderer& r) { r.set_hdr(scale, srgb); });
    }
    bool draw_model(const char* name, const ui::ModelView& view) override {
        bool drawn[kRenderers] = {};
        each([&](int i, ui::Renderer& r) { drawn[i] = r.draw_model(name, view); });
        for (int i = 1; i < count(); i++) mismatches += drawn[i] != drawn[0];
        return drawn[0];
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
        return made([&](Rml::RenderInterface& r) { return r.CompileGeometry(vertices, indices); });
    }
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        each([&](int i, ui::Renderer& r) { r.rml()->RenderGeometry(of(geometry, i), translation, of(texture, i)); });
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        let_go(geometry, [](Rml::RenderInterface& r, uintptr_t h) { r.ReleaseGeometry(h); });
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        Rml::Vector2i d[kRenderers];
        int i = 0;
        const Rml::TextureHandle t = made([&](Rml::RenderInterface& r) {
            d[i] = dimensions;
            return r.LoadTexture(d[i++], source);
        });
        for (int k = 1; k < count(); k++) mismatches += d[k] != d[0];
        dimensions = d[0];
        return t;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        return made([&](Rml::RenderInterface& r) { return r.GenerateTexture(source, dimensions); });
    }
    void ReleaseTexture(Rml::TextureHandle texture) override {
        let_go(texture, [](Rml::RenderInterface& r, uintptr_t h) { r.ReleaseTexture(h); });
    }
    void EnableScissorRegion(bool enable) override {
        each([&](int, ui::Renderer& r) { r.rml()->EnableScissorRegion(enable); });
    }
    void SetScissorRegion(Rml::Rectanglei region) override {
        each([&](int, ui::Renderer& r) { r.rml()->SetScissorRegion(region); });
    }
    void EnableClipMask(bool enable) override {
        each([&](int, ui::Renderer& r) { r.rml()->EnableClipMask(enable); });
    }
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override {
        each([&](int i, ui::Renderer& r) { r.rml()->RenderToClipMask(operation, of(geometry, i), translation); });
    }
    void SetTransform(const Rml::Matrix4f* transform) override {
        each([&](int, ui::Renderer& r) { r.rml()->SetTransform(transform); });
    }
    // (Layers are numbers that every renderer hands out the same: the base layer is 0.)
    Rml::LayerHandle PushLayer() override {
        Rml::LayerHandle l[kRenderers] = {};
        each([&](int i, ui::Renderer& r) { l[i] = r.rml()->PushLayer(); });
        for (int i = 1; i < count(); i++) mismatches += l[i] != l[0];
        return l[0];
    }
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override {
        each([&](int i, ui::Renderer& r) {
            std::vector<Rml::CompiledFilterHandle> f;
            for (const Rml::CompiledFilterHandle h : filters) f.push_back(of(h, i));
            r.rml()->CompositeLayers(source, destination, blend_mode, {f.data(), f.size()});
        });
    }
    void PopLayer() override {
        each([&](int, ui::Renderer& r) { r.rml()->PopLayer(); });
    }
    Rml::TextureHandle SaveLayerAsTexture() override {
        return made([&](Rml::RenderInterface& r) { return r.SaveLayerAsTexture(); });
    }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override {
        return made([&](Rml::RenderInterface& r) { return r.SaveLayerAsMaskImage(); });
    }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override {
        return made([&](Rml::RenderInterface& r) { return r.CompileFilter(name, parameters); });
    }
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override {
        let_go(filter, [](Rml::RenderInterface& r, uintptr_t h) { r.ReleaseFilter(h); });
    }
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override {
        return made([&](Rml::RenderInterface& r) { return r.CompileShader(name, parameters); });
    }
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override {
        each([&](int i, ui::Renderer& r) { r.rml()->RenderShader(of(shader, i), of(geometry, i), translation, of(texture, i)); });
    }
    void ReleaseShader(Rml::CompiledShaderHandle shader) override {
        let_go(shader, [](Rml::RenderInterface& r, uintptr_t h) { r.ReleaseShader(h); });
    }

    int count() const { return int(renderers.size()); }
    // Renderer i's calls, counted as its own (ui_render_compare_hooks.c).
    template <typename F>
    void each(F&& f) {
        for (int i = 0; i < count(); i++) {
            g_urc_who = i;
            f(i, *renderers[i]);
        }
        g_urc_who = -1;
    }

private:
    static uintptr_t of(uintptr_t handle, int i) { return handle ? reinterpret_cast<const Handles*>(handle)->h[i] : 0; }
    // A handle each renderer makes: none if the first made none (and then the others' are let go of, as a mismatch).
    template <typename F>
    uintptr_t made(F&& make) {
        auto* h = new Handles;
        each([&](int i, ui::Renderer& r) { h->h[i] = uintptr_t(make(*r.rml())); });
        bool any = false, all = true;
        for (int i = 0; i < count(); i++) {
            any = any || h->h[i];
            all = all && h->h[i];
        }
        if (any && !all) mismatches++;
        if (!h->h[0]) {
            delete h;
            return 0;
        }
        return reinterpret_cast<uintptr_t>(h);
    }
    template <typename F>
    void let_go(uintptr_t handle, F&& f) {
        if (!handle) return;
        auto* h = reinterpret_cast<Handles*>(handle);
        each([&](int i, ui::Renderer& r) {
            if (h->h[i]) f(*r.rml(), h->h[i]);
        });
        delete h;
    }
};

// RmlUi's clock: 1/60 s a frame, whatever the GPU takes, so the pages are drawn the same way on every run.
class Clock final : public Rml::SystemInterface {
public:
    double now = 0.0;
    double GetElapsedTime() override { return now; }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "[rml] %s\n", message.c_str());
        return true;
    }
};

// ---- Pages ----

const char* const kStyle = R"(
body { width: 100%; height: 100%; font-family: Alegreya Sans; font-size: 22px; color: #f0e8d8; }
div { display: block; }
.box { position: absolute; }
)";

const char* const kClipPage = R"(<rml><head><style>
.round { border-radius: 48px; overflow: hidden; background-color: #3a6ea5c0; padding: 18px; }
.inner { border-radius: 28px; overflow: hidden; background-color: #e0a030d0; height: 260px; padding: 12px; }
.deep { border-radius: 60px 10px; overflow: hidden; background-color: #20a060e0; height: 200px; margin: 16px; }
.turn { transform: rotate(14deg); }
.tilt { transform: perspective(600px) rotateY(25deg); }
.shadow { box-shadow: #000c 8px 10px 24px 4px, #ff8000c0 0px 0px 14px 2px inset; background-color: #602040; border-radius: 22px; }
.outline { border: 6px #fff8; border-radius: 30px; }
.big { font-size: 44px; font-family: Cinzel; }
</style></head><body>
<div class="box round" style="left: 40px; top: 40px; width: 420px; height: 420px;">
  <div class="inner turn"><div class="deep"><span class="big">Clip in clip in clip</span> The quick brown fox jumps
  over the lazy dog again and again, past the rounded edges of three boxes inside each other.</div></div>
</div>
<div class="box round tilt" style="left: 520px; top: 60px; width: 300px; height: 300px;">
  <div class="inner">Turned in depth, its rounded corners a clip mask through a perspective transform.</div>
</div>
<div class="box shadow" style="left: 880px; top: 80px; width: 320px; height: 200px;">A box shadow and an inset one.</div>
<div class="box outline" style="left: 880px; top: 340px; width: 320px; height: 140px;">A rounded border</div>
<div class="box round" style="left: 520px; top: 420px; width: 300px; height: 240px; transform: rotate(-7deg) scale(1.1);">
  <div class="inner turn" style="transform: rotate(31deg);">Two turns, nested.</div>
</div>
</body></rml>)";

const char* const kFilterPage = R"(<rml><head><style>
.f { position: absolute; width: 230px; height: 130px; border-radius: 18px; background-color: #c04040; padding: 10px;
     font-size: 26px; }
.blur { filter: blur(5px); }
.blur2 { filter: blur(28px); }
.ds { filter: drop-shadow(#000d 10px 12px 7px); background-color: #4080c0; }
.op { filter: opacity(0.45); background-color: #40c080; }
.cm { filter: brightness(1.4) contrast(0.7) saturate(1.8) hue-rotate(40deg) sepia(0.3) grayscale(0.2) invert(0.1); }
.bd { backdrop-filter: blur(9px); background-color: #ffffff30; }
.mask { mask-image: linear-gradient(90deg, #000f, #0000); background-color: #ffd040; }
.g1 { decorator: linear-gradient(120deg, #ff0040, #4000ff 60%, #00ffc0); }
.g2 { decorator: radial-gradient(circle, #ffffff, #ff8000 40%, #200040); }
.g3 { decorator: conic-gradient(from 30deg, #ff0000, #00ff00, #0000ff, #ff0000); }
.g4 { decorator: repeating-linear-gradient(45deg, #000000 0px, #ffffff 9px, #000000 18px); }
.both { filter: blur(3px) drop-shadow(#ff00ffcc 0px 0px 10px); opacity: 0.8; background-color: #208080; }
img { width: 96px; height: 96px; }
</style></head><body>
<div class="f blur" style="left: 30px; top: 30px;">Blurred a little</div>
<div class="f blur2" style="left: 290px; top: 30px;">Blurred a lot</div>
<div class="f ds" style="left: 550px; top: 30px;">Drop shadow</div>
<div class="f op" style="left: 810px; top: 30px;">Opacity filter</div>
<div class="f cm" style="left: 30px; top: 200px;">Colour matrix</div>
<div class="f bd" style="left: 290px; top: 190px;">Backdrop blur over the boxes</div>
<div class="f mask" style="left: 550px; top: 200px;">Mask image</div>
<div class="f both" style="left: 810px; top: 200px;">Blur and shadow</div>
<div class="f g1" style="left: 30px; top: 370px;">Linear</div>
<div class="f g2" style="left: 290px; top: 370px;">Radial</div>
<div class="f g3" style="left: 550px; top: 370px;">Conic</div>
<div class="f g4" style="left: 810px; top: 370px;">Repeating</div>
<div class="f" style="left: 1060px; top: 30px; width: 160px; height: 460px; background-color: #303030;">
  <img src="compare:image"/><img src="compare:image" style="width: 140px; height: 50px;"/>
  <div style="transform: rotate(20deg);"><img src="compare:image" style="width: 60px; height: 60px;"/></div>
</div>
<div class="f" style="left: 30px; top: 540px; width: 1000px; height: 110px; background-color: #00000080;
     font-family: Cinzel; font-size: 34px;">Demon's Souls, a line of text in the display face, and
  <span style="font-family: Material Symbols Rounded; font-size: 40px;">settings home</span> icons.</div>
</body></rml>)";

const char* const kModelPage = R"(<rml><head><style>
.m { position: absolute; decorator: shader("model:moonlight"); }
</style></head><body>
<div class="m" style="left: 60px; top: 60px; width: 300px; height: 300px;"></div>
<div class="m" style="left: 380px; top: 100px; width: 200px; height: 360px; opacity: 0.6;"></div>
<div class="m" style="left: 620px; top: 80px; width: 260px; height: 260px; transform: rotate(25deg);"></div>
<div style="position: absolute; left: 900px; top: 420px; width: 300px; height: 200px; border-radius: 40px;
     overflow: hidden; background-color: #20304080;"><div class="m" style="position: relative; width: 400px;
     height: 300px;"></div></div>
</body></rml>)";

// A 64x64 TGA (stb_image reads it, as the UI's PNGs): a gradient and a disc, half transparent at the edge.
std::vector<unsigned char> test_image() {
    const int n = 64;
    std::vector<unsigned char> f(18 + size_t(n) * n * 4, 0);
    f[2] = 2;  // uncompressed true colour
    f[12] = uint8_t(n);
    f[14] = uint8_t(n);
    f[16] = 32;
    f[17] = 0x28;  // 8 alpha bits, top-down
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            unsigned char* p = f.data() + 18 + (size_t(y) * n + size_t(x)) * 4;
            const float dx = float(x) - 31.5f, dy = float(y) - 31.5f, r = std::sqrt(dx * dx + dy * dy);
            p[0] = uint8_t(255 - x * 4);           // B
            p[1] = uint8_t(y * 4);                 // G
            p[2] = uint8_t(r < 20.f ? 255 : 60);   // R
            p[3] = uint8_t(r < 28.f ? 255 : r < 32.f ? 128 : 0);
        }
    return f;
}

void write_bmp(const std::filesystem::path& path, const std::vector<unsigned char>& rgba, int w, int h) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t data = uint32_t(w) * uint32_t(h) * 4;
    unsigned char hdr[54] = {'B', 'M'};
    const auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(-h));  // top-down
    hdr[26] = 1;
    hdr[28] = 32;
    put32(34, data);
    std::fwrite(hdr, 1, sizeof hdr, f);
    std::vector<unsigned char> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);
}

struct Page {
    std::string name;
    int w = 1280, h = 720;
    bool hdr = false;
    float hdr_scale = 1.f;
    bool hdr_srgb = false;
    std::function<void()> open;
    std::function<void(Tee&)> draws;  // before end_frame
    bool still = false;                // drawn alike in every frame (the test's own pages: no animation, fixed times)
};

std::vector<Rml::ElementDocument*> gDocuments;  // the pages' own documents, closed after each page

void open_rml(const char* rml) {
    std::string text = rml;
    text.insert(text.find("<style>") + 7, kStyle);
    if (Rml::ElementDocument* d = ui::host::context()->LoadDocumentFromMemory(text, "[ui_render_compare]")) {
        d->Show();
        gDocuments.push_back(d);
    } else {
        std::fprintf(stderr, "ui_render_compare: a page's document did not load\n");
    }
}

void open_settings(int tab, bool prelaunch) {
    auto& w = static_cast<ui::SettingsWindow&>(ui::push_document(std::make_unique<ui::SettingsWindow>(prelaunch)));
    if (tab > 0 && !w.set_active_tab(tab)) std::fprintf(stderr, "ui_render_compare: no tab %d\n", tab);  // (0: open)
}

void close_pages() {
    ui::close_all_documents();
    for (Rml::ElementDocument* d : gDocuments) d->Close();
    gDocuments.clear();
}

// The models, drawn as host.cpp draws the sword cursor and the lizard, at fixed times.
void draw_models(Tee& tee) {
    if (const ui::models::Model* m = ui::models::get("moonlight")) {
        float lo[3], hi[3];
        ui::models::bounds(*m, lo, hi);
        ui::ModelView v;
        std::memcpy(v.rotation, ui::models::look("moonlight", 0.f).rotation, sizeof v.rotation);
        v.pivot[1] = lo[1];
        v.at[0] = 1000.f;
        v.at[1] = 160.f;
        v.scale = hi[1] > lo[1] ? 220.f / (hi[1] - lo[1]) : 0.f;
        if (v.scale > 0.f) tee.draw_model("moonlight", v);
    }
    ui::ModelView v;
    v.time = 1.37;
    std::memcpy(v.rotation, ui::models::look("lizard", 40.f).rotation, sizeof v.rotation);
    v.turning = ui::models::spin("lizard") != 0.f;
    v.at[0] = 60.f;
    v.at[1] = 380.f;
    v.size[0] = v.size[1] = 300.f;
    tee.draw_model("lizard", v);
    ui::ModelView w;
    std::memcpy(w.rotation, ui::models::look("moonlight", 30.f).rotation, sizeof w.rotation);
    w.at[0] = 640.f;
    w.at[1] = 400.f;
    w.size[0] = 240.f;
    w.size[1] = 280.f;
    tee.draw_model("moonlight", w);
}

std::filesystem::path env_path(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? std::filesystem::path(v) : std::filesystem::path();
}

}  // namespace

int main(int argc, char** argv) {
    bool warp = false, verbose = false;
    int frames = 24, probe_x = -1, probe_y = -1;
    std::filesystem::path bmp, scratch;
    std::string only;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::max(2, std::atoi(argv[++i]));
        else if (a == "--bmp" && i + 1 < argc) bmp = argv[++i];
        else if (a == "--scratch" && i + 1 < argc) scratch = argv[++i];
        else if (a == "--page" && i + 1 < argc) only = argv[++i];
        else if (a == "--probe" && i + 2 < argc) {
            probe_x = std::atoi(argv[++i]);
            probe_y = std::atoi(argv[++i]);
        }
        else {
            std::fprintf(stderr, "usage: ui_render_compare [--warp] [--frames N] [--bmp DIR] [--scratch DIR] [--page NAME] "
                                 "[--verbose]\n");
            return 2;
        }
    }
    // The UI's settings and data in a folder of the test's own; no input of the user's.
    if (scratch.empty()) scratch = std::filesystem::temp_directory_path() / "ui_render_compare";
    std::error_code ec;
    std::filesystem::create_directories(scratch / "data", ec);
    std::filesystem::create_directories(scratch / "setup", ec);
    if (!bmp.empty()) std::filesystem::create_directories(bmp, ec);
    _putenv_s("DES_DATA_DIR", (scratch / "data").string().c_str());
    _putenv_s("DES_SETTINGS", (scratch / "settings.ini").string().c_str());
    _putenv_s("DES_SETUP_DATA", (scratch / "setup").string().c_str());
    _putenv_s("PS3_NO_USER_INPUT", "1");
    _putenv_s("DES_UI_NO_ANIM", "1");
    const bool models = !env_path("PS3_VFS_ROOT").empty();

    Device d;
    std::string adapter;
    if (!d.create(warp, adapter)) {
        std::fprintf(stderr, "ui_render_compare: no D3D12 device%s\n", warp ? " (WARP)" : "");
        return 2;
    }
    std::printf("adapter: %s\n", adapter.c_str());
    if (urc_hooks_install(d.list) != 0) {
        std::fprintf(stderr, "ui_render_compare: the command list's calls could not be counted\n");
        return 2;
    }

    // The third renderer's device: the layer's own over the same D3D12 objects, its back buffers named as textures.
    gpu_device_desc gd{};
    gd.cmd_slots = 3;
    gpu_device* gdev = gpu_d3d12_create_on(d.dev, d.queue, &gd);
    if (!gdev) {
        std::fprintf(stderr, "ui_render_compare: no device of the GPU layer\n");
        return 2;
    }
    std::map<void*, std::pair<gpu_texture, gpu_texture_desc>> named;
    const auto back_buffers = [&](void* res, unsigned) -> ui::gpu::BackBuffer {
        auto it = named.find(res);
        if (it == named.end()) {
            gpu_texture_desc td{};
            const gpu_texture t = gpu_d3d12_texture_on(gdev, res, &td);
            if (!t.id) return {};
            it = named.emplace(res, std::make_pair(t, td)).first;
        }
        const gpu_texture_desc& td = it->second.second;
        return {it->second.first.id, int(td.width), int(td.height), td.format};
    };

    Tee* tee = nullptr;
    const bool started = ui::host::init(
        [&](ui::ImageSource images) -> std::unique_ptr<ui::Renderer> {
            auto t = std::make_unique<Tee>();
            g_urc_who = 0;
            t->renderers.push_back(ui::d3d12::create(d.dev, d.queue, images));
            g_urc_who = 1;
            t->renderers.push_back(ui::gpu::create_d3d12(d.dev, d.queue, images));
            g_urc_who = 2;
            t->renderers.push_back(ui::gpu::create(gdev, 0, back_buffers, images));
            g_urc_who = kControl;
            t->renderers.push_back(ui::d3d12::create(d.dev, d.queue, images));
            g_urc_who = -1;
            for (int i = 0; i < kRenderers; i++)
                if (!t->renderers[size_t(i)]) {
                    std::fprintf(stderr, "ui_render_compare: the %s renderer did not start\n", kRendererName[i]);
                    return nullptr;
                }
            tee = t.get();
            return t;
        },
        nullptr, 1280, 720);
    if (!started || !tee || !ui::initialize()) {
        std::fprintf(stderr, "ui_render_compare: the UI did not start\n");
        return 2;
    }
    static Clock clock;
    Rml::SetSystemInterface(&clock);
    const std::vector<unsigned char> image = test_image();
    ui::host::add_png("compare:image", image.data(), image.size());

    std::vector<Page> pages;
    const char* const kTabs[] = {"display", "game", "controls", "network", "interface", "about"};
    for (int t = 0; t < 6; t++) pages.push_back({std::string("settings-") + kTabs[t], 1280, 720, false, 1.f, false, [t] { open_settings(t, false); }, {}});
    pages.push_back({"settings-prelaunch", 1280, 720, false, 1.f, false, [] { open_settings(0, true); }, {}});
    pages.push_back({"setup-start", 1280, 720, false, 1.f, false, [] {
        ui::push_document(std::make_unique<ui::SetupStartWindow>(ui::SetupStartWindow::Props{}));
    }, {}});
    pages.push_back({"setup-source", 1280, 720, false, 1.f, false, [] {
        ui::push_document(std::make_unique<ui::SetupSourceWindow>(ui::SetupSourceWindow::Props{{}, [](ui::setup::Request) {}}));
    }, {}});
    pages.push_back({"setup-files", 1280, 720, false, 1.f, false, [] {
        ui::SetupFilesWindow::Props p;
        p.onStart = [](ui::setup::Request) {};
        ui::push_document(std::make_unique<ui::SetupFilesWindow>(std::move(p)));
    }, {}});
    pages.push_back({"setup-help", 1280, 720, false, 1.f, false, [] {
        ui::push_document(std::make_unique<ui::SetupHelpWindow>(ui::SetupHelpWindow::Props{[] {}, [] {}}));
    }, {}});
    pages.push_back({"setup-before", 1280, 720, false, 1.f, false, [] {
        ui::SetupBeforeWindow::Props p;
        p.onStart = [](ui::setup::Request) {};
        ui::push_document(std::make_unique<ui::SetupBeforeWindow>(std::move(p)));
    }, {}});
    pages.push_back({"clip", 1280, 720, false, 1.f, false, [] { open_rml(kClipPage); }, {}, true});
    pages.push_back({"filters", 1280, 720, false, 1.f, false, [] { open_rml(kFilterPage); }, {}, true});
    pages.push_back({"filters-odd-size", 1001, 563, false, 1.f, false, [] { open_rml(kFilterPage); }, {}, true});
    pages.push_back({"hdr-settings", 1280, 720, true, 2.5f, true, [] { open_settings(4, false); }, {}});
    pages.push_back({"hdr-filters", 1280, 720, true, 1.8f, false, [] { open_rml(kFilterPage); }, {}, true});
    pages.push_back({"hdr-clip", 1280, 720, true, 3.0f, true, [] { open_rml(kClipPage); }, {}, true});
    if (models) {
        pages.push_back({"models", 1280, 720, false, 1.f, false, [] { open_rml(kModelPage); }, draw_models, true});
        pages.push_back({"hdr-models", 1280, 720, true, 2.0f, true, [] { open_rml(kModelPage); }, draw_models, true});
    } else {
        std::printf("models: PS3_VFS_ROOT is not set, so no models page\n");
    }
    if (!only.empty())  // --page: the pages whose names hold it
        pages.erase(std::remove_if(pages.begin(), pages.end(), [&](const Page& p) { return p.name.find(only) == std::string::npos; }),
                    pages.end());

    Targets targets;
    int failures = 0, steady_only = 0;
    std::vector<uint8_t> pixels[kRenderers];
    int dummy = 0;
    for (const Page& p : pages) {
        if (targets.w != p.w || targets.h != p.h || targets.hdr != p.hdr) {
            ui::host::release_buffers();
            for (auto& [res, t] : named) gpu_texture_destroy(gdev, t.first, 0);  // (its renderer is past them)
            named.clear();
            targets.destroy(d);
            if (!targets.create(d, p.w, p.h, p.hdr)) {
                std::fprintf(stderr, "ui_render_compare: no %dx%d targets\n", p.w, p.h);
                return 2;
            }
        }
        for (int i = 0; i < kRenderers; i++) tee->targets[i] = targets.target[i];
        ui::host::set_hdr(p.hdr_scale, p.hdr_srgb);
        unsigned long long before[URC_RENDERERS][URC_COUNT];
        std::memcpy(before, g_urc_count, sizeof before);
        const int mismatches = tee->mismatches;
        p.open();
        tee->before_end = p.draws;
        uint64_t diff[kRenderers] = {}, drawn = 0, changed[kRenderers] = {};
        int first_x = -1, first_y = -1, first_r = 0;
        // Where a renderer's own picture changed from one frame to the next, and where the renderers' pictures
        // differed. Some GPUs do not draw every frame of a still page alike (today's renderer's models on its MSAA
        // layers, on an NVIDIA GPU: a few pixels in a hundred frames, by one step): on a still page (p.still) a pixel
        // where the renderers differ but some renderer's own picture changed between frames drawn alike is the GPU's
        // doing; one where they differ and every renderer drew it the same in every frame is a difference of the code
        // (render_gpu.cpp's). WARP draws alike every time.
        std::vector<uint8_t> previous[kRenderers], unsteady, differs;
        for (int f = 0; f < frames; f++) {
            clock.now += 1.0 / 60.0;
            targets.fill(d);
            ui::update();
            ui::host::render(&dummy, 0, p.w, p.h, nullptr);
            targets.read(d, pixels);
            const size_t px = p.hdr ? 8 : 4, npx = size_t(p.w) * size_t(p.h);
            unsteady.resize(npx);
            differs.resize(npx);
            for (int i = 0; i < kRenderers; i++) {
                if (f >= 1 && previous[i].size() == pixels[i].size())
                    for (size_t q = 0; q < npx; q++)
                        if (std::memcmp(&previous[i][q * px], &pixels[i][q * px], px) != 0) {
                            changed[i]++;
                            unsteady[q] = 1;
                        }
                previous[i] = pixels[i];
            }
            if (probe_x >= 0 && probe_x < p.w && probe_y >= 0 && probe_y < p.h) {  // --probe: one pixel in each frame
                std::printf("  frame %d (%d, %d):", f, probe_x, probe_y);
                for (int i = 0; i < kRenderers; i++) {
                    std::printf("  %s", kRendererName[i]);
                    for (size_t b = 0; b < px && !pixels[i].empty(); b++)
                        std::printf(" %u", unsigned(pixels[i][(size_t(probe_y) * size_t(p.w) + size_t(probe_x)) * px + b]));
                }
                std::printf("\n");
            }
            for (int i = 1; i < kRenderers; i++) {
                if (pixels[i].size() != pixels[0].size() || pixels[0].empty()) {
                    diff[i] += 1;
                    continue;
                }
                int shown = 0;
                for (size_t k = 0; k < pixels[0].size(); k++)
                    if (pixels[i][k] != pixels[0][k]) {
                        const int x = int((k / px) % size_t(p.w)), y = int((k / px) / size_t(p.w));
                        if (i != kControl) differs[k / px] = 1;
                        if (!diff[i]++ && first_x < 0 && i != kControl) {
                            first_x = x;
                            first_y = y;
                            first_r = i;
                        }
                        if (verbose && shown++ < 8)
                            std::printf("  frame %d %s byte %zu of (%d, %d): %u, d3d12 %u\n", f, kRendererName[i],
                                        k % px, x, y, unsigned(pixels[i][k]), unsigned(pixels[0][k]));
                    }
            }
            if (f + 1 == frames) {  // what the UI drew over the background, for a page that shows something
                for (size_t k = 0; k < pixels[0].size() && k < targets.bg.size(); k += px)
                    drawn += std::memcmp(&pixels[0][k], &targets.bg[k], px) != 0;
                // The renderers' own read backs of their last frames.
                std::vector<unsigned char> rb[kRenderers];
                bool rb_ok = true;
                tee->each([&](int i, ui::Renderer& r) { rb_ok = r.read_back(targets.target[i], 0, p.w, p.h, rb[i]) && rb_ok; });
                for (int i = 1; i < kRenderers; i++)
                    if (!rb_ok || rb[i] != rb[0]) diff[i] += 1;
                if (!bmp.empty() && rb_ok) {
                    write_bmp(bmp / (p.name + ".bmp"), rb[0], p.w, p.h);
                    for (int i = 1; i < kRenderers; i++)
                        if (diff[i]) write_bmp(bmp / (p.name + "-" + kRendererName[i] + ".bmp"), rb[i], p.w, p.h);
                }
            }
        }
        close_pages();
        tee->before_end = {};
        const int page_mismatches = tee->mismatches - mismatches;
        bool counts_same = true;
        for (int k = 0; k < URC_COUNT; k++)
            for (int i = 1; i < kRenderers; i++)
                counts_same = counts_same && g_urc_count[i][k] - before[i][k] == g_urc_count[0][k] - before[0][k];
        size_t differ_px = 0, steady_differ_px = 0, unsteady_px = 0;
        for (size_t q = 0; q < differs.size(); q++) {
            differ_px += differs[q];
            steady_differ_px += differs[q] && !unsteady[q];
            unsteady_px += unsteady[q];
        }
        const bool every_frame = !diff[1] && !diff[2];
        // (a still page only: its frames are drawn alike, so a change between them is the GPU's)
        const bool device_only = p.still && differ_px && !steady_differ_px;
        const bool same = (every_frame || device_only) && !page_mismatches && counts_same;
        failures += !same;
        steady_only += same && !every_frame;
        std::printf("page %-20s %4dx%-4d %s  frames %d  drawn %llu px  differing bytes: gpu %llu, gpu-dev %llu "
                    "(d3d12-again %llu)  %s\n",
                    p.name.c_str(), p.w, p.h, p.hdr ? "FP16" : "RGBA8", frames, (unsigned long long)drawn,
                    (unsigned long long)diff[1], (unsigned long long)diff[2], (unsigned long long)diff[kControl],
                    !same ? "DIFFERENT" : every_frame ? "SAME" : "SAME where the GPU draws alike (it varies between frames)");
        if (first_x >= 0)
            std::printf("  first difference: %s at (%d, %d)\n", kRendererName[first_r], first_x, first_y);
        if (p.still && (unsteady_px || differ_px))
            std::printf("  still page: own changes between frames (px) d3d12 %llu, gpu %llu, gpu-dev %llu, d3d12-again "
                        "%llu, at %zu px; renderers differ at %zu px, %zu of them where every renderer drew alike in "
                        "every frame\n",
                        (unsigned long long)changed[0], (unsigned long long)changed[1], (unsigned long long)changed[2],
                        (unsigned long long)changed[kControl], unsteady_px, differ_px, steady_differ_px);
        if (page_mismatches) std::printf("  %d calls whose results differed between the renderers\n", page_mismatches);
        std::printf("  %-34s %10s %10s %10s %12s\n", "calls", kRendererName[0], kRendererName[1], kRendererName[2],
                    kRendererName[3]);
        for (int k = 0; k < URC_COUNT; k++) {
            unsigned long long n[kRenderers];
            for (int i = 0; i < kRenderers; i++) n[i] = g_urc_count[i][k] - before[i][k];
            if (!n[0] && !n[1] && !n[2] && !n[3]) continue;
            std::printf("  %-34s %10llu %10llu %10llu %12llu%s\n", g_urc_name[k], n[0], n[1], n[2], n[3],
                        n[1] == n[0] && n[2] == n[0] && n[3] == n[0] ? "" : "  <- differs");
        }
        std::fflush(stdout);
    }

    ui::shutdown();
    ui::host::shutdown();
    for (auto& [res, t] : named) gpu_texture_destroy(gdev, t.first, 0);
    named.clear();
    gpu_destroy(gdev);
    targets.destroy(d);
    urc_hooks_remove();
    d.destroy();
    std::printf("%s: %d of %zu pages differ; %d the same in their steady pictures only\n", failures ? "FAILED" : "PASSED",
                failures, pages.size(), steady_only);
    return failures ? 1 : 0;
}
