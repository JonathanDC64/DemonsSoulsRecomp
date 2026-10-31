// Written for Demon's Souls. What draws the wrapper UI over the game (host.cpp, docs/76): RmlUi's renderer on the
// game's D3D12 device and queue (render_d3d12.cpp), on every platform; on Windows, for one release, also RmlUi's own
// D3D11 renderer on a D3D11On12 device (render_d3d11.cpp, DES_UI_RENDER=d3d11), the way the menus were drawn before.
#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <functional>
#include <memory>
#include <vector>

namespace ui {

// An image RmlUi asks for by name (an <img src>): the file's bytes (PNG, TGA or JPEG), or false if there is none.
using ImageSource = std::function<bool(const Rml::String& source, std::vector<unsigned char>& file)>;

// Where one of the game's models goes (draw_model, docs/112). rotation: model space to the screen's (x right, y down,
// z toward the viewer), its rows. Then either its point `pivot` at `at`, `scale` pixels to the model's unit, or, with
// scale 0, the whole model (every frame of its animation) fitted into the box from `at`, `size` pixels; `turning`: as
// it would fit at any turn about its up (model y), so a model that spins keeps its size.
struct ModelView {
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float pivot[3] = {0, 0, 0};
    float at[2] = {0, 0};
    float size[2] = {0, 0};
    float scale = 0;
    bool turning = false;
    double time = 0;  // seconds into its animation (looped)
};

class Renderer {
public:
    virtual ~Renderer() = default;

    // What RmlUi draws through (Rml::SetRenderInterface).
    virtual Rml::RenderInterface* rml() = 0;

    // A frame: begin_frame, the context's Render, end_frame. The UI is drawn over back_buffer (the swap chain's
    // ID3D12Resource* number `index`, in the PRESENT state on entry and on exit, width x height), and end_frame
    // submits the frame's work to the game's queue, ahead of its Present. False from begin_frame: no frame (skip the
    // Render and the end_frame).
    virtual bool begin_frame(void* back_buffer, unsigned index, int width, int height) = 0;
    virtual void end_frame() = 0;

    // Copies the back buffer (PRESENT state) into rgba (width * height * 4 bytes, rows top down). Waits for the GPU.
    virtual bool read_back(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba) = 0;

    // Before the window's buffers are resized: waits for the UI's work on them and forgets their views.
    virtual void release_back_buffers() = 0;

    // HDR output (RSX_HDR): on an FP16 scRGB back buffer the UI's colours are decoded (the sRGB curve if srgb, else
    // gamma 2.2) and multiplied by scale, so its white sits at the game's paper white. The D3D11 renderer ignores it.
    virtual void set_hdr(float scale, bool srgb) {
        (void)scale;
        (void)srgb;
    }

    // One of the game's models (souls_model.hpp: "moonlight", "lizard") over what the frame has so far, between the
    // context's Render and end_frame, with no scissor or clip. False if it is not drawn: the game's files do not have
    // it, or this renderer draws none (the D3D11 one).
    virtual bool draw_model(const char* name, const ModelView& view) {
        (void)name;
        (void)view;
        return false;
    }
};

namespace d3d12 {
// device: the ID3D12Device*, queue: the game's ID3D12CommandQueue*. images: images not among RmlUi's files. Null if
// the device cannot make what the renderer needs.
std::unique_ptr<Renderer> create(void* device, void* queue, ImageSource images);
}  // namespace d3d12

#ifdef _WIN32
namespace d3d11 {
// The same, through D3D11On12 (no debug layer). Null if D3D11On12 cannot start.
std::unique_ptr<Renderer> create(void* device, void* queue, ImageSource images);
}  // namespace d3d11
#endif

}  // namespace ui

#if defined(PS3RECOMP_RSX_HAL)
#include <cstdint>

struct gpu_device;

// The menus on ps3recomp's GPU layer (render_gpu.cpp, DES_UI_RENDER=gpu): render_d3d12.cpp's drawing through gpu.h,
// on any of its backends. Built with PS3RECOMP_RSX_HAL only.
namespace ui::gpu {
// A back buffer as one of the device's textures: its handle (gpu_texture's id; 0 for none), size and format (gpu.h's
// numbers). In GPU_STATE_PRESENT on entry to begin_frame and on exit from end_frame, as the D3D12 ones.
struct BackBuffer {
    uint32_t texture = 0;
    int width = 0, height = 0;
    uint32_t format = 0;
};
// begin_frame's back_buffer and index, as a texture.
using BackBuffers = std::function<BackBuffer(void* back_buffer, unsigned index)>;

// On a device of the caller's, recording on its slots first_slot to first_slot + 2 (gpu_cmd_begin), one submit a
// frame. Null if the device cannot make what the renderer needs.
std::unique_ptr<Renderer> create(gpu_device* device, unsigned first_slot, BackBuffers back_buffers, ImageSource images);
// On the D3D12 device and queue d3d12::create takes: a device of the layer's over them (gpu_d3d12_create_on), the
// back buffers their ID3D12Resource*s. Null off Windows.
std::unique_ptr<Renderer> create_d3d12(void* device, void* queue, ImageSource images);
}  // namespace ui::gpu
#endif
