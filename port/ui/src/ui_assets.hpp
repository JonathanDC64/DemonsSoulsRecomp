// Written for Demon's Souls (docs/32).
// The UI's embedded files (port/ui/res, compiled in by cmake/EmbedFiles.cmake). From FFCCEOT-Recomp.
#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace ui {

struct Asset {
    const char* path;  // relative to port/ui, forward slashes ("res/rml/theme.rcss")
    const unsigned char* data;
    size_t size;
};

extern const Asset kAssets[];
extern const size_t kAssetCount;

inline const Asset* find_asset(std::string_view path) {
    for (size_t i = 0; i < kAssetCount; i++) {
        if (path == kAssets[i].path) return &kAssets[i];
    }
    return nullptr;
}

}  // namespace ui
