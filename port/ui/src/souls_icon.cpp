// Written for Demon's Souls. The program's icon (setup's `--icon <game folder> <out.ico>`, tools/setup/des_setup/
// exe_icon.py, docs/112 section 7): one of the game's swords, drawn from the player's own copy of the game. A picture
// made from the game cannot be in the download (docs/51), so it is made here, once the game's files are there.
//
// It is drawn on the CPU: setup needs no graphics card for it, and it comes out the same on every PC. The light, the
// view and the glow are the cursor's (ui::models::look, the model shader in render_d3d12.cpp). An icon is small and a
// sword is long and thin, so the sword fills the square corner to corner, and in the small sizes it is drawn wider
// than it is, or it would be a line.
#include "souls_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void ps3_setenv(const char* name, const char* value);  // boot_main.cpp

namespace {

using ui::models::Look;
using ui::models::Mesh;
using ui::models::Model;
using ui::models::TexFormat;
using ui::models::Texture;

const char kSword[] = "soulbrandt";
constexpr float kMargin = 0.06f;  // of the square, each side: room for the glow

// The sizes the icon holds, and how much wider the sword is drawn in each. (Windows picks the nearest for the view.)
struct Size {
    int pixels;
    float widen;
};
constexpr Size kSizes[] = {{256, 1.f}, {128, 1.f}, {96, 1.f},   {64, 1.f},   {48, 1.3f},
                           {40, 1.5f}, {32, 1.7f}, {24, 2.1f},  {20, 2.35f}, {16, 2.6f}};

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

void colour565(unsigned c, int out[3]) {
    out[0] = ((c >> 11) & 31) * 255 / 31;
    out[1] = ((c >> 5) & 63) * 255 / 63;
    out[2] = (c & 31) * 255 / 31;
}

// A texture's largest level. PS3 blocks are the same bytes as a PC's: BC1, BC2 or BC3.
Image decode(const Texture& t) {
    Image im;
    const size_t block = t.format == TexFormat::BC1 ? 8 : 16;
    const int across = (t.width + 3) / 4, down = (t.height + 3) / 4;
    if (t.width <= 0 || t.height <= 0 || t.data.size() < block * size_t(across) * size_t(down)) return im;
    im.w = t.width;
    im.h = t.height;
    im.rgba.resize(size_t(t.width) * size_t(t.height) * 4);
    for (int by = 0; by < down; by++) {
        for (int bx = 0; bx < across; bx++) {
            const uint8_t* b = t.data.data() + block * (size_t(by) * size_t(across) + size_t(bx));
            int alpha[16];
            std::fill(alpha, alpha + 16, 255);
            if (t.format == TexFormat::BC2) {
                for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> (i & 1 ? 4 : 0)) & 15) * 17;
            } else if (t.format == TexFormat::BC3) {
                const int a0 = b[0], a1 = b[1];
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) {
                    const int k = int((bits >> (3 * i)) & 7);
                    if (k < 2) alpha[i] = k ? a1 : a0;
                    else if (a0 > a1) alpha[i] = ((8 - k) * a0 + (k - 1) * a1) / 7;
                    else alpha[i] = k == 6 ? 0 : k == 7 ? 255 : ((6 - k) * a0 + (k - 1) * a1) / 5;
                }
            }
            const uint8_t* c = block == 16 ? b + 8 : b;
            const unsigned c0 = c[0] | unsigned(c[1]) << 8, c1 = c[2] | unsigned(c[3]) << 8;
            int pal[4][4];
            colour565(c0, pal[0]);
            colour565(c1, pal[1]);
            const bool four = t.format != TexFormat::BC1 || c0 > c1;
            for (int k = 0; k < 3; k++) {
                pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
                pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
            }
            const uint32_t index = c[4] | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
            for (int i = 0; i < 16; i++) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= t.width || y >= t.height) continue;
                const int k = (index >> (2 * i)) & 3;
                uint8_t* out = &im.rgba[(size_t(y) * size_t(t.width) + size_t(x)) * 4];
                for (int n = 0; n < 3; n++) out[n] = uint8_t(pal[k][n]);
                out[3] = uint8_t(!four && k == 3 ? 0 : alpha[i]);
            }
        }
    }
    return im;
}

// Bilinear, wrapping, as the model shader samples; out's values from 0 to 1. No texture: the fallback colour.
void sample(const Image* t, float u, float v, float fallback, float out[4]) {
    if (!t || !t->w) {
        out[0] = out[1] = out[2] = fallback;
        out[3] = 1.f;
        return;
    }
    const float x = u * float(t->w) - 0.5f, y = v * float(t->h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y), ax = x - fx, ay = y - fy;
    const auto at = [t](int px, int py, int n) {
        px %= t->w;
        py %= t->h;
        if (px < 0) px += t->w;
        if (py < 0) py += t->h;
        return float(t->rgba[(size_t(py) * size_t(t->w) + size_t(px)) * 4 + size_t(n)]) / 255.f;
    };
    const int ix = int(fx), iy = int(fy);
    for (int n = 0; n < 4; n++) {
        const float top = at(ix, iy, n) * (1.f - ax) + at(ix + 1, iy, n) * ax;
        const float low = at(ix, iy + 1, n) * (1.f - ax) + at(ix + 1, iy + 1, n) * ax;
        out[n] = top * (1.f - ay) + low * ay;
    }
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void unit(float v[3]) {
    const float n = std::sqrt(dot3(v, v));
    if (n > 1e-6f)
        for (int k = 0; k < 3; k++) v[k] /= n;
}

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// The sword on a clear square: premultiplied RGBA, size by size, its glow around it.
std::vector<float> draw(const Model& model, const Look& look, const std::vector<Image>& textures, const Size& size) {
    const int ss = size.pixels >= 128 ? 3 : 8, S = size.pixels * ss;
    const std::vector<Mesh>& meshes = ui::models::meshes(model);
    const float* R = look.rotation;
    const auto turned = [&](const float p[3], float out[3]) {
        const float x = p[0] * size.widen, y = p[1], z = p[2] * size.widen;
        for (int r = 0; r < 3; r++) out[r] = R[r * 3] * x + R[r * 3 + 1] * y + R[r * 3 + 2] * z;
    };
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (const Mesh& mesh : meshes) {
        for (const auto& v : mesh.vertices) {
            float q[3];
            turned(v.pos, q);
            for (int k = 0; k < 2; k++) {
                lo[k] = std::min(lo[k], q[k]);
                hi[k] = std::max(hi[k], q[k]);
            }
        }
    }
    const float span = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (!(span > 0.f)) return {};
    const float scale = float(S) * (1.f - 2.f * kMargin) / span;
    const float mid[2] = {float(S) * 0.5f - 0.5f * (lo[0] + hi[0]) * scale, float(S) * 0.5f - 0.5f * (lo[1] + hi[1]) * scale};

    std::vector<float> colour(size_t(S) * size_t(S) * 4, 0.f), depth(size_t(S) * size_t(S), -1e30f);
    float L[3] = {look.light[0], look.light[1], look.light[2]};
    unit(L);
    float H[3] = {L[0], L[1], L[2] + 1.f};
    unit(H);
    struct Point {
        float x, y, z, n[3], u, v;
    };
    std::vector<Point> points;
    for (const bool blended : {false, true}) {
        for (const Mesh& mesh : meshes) {
            if (mesh.blend != blended) continue;
            const Image* diffuse = mesh.texture >= 0 && size_t(mesh.texture) < textures.size() ? &textures[size_t(mesh.texture)] : nullptr;
            const Image* specular = mesh.specular >= 0 && size_t(mesh.specular) < textures.size() ? &textures[size_t(mesh.specular)] : nullptr;
            const float* tint = blended ? look.tint : look.solid;
            points.resize(mesh.vertices.size());
            for (size_t i = 0; i < mesh.vertices.size(); i++) {
                const auto& v = mesh.vertices[i];
                float q[3];
                turned(v.pos, q);
                points[i] = {q[0] * scale + mid[0], q[1] * scale + mid[1], q[2] * scale, {0.f, 0.f, 0.f}, v.uv[0], v.uv[1]};
                for (int r = 0; r < 3; r++) points[i].n[r] = R[r * 3] * v.normal[0] + R[r * 3 + 1] * v.normal[1] + R[r * 3 + 2] * v.normal[2];
            }
            for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                if (mesh.indices[t] >= points.size() || mesh.indices[t + 1] >= points.size() || mesh.indices[t + 2] >= points.size()) continue;
                const Point& a = points[mesh.indices[t]];
                const Point& b = points[mesh.indices[t + 1]];
                const Point& c = points[mesh.indices[t + 2]];
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (std::fabs(area) < 1e-6f) continue;
                const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(S - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
                const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(S - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
                for (int y = y0; y <= y1; y++) {
                    for (int x = x0; x <= x1; x++) {
                        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                        const float wa = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                        const float wb = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                        const float wc = 1.f - wa - wb;
                        if (wa < 0.f || wb < 0.f || wc < 0.f) continue;
                        const float z = wa * a.z + wb * b.z + wc * c.z;
                        const size_t at = size_t(y) * size_t(S) + size_t(x);
                        if (z <= depth[at]) continue;
                        float n[3];
                        for (int k = 0; k < 3; k++) n[k] = wa * a.n[k] + wb * b.n[k] + wc * c.n[k];
                        unit(n);
                        if (blended && n[2] <= 0.f) continue;
                        const float u = wa * a.u + wb * b.u + wc * c.u, v = wa * a.v + wb * b.v + wc * c.v;
                        float d[4], s[4];
                        sample(diffuse, u, v, 1.f, d);
                        if (!blended && d[3] < look.alpha_test) continue;
                        sample(specular, u, v, 0.f, s);
                        if (n[2] < 0.f)
                            for (float& e : n) e = -e;
                        const float lit = look.ambient + (1.f - look.ambient) * clamp01(dot3(n, L));
                        const float shine = std::pow(clamp01(dot3(n, H)), 24.f) * 0.9f;
                        float* out = &colour[at * 4];
                        if (!blended) {
                            for (int k = 0; k < 3; k++) out[k] = clamp01((d[k] * lit + s[k] * shine) * tint[k]);
                            out[3] = 1.f;
                            depth[at] = z;
                        } else {
                            const float over = d[3];
                            for (int k = 0; k < 3; k++) out[k] = clamp01((d[k] * lit + s[k] * shine) * tint[k]) * over + out[k] * (1.f - over);
                            out[3] = over + out[3] * (1.f - over);
                        }
                    }
                }
            }
        }
    }

    // Down to the icon's size.
    const int N = size.pixels;
    std::vector<float> image(size_t(N) * size_t(N) * 4, 0.f);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            for (int k = 0; k < 4; k++)
                image[(size_t(y / ss) * size_t(N) + size_t(x / ss)) * 4 + size_t(k)] += colour[(size_t(y) * size_t(S) + size_t(x)) * 4 + size_t(k)] / float(ss * ss);

    // The glow: the sword's outline blurred, in the glow's colour, under the sword.
    if (look.glow_strength > 0.f) {
        const float sigma = std::max(0.7f, float(N) * (N >= 64 ? 0.03f : 0.05f));
        const int reach = int(std::ceil(sigma * 3.f));
        std::vector<float> kernel(size_t(reach) * 2 + 1);
        float sum = 0.f;
        for (int i = -reach; i <= reach; i++) sum += kernel[size_t(i + reach)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
        for (float& k : kernel) k /= sum;
        std::vector<float> across(size_t(N) * size_t(N), 0.f), blur(size_t(N) * size_t(N), 0.f);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int i = -reach; i <= reach; i++)
                    if (x + i >= 0 && x + i < N) across[size_t(y) * size_t(N) + size_t(x)] += image[(size_t(y) * size_t(N) + size_t(x + i)) * 4 + 3] * kernel[size_t(i + reach)];
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int i = -reach; i <= reach; i++)
                    if (y + i >= 0 && y + i < N) blur[size_t(y) * size_t(N) + size_t(x)] += across[size_t(y + i) * size_t(N) + size_t(x)] * kernel[size_t(i + reach)];
        for (size_t i = 0; i < blur.size(); i++) {
            const float halo = std::min(1.f, blur[i] * 1.8f) * look.glow_strength * (1.f - image[i * 4 + 3]);
            for (size_t k = 0; k < 3; k++) image[i * 4 + k] += look.glow[k] * halo;
            image[i * 4 + 3] += halo;
        }
    }
    return image;
}

void put16(std::vector<uint8_t>& out, unsigned v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

// One frame of an .ico: a 32-bit bitmap, its rows bottom up, straight alpha, then the (unused) 1-bit mask.
std::vector<uint8_t> frame(const std::vector<float>& image, int n) {
    std::vector<uint8_t> out;
    put32(out, 40);
    put32(out, uint32_t(n));
    put32(out, uint32_t(n) * 2);  // (the bitmap and the mask, one above the other)
    put16(out, 1);
    put16(out, 32);
    for (int i = 0; i < 6; i++) put32(out, 0);
    for (int y = n - 1; y >= 0; y--) {
        for (int x = 0; x < n; x++) {
            const float* p = &image[(size_t(y) * size_t(n) + size_t(x)) * 4];
            const float a = clamp01(p[3]), keep = a > 1e-4f ? 1.f / a : 0.f;
            for (int k : {2, 1, 0}) out.push_back(uint8_t(clamp01(p[k] * keep) * 255.f + 0.5f));
            out.push_back(uint8_t(a * 255.f + 0.5f));
        }
    }
    out.resize(out.size() + size_t((n + 31) / 32 * 4) * size_t(n), 0);
    return out;
}

}  // namespace

// `game`: the game's folder; `out`: the .ico to write. 0 when it is written.
extern "C" int des_icon_render(const char* game, const char* out) {
    ps3_setenv("PS3_VFS_ROOT", game);
    const Model* model = ui::models::get(kSword);
    if (!model) {
        std::fprintf(stderr, "[icon] the sword's model could not be read\n");
        return 1;
    }
    std::vector<Image> textures;
    for (const Texture& t : ui::models::textures(*model)) textures.push_back(decode(t));
    const Look look = ui::models::look(kSword, 0.f);
    std::vector<std::vector<uint8_t>> frames;
    for (const Size& size : kSizes) {
        const std::vector<float> image = draw(*model, look, textures, size);
        if (image.empty()) {
            std::fprintf(stderr, "[icon] the sword's model has no vertices\n");
            return 1;
        }
        frames.push_back(frame(image, size.pixels));
    }
    std::vector<uint8_t> file;
    put16(file, 0);
    put16(file, 1);  // an icon
    put16(file, unsigned(frames.size()));
    uint32_t at = uint32_t(6 + 16 * frames.size());
    for (size_t i = 0; i < frames.size(); i++) {
        const int n = kSizes[i].pixels;
        file.push_back(uint8_t(n == 256 ? 0 : n));  // (0 means 256)
        file.push_back(uint8_t(n == 256 ? 0 : n));
        file.push_back(0);
        file.push_back(0);
        put16(file, 1);
        put16(file, 32);
        put32(file, uint32_t(frames[i].size()));
        put32(file, at);
        at += uint32_t(frames[i].size());
    }
    for (const auto& f : frames) file.insert(file.end(), f.begin(), f.end());
    const std::string name = out;
    std::ofstream to(std::filesystem::path(std::u8string(name.begin(), name.end())), std::ios::binary);
    to.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    to.close();
    if (!to) {
        std::fprintf(stderr, "[icon] %s could not be written\n", out);
        return 1;
    }
    std::fprintf(stderr, "[icon] %s: %zu sizes\n", out, frames.size());
    return 0;
}
