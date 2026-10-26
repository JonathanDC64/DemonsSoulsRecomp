// SHA-256 (see sha256.hpp): the 64 rounds over each 64-byte block, the message padded with 0x80, zeros and its length
// in bits, big-endian. The same code everywhere.
#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace ui {
namespace {

// The first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));  // (n is 2 to 25: never a shift by 32)
}

std::string sum_of(const std::string& text, size_t piece) {
    Sha256 h;
    for (size_t from = 0; from < text.size(); from += piece)
        h.add(text.data() + from, std::min(piece, text.size() - from));
    return h.hex();
}

}  // namespace

Sha256::Sha256()
    : mState{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3];
    uint32_t e = mState[4], f = mState[5], g = mState[6], h = mState[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    mState[0] += a;
    mState[1] += b;
    mState[2] += c;
    mState[3] += d;
    mState[4] += e;
    mState[5] += f;
    mState[6] += g;
    mState[7] += h;
}

void Sha256::add(const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    mBytes += size;
    while (size) {
        if (mFill == 0 && size >= 64) {  // (a whole block: straight from the caller's bytes)
            block(p);
            p += 64;
            size -= 64;
            continue;
        }
        const size_t n = std::min(size, sizeof mBuffer - mFill);
        std::memcpy(mBuffer + mFill, p, n);
        mFill += n;
        p += n;
        size -= n;
        if (mFill == sizeof mBuffer) {
            block(mBuffer);
            mFill = 0;
        }
    }
}

std::string Sha256::hex() {
    const uint64_t bits = mBytes * 8;  // (before the padding adds to it)
    const unsigned char one = 0x80, zero = 0;
    add(&one, 1);
    while (mFill != 56) add(&zero, 1);
    unsigned char length[8];
    for (int i = 0; i < 8; ++i) length[i] = (unsigned char)(bits >> (56 - 8 * i));
    add(length, sizeof length);  // (fills the last block, which runs)
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const uint32_t v : mState)
        for (int shift = 28; shift >= 0; shift -= 4) out += kDigits[(v >> shift) & 15];
    return out;
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    Sha256 h;
    std::vector<char> piece(1 << 20);
    while (f) {
        f.read(piece.data(), std::streamsize(piece.size()));
        h.add(piece.data(), size_t(f.gcount()));
    }
    return f.eof() && !f.bad() ? h.hex() : std::string();
}

bool sha256_known_answers() {
    return sum_of("", 1) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
           sum_of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
           sum_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" &&
           sum_of(std::string(1000000, 'a'), 1000) ==
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
}

}  // namespace ui
