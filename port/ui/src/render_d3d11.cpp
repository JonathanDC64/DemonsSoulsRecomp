// The menus' old renderer on Windows (renderer.hpp), kept for one release behind DES_UI_RENDER=d3d11 in case the
// D3D12 one (render_d3d12.cpp) shows a problem on someone's PC. RmlUi's stock D3D11 renderer draws on a D3D11On12
// device that shares the game's D3D12 device and command queue; each swap chain back buffer is wrapped as a D3D11
// render target, and the UI is composited over the finished game frame (premultiplied alpha) just before Present.
// Adapted for Demon's Souls from FFCCEOT-Recomp's host.cpp.
#include "RmlUi_Renderer_DX11.h"

#include "renderer.hpp"

#include <d3d11on12.h>
#include <d3d12.h>

#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace ui::d3d11 {
namespace {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// RmlUi's D3D11 renderer, which reads TGA files only, plus the images the host serves (decoded with stb_image).
class Dx11 final : public RenderInterface_DX11 {
public:
    Dx11(ID3D11Device* device, ImageSource images) : RenderInterface_DX11(device), mImages(std::move(images)) {}

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        std::vector<unsigned char> file;
        if (!mImages || !mImages(source, file)) return RenderInterface_DX11::LoadTexture(dimensions, source);
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
        if (!px) {
            std::fprintf(stderr, "[ui] cannot decode the image %s\n", source.c_str());
            return 0;
        }
        for (size_t i = 0, end = size_t(w) * size_t(h) * 4; i < end; i += 4) {  // (RmlUi's textures are premultiplied)
            const unsigned a = px[i + 3];
            for (int c = 0; c < 3; c++) px[i + c] = stbi_uc((unsigned(px[i + c]) * a + 127) / 255);
        }
        dimensions = Rml::Vector2i(w, h);
        const Rml::TextureHandle t = GenerateTexture(Rml::Span<const Rml::byte>(px, size_t(w) * size_t(h) * 4), dimensions);
        stbi_image_free(px);
        return t;
    }

private:
    ImageSource mImages;
};

struct Wrapped {
    void* source = nullptr;  // the ID3D12Resource it wraps
    ID3D11Resource* resource = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};

class RendererImpl final : public Renderer {
public:
    ~RendererImpl() override {
        release_wrapped();
        mDx11.reset();
        release(mOn12);
        release(mContext);
        release(mDevice);
    }

    bool init(void* d3d12_device, void* d3d12_queue, ImageSource images) {
        IUnknown* queues[] = {static_cast<IUnknown*>(d3d12_queue)};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // No debug layer, ever (the D3D12 debug layer must stay off).
        HRESULT hr = D3D11On12CreateDevice(static_cast<IUnknown*>(d3d12_device), D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                           UINT(std::size(levels)), queues, 1, 0, &mDevice, &mContext, nullptr);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[ui] D3D11On12CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
            return false;
        }
        if (FAILED(mDevice->QueryInterface(IID_PPV_ARGS(&mOn12)))) {
            std::fprintf(stderr, "[ui] no ID3D11On12Device\n");
            return false;
        }
        mDx11 = std::make_unique<Dx11>(mDevice, std::move(images));
        return true;
    }

    Rml::RenderInterface* rml() override { return mDx11.get(); }

    bool begin_frame(void* back_buffer, unsigned index, int width, int height) override {
        mFrame = wrap(back_buffer, index);
        if (!mFrame) return false;
        mOn12->AcquireWrappedResources(&mFrame->resource, 1);
        mDx11->SetViewport(width, height);
        mDx11->BeginFrame();
        return true;
    }

    void end_frame() override {
        if (!mFrame) return;
        mDx11->EndFrame(mFrame->rtv);
        mOn12->ReleaseWrappedResources(&mFrame->resource, 1);
        mFrame = nullptr;
        mContext->Flush();  // submits to the game's queue, ahead of its Present
    }

    bool read_back(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba) override {
        if (width <= 0 || height <= 0) return false;
        Wrapped* w = wrap(back_buffer, index);
        if (!w) return false;
        mOn12->AcquireWrappedResources(&w->resource, 1);
        ID3D11Texture2D* staging = copy_to_staging(*w, width, height);
        mOn12->ReleaseWrappedResources(&w->resource, 1);
        mContext->Flush();
        if (!staging) return false;
        bool ok = false;
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(mContext->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            const size_t row = size_t(width) * 4;
            rgba.resize(row * size_t(height));
            for (int y = 0; y < height; y++)
                std::memcpy(rgba.data() + row * size_t(y), static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch, row);
            mContext->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
        return ok;
    }

    void release_back_buffers() override {
        release_wrapped();
        // D3D11On12 holds the D3D12 resources until the context lets go of them: unbind everything and submit.
        mContext->ClearState();
        mContext->Flush();
    }

private:
    void release_wrapped() {
        for (Wrapped& w : mBuffers) {
            release(w.rtv);
            release(w.resource);
            w.source = nullptr;
        }
    }

    Wrapped* wrap(void* back_buffer, unsigned index) {
        if (index >= std::size(mBuffers) || !back_buffer) return nullptr;
        Wrapped& w = mBuffers[index];
        if (w.source == back_buffer && w.rtv) return &w;
        release(w.rtv);
        release(w.resource);
        w.source = nullptr;
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = mOn12->CreateWrappedResource(static_cast<IUnknown*>(back_buffer), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                                  D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&w.resource));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[ui] CreateWrappedResource failed (0x%08lX)\n", (unsigned long)hr);
            return nullptr;
        }
        hr = mDevice->CreateRenderTargetView(w.resource, nullptr, &w.rtv);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[ui] CreateRenderTargetView failed (0x%08lX)\n", (unsigned long)hr);
            release(w.resource);
            return nullptr;
        }
        w.source = back_buffer;
        return &w;
    }

    // Copies the (acquired) wrapped back buffer to a staging texture, mapped once the work has run.
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
        if (FAILED(mDevice->CreateTexture2D(&desc, nullptr, &staging))) return nullptr;
        mContext->CopyResource(staging, w.resource);
        return staging;
    }

    ID3D11Device* mDevice = nullptr;
    ID3D11DeviceContext* mContext = nullptr;
    ID3D11On12Device* mOn12 = nullptr;
    std::unique_ptr<Dx11> mDx11;
    Wrapped mBuffers[8];
    Wrapped* mFrame = nullptr;  // the frame's back buffer, from begin_frame to end_frame
};

}  // namespace

std::unique_ptr<Renderer> create(void* device, void* queue, ImageSource images) {
    if (!device || !queue) return nullptr;
    auto r = std::make_unique<RendererImpl>();
    if (!r->init(device, queue, std::move(images))) return nullptr;
    return r;
}

}  // namespace ui::d3d11
