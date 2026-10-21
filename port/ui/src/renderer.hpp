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
