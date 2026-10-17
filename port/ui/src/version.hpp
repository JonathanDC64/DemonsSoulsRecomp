// Written for Demon's Souls (docs/32).
// The port's version, from git at configure time (port/ui/CMakeLists.txt).
#pragma once

#ifndef DES_VERSION
#define DES_VERSION "dev"
#endif

namespace ui {
inline constexpr const char* kVersion = DES_VERSION;
}  // namespace ui
