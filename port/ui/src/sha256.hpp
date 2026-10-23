// Written for Demon's Souls. SHA-256 (FIPS 180-4) in portable code, for the host's own check of the first Python it
// downloads (setup_python.hpp, docs/88 section 5): a file read in pieces, its sum as 64 lowercase hex digits, as
// Python's hashlib writes it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ui {

class Sha256 {
public:
    Sha256();
    void add(const void* data, size_t size);
    // The sum of everything added, in lowercase hex. It finishes the sum: add nothing after it.
    std::string hex();

private:
    void block(const unsigned char* p);

    uint32_t mState[8];
    unsigned char mBuffer[64];
    size_t mFill = 0;     // bytes in mBuffer
    uint64_t mBytes = 0;  // bytes added in all
};

// The file's SHA-256 in lowercase hex, or empty if it could not be read to its end.
std::string sha256_file(const std::filesystem::path& path);

// The known answers (FIPS 180-2 appendix B): "", "abc", the 56-byte two-block message, and a million 'a's added 1,000
// at a time (so the buffered and the whole-block paths both run, at eight offsets). True if this build gets all four.
// port/ui has no C++ test target, so the first Python's check runs this before it trusts a sum: a few milliseconds.
bool sha256_known_answers();

}  // namespace ui
