// Written for Demon's Souls (docs/32, docs/54).
// What this program is: the port's version, the ps3recomp it was built with and the game dump it was built from.
// des_version.h is written on every build by port/cmake/DesVersion.cmake.
#pragma once

#include "des_version.h"

namespace ui {
// "0.1.0" for a release; "0.1.0-dev.<commits>+g<commit>" on the way to it, ".dirty" with uncommitted changes.
inline constexpr const char* kVersion = DES_VERSION;
// The fork's release tag ("des-0.14.0-1"), or its commit, as git describe gives it; "unknown" if not known.
inline constexpr const char* kPs3recompVersion = DES_PS3RECOMP_VERSION;
// The SHA-256 of the decrypted EBOOT.ELF the lift was made from; empty if not known.
inline constexpr const char* kDumpSha256 = DES_DUMP_SHA256;
}  // namespace ui
