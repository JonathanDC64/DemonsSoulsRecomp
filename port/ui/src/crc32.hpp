// Written for Demon's Souls. CRC-32 as zip files use it (reflected, polynomial 0xEDB88320): for the zips the host
// writes (report.cpp) and the one it reads (setup_unzip.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ui {

inline uint32_t crc32(const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace ui
