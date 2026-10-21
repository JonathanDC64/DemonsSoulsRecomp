// Written for Demon's Souls. RmlUi's renderer on D3D12 (render_d3d12.cpp, docs/76): what draws the wrapper UI over the
// game where there is no D3D11On12 (the D3D12 backend on vkd3d, off Windows).
#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <functional>
#include <memory>
#include <vector>

namespace ui::d3d12 {

// An image RmlUi asks for by name (an <img src>): the file's bytes (PNG, TGA or JPEG), or false if there is none.
using ImageSource = std::function<bool(const Rml::String& source, std::vector<unsigned char>& file)>;

class Renderer : public Rml::RenderInterface {
public:
    // device: the ID3D12Device*, queue: the game's ID3D12CommandQueue* (the UI's work goes to it ahead of Present).
    // images: images not among RmlUi's files. Null if the device cannot make what the renderer needs.
    static std::unique_ptr<Renderer> create(void* device, void* queue, ImageSource images);
    ~Renderer() override = default;

    // A frame: begin_frame, the context's Render, end_frame. end_frame draws the UI over back_buffer (an
    // ID3D12Resource*, in the PRESENT state on entry and on exit, width x height) and submits the frame's work.
    virtual void begin_frame(int width, int height) = 0;
    virtual void end_frame(void* back_buffer) = 0;

    // Copies back_buffer (PRESENT state) into rgba (width * height * 4 bytes, rows top down). Waits for the GPU.
    virtual bool read_back(void* back_buffer, int width, int height, std::vector<unsigned char>& rgba) = 0;

    // Before the window's buffers are resized: waits for the UI's work on them and forgets their views.
    virtual void release_back_buffers() = 0;
};

}  // namespace ui::d3d12
