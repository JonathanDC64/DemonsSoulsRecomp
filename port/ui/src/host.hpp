// The UI's connection to the game: the RmlUi context, its renderer on the game's D3D12 device and queue
// (renderer.hpp), the system and file interfaces, the embedded files and the scale. FFCCEOT-Recomp's host (GL3 in an
// SDL window) rewritten for the ps3recomp D3D12 backend's overlay hook. Adapted for Demon's Souls.
#pragma once

#include <RmlUi/Core.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ui::host {

// On the render thread, inside the overlay frame hook (the device and the queue exist by then). hwnd: the game's
// window (an HWND on Windows, an SDL_Window* elsewhere).
bool init(void* d3d12_device, void* d3d12_queue, void* hwnd, int width, int height);
void shutdown();
bool is_initialized();

Rml::Context* context();
// The window init was given (an HWND on Windows, an SDL_Window* elsewhere), or null before init: what a dialog of the
// system's belongs to (folder_picker.hpp).
void* window();

// The UI's size: 0 is automatic (1 dp = 1 px at the display's DPI), anything else multiplies that.
void set_ui_scale(float scale);
// While on, the UI's base scale also follows the window's height, 720 px = 1 dp a px (never below the DPI's): for
// documents drawn as the whole window, the setup screens (Document::scales_with_window). ui::update sets it.
void set_follow_height(bool on);

// The window's DPI (96 is 100%), 0 if not known.
unsigned window_dpi();

// Updates the context and draws the documents into the back buffer (D3D12, PRESENT state on entry and on exit). With
// shot_path, also writes the result (game frame and UI) to that BMP file.
void render(void* back_buffer, unsigned index, int width, int height, const char* shot_path);

// Before the window's buffers are resized: finishes the work using them and drops the views of them. The next render
// makes views of the new buffers.
void release_buffers();

// Writes the back buffer as it is (no UI drawn) to a BMP file.
void screenshot(void* back_buffer, unsigned index, int width, int height, const char* shot_path);

// Reads the back buffer as it is (no UI drawn) into `rgba` (width * height * 4 bytes, rows top down): the picture of
// a report (report.cpp). Waits for the GPU. False if it could not be read.
bool capture(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba);

// What the on-screen keyboard offers (unused on a desktop).
enum class InputType { Text, Number };
void set_input_type(InputType type);

// False when DES_UI_NO_ANIM is set: documents then open and close without their transitions.
bool animations_enabled();

// Serves a PNG file's bytes (copied) as the image `source`, for an <img src="..."> such as "trophy-icon:5". They are
// decoded with stb_image when RmlUi first draws them, and RmlUi keeps the texture. Any thread.
void add_png(const std::string& source, const void* png, size_t size);

}  // namespace ui::host
