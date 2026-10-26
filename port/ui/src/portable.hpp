// Written for Demon's Souls. Small helpers the UI uses on every platform in place of Windows-only calls (docs/76): a
// steady clock in milliseconds, a comparison that ignores case, and the local time.
#pragma once

#include <cctype>
#include <chrono>
#include <ctime>

namespace ui {

// Milliseconds on a steady clock: only differences mean anything.
inline unsigned long long now_ms() {
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The same name, ignoring the case of ASCII letters.
inline bool same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}

// The local time of `t`; false if there is none.
inline bool local_time(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

}  // namespace ui
