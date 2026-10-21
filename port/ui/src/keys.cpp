// Written for Demon's Souls. See keys.hpp.
#include "keys.hpp"

#ifdef _WIN32

#include "RmlUi_Platform_Win32.h"

namespace ui::keys {

Rml::Input::KeyIdentifier to_rml(int vk) {
    return RmlWin32::ConvertKey(vk);
}

int modifiers() {
    return RmlWin32::GetKeyModifierState();
}

}  // namespace ui::keys

#else

#include "platform.hpp"

#include <SDL.h>

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

}  // namespace ui::keys

#endif
