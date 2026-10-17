// The UI's connection to the game: the RmlUi context, its D3D11 renderer on a D3D11On12 device built on the game's
// D3D12 device and queue, the system and file interfaces, the embedded files and the scale. FFCCEOT-Recomp's host
// (GL3 in an SDL window) rewritten for the ps3recomp D3D12 backend's overlay hook. Adapted for Demon's Souls.
#pragma once

#include <RmlUi/Core.h>

namespace ui::host {

// On the render thread, inside the overlay frame hook (the device and the queue exist by then).
bool init(void* d3d12_device, void* d3d12_queue, void* hwnd, int width, int height);
void shutdown();
bool is_initialized();

Rml::Context* context();

// The UI's size: 0 is automatic (1 dp = 1 px at 720p), anything else multiplies that.
void set_ui_scale(float scale);

// Updates the context and draws the documents into the back buffer (D3D12, PRESENT state on entry and on exit). With
// shot_path, also writes the result (game frame and UI) to that BMP file.
void render(void* back_buffer, unsigned index, int width, int height, const char* shot_path);

// Writes the back buffer as it is (no UI drawn) to a BMP file.
void screenshot(void* back_buffer, unsigned index, int width, int height, const char* shot_path);

// What the on-screen keyboard offers (unused on a desktop).
enum class InputType { Text, Number };
void set_input_type(InputType type);

// False when DES_UI_NO_ANIM is set: documents then open and close without their transitions.
bool animations_enabled();

}  // namespace ui::host
