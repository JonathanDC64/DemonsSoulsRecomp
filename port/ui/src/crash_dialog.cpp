// The crash dialog (see crash_dialog.hpp). Adapted for Demon's Souls from FFCCEOT-Recomp's crash_dialog.cpp (the
// modal and its actions); the window, the D3D12 swap chain and the input are written for this port, so that the
// dialog is drawn by the same host (host.cpp) as the in-game menu.
#include "crash_dialog.hpp"

#include "RmlUi_Platform_Win32.h"

#include "host.hpp"
#include "input.hpp"
#include "modal.hpp"
#include "ui.hpp"

#include <windows.h>
#include <windowsx.h>
// (after windows.h)
#include <d3d12.h>
#include <dxgi1_4.h>
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace ui::crash_dialog {
namespace {

constexpr int kWidth = 1024, kHeight = 640;
constexpr UINT kBuffers = 2;

bool sClose = false;
int sClientW = kWidth, sClientH = kHeight;

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Rml::Context* ctx = host::context();
    switch (msg) {
    case WM_CLOSE:
        sClose = true;
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (ctx) {
            if (msg == WM_KEYDOWN) ctx->ProcessKeyDown(RmlWin32::ConvertKey(int(wp)), RmlWin32::GetKeyModifierState());
            else ctx->ProcessKeyUp(RmlWin32::ConvertKey(int(wp)), RmlWin32::GetKeyModifierState());
        }
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        if (ctx) {
            // (the swap chain is the client area's size: no scaling)
            ctx->ProcessMouseMove(GET_X_LPARAM(lp) * kWidth / std::max(1, sClientW),
                                  GET_Y_LPARAM(lp) * kHeight / std::max(1, sClientH), RmlWin32::GetKeyModifierState());
            if (msg == WM_LBUTTONDOWN) {
                SetCapture(hwnd);
                ctx->ProcessMouseButtonDown(0, RmlWin32::GetKeyModifierState());
            } else if (msg == WM_LBUTTONUP) {
                ReleaseCapture();
                ctx->ProcessMouseButtonUp(0, RmlWin32::GetKeyModifierState());
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (ctx) ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, -float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA)), 0);
        return 0;
    case WM_SIZE:
        sClientW = LOWORD(lp);
        sClientH = HIWORD(lp);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// The gamepads (XInput) as the menu's pad state: the D-pad and left stick move, Cross (A) confirms, Circle (B) cancels.
input::PadState read_pads() {
    input::PadState pad;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        const WORD b = st.Gamepad.wButtons;
        if (b & XINPUT_GAMEPAD_DPAD_UP) pad.buttons |= 1u << 4;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT) pad.buttons |= 1u << 5;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN) pad.buttons |= 1u << 6;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT) pad.buttons |= 1u << 7;
        if (b & XINPUT_GAMEPAD_B) pad.buttons |= 1u << 13;
        if (b & XINPUT_GAMEPAD_A) pad.buttons |= 1u << 14;
        const float lx = std::clamp(st.Gamepad.sThumbLX / 32767.0f, -1.0f, 1.0f);
        const float ly = std::clamp(-st.Gamepad.sThumbLY / 32767.0f, -1.0f, 1.0f);
        if (std::abs(lx) > std::abs(pad.lx)) pad.lx = lx;
        if (std::abs(ly) > std::abs(pad.ly)) pad.ly = ly;
    }
    return pad;
}

struct Gpu {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12Resource* buffers[kBuffers] = {};
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    UINT rtv_size = 0;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;

    bool create(HWND hwnd) {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = kWidth;
        sd.Height = kHeight;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // (as the game's: the host's picture path copies this format)
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1))) return false;
        const HRESULT hr = sc1->QueryInterface(IID_PPV_ARGS(&swapchain));
        sc1->Release();
        if (FAILED(hr)) return false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBuffers;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) return false;
        rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < kBuffers; i++) {
            if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])))) return false;
            device->CreateRenderTargetView(buffers[i], nullptr, rtv(i));
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
            return false;
        list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtv_size;
        return h;
    }

    // The background, before the host draws the dialog over it (the buffer is in the PRESENT state before and after).
    void clear(UINT i) {
        allocator->Reset();
        list->Reset(allocator, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffers[i];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        const float color[4] = {0.025f, 0.025f, 0.03f, 1.0f};
        list->ClearRenderTargetView(rtv(i), color, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }

    void wait() {
        queue->Signal(fence, ++fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, event);
            WaitForSingleObject(event, 2000);
        }
    }

    void destroy() {
        if (queue && fence) wait();
        if (event) CloseHandle(event);
        release(fence);
        release(list);
        release(allocator);
        for (auto*& b : buffers) release(b);
        release(rtv_heap);
        release(swapchain);
        release(queue);
        release(device);
        release(factory);
    }
};

}  // namespace

bool show(const Content& content, const Actions& actions) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"DesCrashDialog";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, kWidth, kHeight};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - w) / 2), y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Demon's Souls", style, x, y, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return false;

    Gpu gpu;
    bool ok = false;
    if (gpu.create(hwnd) && host::init(gpu.device, gpu.queue, hwnd, kWidth, kHeight) && ui::initialize()) {
        ok = true;
        sClose = false;
        const auto dismiss = [](Modal&) { sClose = true; };
        const auto details = [&content](Modal& modal) {
            modal.push(std::make_unique<Modal>(Modal::Props{
                .title = "Details",
                .bodyRml = escape_lines(content.details),
                .actions = {ModalAction{.label = "Close", .onPressed = [](Modal& d) { d.pop(); }}},
                .onDismiss = [](Modal& d) { d.pop(); },
            }));
        };
        if (content.hang) push_document(std::make_unique<Modal>(Modal::Props{
            .title = "The game is not responding",
            .bodyRml = "<b>" + escape(content.what) +
                       ".</b> The menu cannot open. The game may still recover: keep waiting, or close it.<br/><br/>"
                       "A report was saved on this PC. Reporting the problem opens a new issue on GitHub with the "
                       "details filled in, and a zip to attach with what each part of the game was doing, your settings "
                       "and the game's recent messages. It has no save data or game files; nothing is sent until you "
                       "submit the issue.",
            .actions =
                {
                    ModalAction{.label = "Keep waiting", .onPressed = dismiss},
                    ModalAction{
                        .label = "Report on GitHub",
                        .onPressed =
                            [&actions](Modal& modal) {
                                if (actions.report) modal.set_body_text(actions.report());
                            },
                    },
                    ModalAction{.label = "Show details", .onPressed = details},
                    ModalAction{
                        .label = "Close the game",
                        .onPressed =
                            [&actions](Modal&) {
                                if (actions.close_game) actions.close_game();
                                sClose = true;
                            },
                    },
                },
            .onDismiss = dismiss,
            .variant = "danger",
            .icon = "error",
        }));
        else push_document(std::make_unique<Modal>(Modal::Props{
            .title = "The game has crashed",
            .bodyRml = "<b>" + escape(content.what) +
                       "</b><br/><br/>A report was saved on this PC. Reporting the problem opens a new issue on GitHub "
                       "with the details filled in, and a zip to attach with the stack, your settings (settings.ini) "
                       "and the game's recent messages. It has no save data or game files; nothing is sent until you "
                       "submit the issue.",
            .actions =
                {
                    ModalAction{
                        .label = "Restart game",
                        .onPressed =
                            [&actions](Modal&) {
                                if (actions.restart) actions.restart();
                                sClose = true;
                            },
                    },
                    ModalAction{
                        .label = "Report on GitHub",
                        .onPressed =
                            [&actions](Modal& modal) {
                                if (actions.report) modal.set_body_text(actions.report());
                            },
                    },
                    ModalAction{.label = "Show details", .onPressed = details},
                    ModalAction{.label = "Close", .onPressed = dismiss},
                },
            .onDismiss = dismiss,
            .variant = "danger",
            .icon = "error",
        }));
        // (PS3_NO_USER_INPUT=1, a test run: it never takes the focus from what the user is doing)
        const char* no_input = std::getenv("PS3_NO_USER_INPUT");
        const bool quiet = no_input && std::atoi(no_input) != 0;
        ShowWindow(hwnd, quiet ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        if (!quiet) SetForegroundWindow(hwnd);

        // Test knobs (crash_dialog.hpp).
        const char* shot = std::getenv("DES_CRASH_DIALOG_SHOT");
        const char* exit_ms = std::getenv("DES_CRASH_DIALOG_EXIT_MS");
        const char* auto_report = std::getenv("DES_CRASH_DIALOG_REPORT");
        const auto start = std::chrono::steady_clock::now();
        const long long close_after = exit_ms && *exit_ms ? std::atoll(exit_ms) : 0;
        unsigned long long frame = 0;
        input::reset_input_state();
        while (!sClose) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) sClose = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            ++frame;
            if (frame == 10 && auto_report && *auto_report && *auto_report != '0' && actions.report) {
                if (auto* modal = dynamic_cast<Modal*>(top_document())) modal->set_body_text(actions.report());
            }
            input::update_pad(read_pads(), 0);
            ui::update();
            const UINT index = gpu.swapchain->GetCurrentBackBufferIndex();
            gpu.clear(index);
            const bool take = shot && *shot && frame == 40;
            host::render(gpu.buffers[index], index, kWidth, kHeight, take ? shot : nullptr);
            gpu.swapchain->Present(1, 0);
            gpu.wait();
            if (close_after > 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(close_after))
                sClose = true;
            if (IsIconic(hwnd)) Sleep(50);
        }
        ui::shutdown();
    }
    host::shutdown();
    gpu.destroy();
    DestroyWindow(hwnd);
    return ok;
}

}  // namespace ui::crash_dialog
