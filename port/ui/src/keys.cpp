// Written for Demon's Souls. See keys.hpp.
#include "keys.hpp"

#ifdef _WIN32

#include "RmlUi_Platform_Win32.h"

#include <windows.h>

namespace ui::keys {

Rml::Input::KeyIdentifier to_rml(int vk) {
    return RmlWin32::ConvertKey(vk);
}

int modifiers() {
    return RmlWin32::GetKeyModifierState();
}

bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::string key_name(int vk) {
    LONG lp = LONG(MapVirtualKeyA(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE:
    case VK_NUMLOCK:
        lp |= 1 << 24;  // extended keys
        break;
    default: break;
    }
    char name[64];
    if (GetKeyNameTextA(lp, name, sizeof name) > 0) return name;
    return {};
}

}  // namespace ui::keys

#else

#include "platform.hpp"

#include <SDL.h>

extern "C" int cellPad_host_key_down(int vk);  // ps3recomp, libs/input/cellPad.h: SDL's keyboard and mouse state

namespace ui::keys {

// RmlWin32::ConvertKey's mapping, for the keys the runtime's SDL window sends (rsx_d3d12_backend.c, win_vk).
Rml::Input::KeyIdentifier to_rml(int vk) {
    using namespace Rml::Input;
    if (vk >= 'A' && vk <= 'Z') return KeyIdentifier(KI_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return KeyIdentifier(KI_0 + (vk - '0'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return KeyIdentifier(KI_NUMPAD0 + (vk - VK_NUMPAD0));
    if (vk >= VK_F1 && vk <= VK_F24) return KeyIdentifier(KI_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_BACK: return KI_BACK;
    case VK_TAB: return KI_TAB;
    case VK_RETURN: return KI_RETURN;
    case VK_SHIFT: return KI_LSHIFT;
    case VK_CONTROL: return KI_LCONTROL;
    case VK_MENU: return KI_LMENU;
    case VK_LSHIFT: return KI_LSHIFT;
    case VK_RSHIFT: return KI_RSHIFT;
    case VK_LCONTROL: return KI_LCONTROL;
    case VK_RCONTROL: return KI_RCONTROL;
    case VK_LMENU: return KI_LMENU;
    case VK_RMENU: return KI_RMENU;
    case VK_PAUSE: return KI_PAUSE;
    case VK_CAPITAL: return KI_CAPITAL;
    case VK_ESCAPE: return KI_ESCAPE;
    case VK_SPACE: return KI_SPACE;
    case VK_PRIOR: return KI_PRIOR;
    case VK_NEXT: return KI_NEXT;
    case VK_END: return KI_END;
    case VK_HOME: return KI_HOME;
    case VK_LEFT: return KI_LEFT;
    case VK_UP: return KI_UP;
    case VK_RIGHT: return KI_RIGHT;
    case VK_DOWN: return KI_DOWN;
    case VK_SNAPSHOT: return KI_SNAPSHOT;
    case VK_INSERT: return KI_INSERT;
    case VK_DELETE: return KI_DELETE;
    case VK_LWIN: return KI_LWIN;
    case VK_RWIN: return KI_RWIN;
    case VK_APPS: return KI_APPS;
    case VK_MULTIPLY: return KI_MULTIPLY;
    case VK_ADD: return KI_ADD;
    case VK_SUBTRACT: return KI_SUBTRACT;
    case VK_DECIMAL: return KI_DECIMAL;
    case VK_DIVIDE: return KI_DIVIDE;
    case VK_NUMLOCK: return KI_NUMLOCK;
    case VK_SCROLL: return KI_SCROLL;
    case VK_OEM_1: return KI_OEM_1;
    case VK_OEM_PLUS: return KI_OEM_PLUS;
    case VK_OEM_COMMA: return KI_OEM_COMMA;
    case VK_OEM_MINUS: return KI_OEM_MINUS;
    case VK_OEM_PERIOD: return KI_OEM_PERIOD;
    case VK_OEM_2: return KI_OEM_2;
    case VK_OEM_3: return KI_OEM_3;
    case VK_OEM_4: return KI_OEM_4;
    case VK_OEM_5: return KI_OEM_5;
    case VK_OEM_6: return KI_OEM_6;
    case VK_OEM_7: return KI_OEM_7;
    default: return KI_UNKNOWN;
    }
}

int modifiers() {
    using namespace Rml::Input;
    const SDL_Keymod m = SDL_GetModState();
    int out = 0;
    if (m & KMOD_CAPS) out |= KM_CAPSLOCK;
    if (m & KMOD_NUM) out |= KM_NUMLOCK;
    if (m & KMOD_SHIFT) out |= KM_SHIFT;
    if (m & KMOD_CTRL) out |= KM_CTRL;
    if (m & KMOD_ALT) out |= KM_ALT;
    return out;
}

bool key_down(int vk) {
    return cellPad_host_key_down(vk) != 0;
}

std::string key_name(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_CAPITAL: return "Caps Lock";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    case VK_PAUSE: return "Pause";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num Del";
    case VK_DIVIDE: return "Num /";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    default: break;
    }
    return {};
}

}  // namespace ui::keys

#endif
