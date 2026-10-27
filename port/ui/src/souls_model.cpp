// Written for Demon's Souls. The game's own models for the UI (souls_model.hpp, docs/112).
#include "souls_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace ui::models {

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    // An animated model: its skeleton's parents and reference pose, and the animation decoded to whole frames.
    struct Qs {
        float t[3] = {0, 0, 0};
        float q[4] = {0, 0, 0, 1};  // x, y, z, w
        float s[3] = {1, 1, 1};
    };
    std::vector<int> parents;
    std::vector<Qs> reference;
    std::vector<std::vector<Qs>> frames;  // [frame][bone], every bone (the tracks bound, the others as the reference)
    double length = 0;
};

namespace {

using Qs = Model::Qs;

// ---- Bytes ----

// A file's bytes, read big-endian. A read past the end gives zeros and marks the view bad.
struct View {
    const uint8_t* p = nullptr;
    size_t n = 0;
    mutable bool bad = false;

    bool in(size_t off, size_t len) const {
        if (off > n || len > n - off) {
            bad = true;
            return false;
        }
        return true;
    }
    uint8_t u8(size_t o) const { return in(o, 1) ? p[o] : 0; }
    uint16_t u16(size_t o) const { return in(o, 2) ? uint16_t(p[o] << 8 | p[o + 1]) : 0; }
    int16_t i16(size_t o) const { return int16_t(u16(o)); }
    uint32_t u32(size_t o) const {
        return in(o, 4) ? uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 | uint32_t(p[o + 3]) : 0;
    }
    int32_t i32(size_t o) const { return int32_t(u32(o)); }
    float f32(size_t o) const {
        const uint32_t u = u32(o);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    std::string cstr(size_t o) const {
        std::string s;
        while (o < n && p[o] && s.size() < 1024) s += char(p[o++]);
        return s;
    }
    // A UTF-16 (big-endian) name. They are ASCII; anything else becomes '?'.
    std::string wstr(size_t o) const {
        std::string s;
        while (in(o, 2) && s.size() < 1024) {
            const uint16_t c = u16(o);
            if (!c) break;
            s += c < 0x80 ? char(c) : '?';
            o += 2;
        }
        return s;
    }
    View sub(size_t off, size_t len) const { return in(off, len) ? View{p + off, len} : View{}; }
};

bool read_file(const std::string& utf8, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::path(std::u8string(utf8.begin(), utf8.end())), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > (std::streamoff(64) << 20)) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The last part of a path in an archive ("N:\FRPG\...\WP_A_0258.flver" -> "wp_a_0258.flver"), lower case.
std::string leaf(const std::string& path) {
    const size_t k = path.find_last_of("\\/");
    return lower(k == std::string::npos ? path : path.substr(k + 1));
}

// ---- BND3 ----

struct Entry {
    std::string name;  // leaf(), lower case
    View data;
};

std::vector<Entry> bnd3(const View& v) {
    std::vector<Entry> out;
    if (v.n < 0x20 || std::memcmp(v.p, "BND3", 4) != 0) return out;
    uint8_t fmt = v.u8(0xC);
    const bool big = v.u8(0xD) != 0;
    if (!v.u8(0xE)) {  // the format's bits, written the other way round
        uint8_t r = 0;
        for (int i = 0; i < 8; i++)
            if (fmt & (1 << i)) r = uint8_t(r | (0x80 >> i));
        fmt = r;
    }
    if (!big && !(fmt & 1)) return out;  // little-endian: not the PS3's
    const int count = v.i32(0x10);
    const bool has_id = fmt & 2, has_name = fmt & 0xC, has_size = fmt & 0x20, long_offset = fmt & 0x10;
    size_t off = 0x20;
    for (int i = 0; i < count && i < 100000 && !v.bad; i++) {
        off += 4;
        const uint32_t size = v.u32(off);
        off += 4;
        uint64_t data = v.u32(off);
        if (long_offset) data = data << 32 | v.u32(off + 4);
        off += long_offset ? 8 : 4;
        if (has_id) off += 4;
        std::string name;
        if (has_name) {
            name = leaf(v.cstr(v.u32(off)));
            off += 4;
        }
        if (has_size) off += 4;
        out.push_back({name, v.sub(size_t(data), size)});
    }
    return out;
}

const Entry* find(const std::vector<Entry>& entries, const std::string& name) {
    for (const Entry& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

// ---- TPF (PS3) ----

std::vector<Texture> tpf(const View& v) {
    std::vector<Texture> out;
    if (v.n < 0x10 || std::memcmp(v.p, "TPF", 3) != 0) return out;
    const int count = v.i32(8);
    for (int i = 0; i < count && i < 4096; i++) {
        const size_t e = 0x10 + size_t(i) * 0x20;
        const uint32_t off = v.u32(e), size = v.u32(e + 4);
        const uint8_t fmt = v.u8(e + 8), cube = v.u8(e + 9), mips = v.u8(e + 10);
        const int w = v.u16(e + 12), h = v.u16(e + 14);
        Texture t;
        t.name = lower(v.cstr(v.u32(e + 24)));
        if (fmt == 0 || fmt == 1) t.format = TexFormat::BC1;
        else if (fmt == 3) t.format = TexFormat::BC2;
        else if (fmt == 5) t.format = TexFormat::BC3;
        else continue;  // (not a block format; the models here use none)
        if (cube || w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;
        const View data = v.sub(off, size);
        if (!data.n) continue;
        t.width = w;
        t.height = h;
        // The levels the data holds, each a whole number of 4x4 blocks.
        const size_t block = t.format == TexFormat::BC1 ? 8 : 16;
        size_t used = 0;
        int levels = 0;
        for (int lw = w, lh = h; levels < std::max<int>(mips, 1); levels++) {
            const size_t bytes = size_t(std::max(1, (lw + 3) / 4)) * size_t(std::max(1, (lh + 3) / 4)) * block;
            if (used + bytes > data.n) break;
            used += bytes;
            if (lw == 1 && lh == 1) {
                levels++;
                break;
            }
            lw = std::max(1, lw / 2);
            lh = std::max(1, lh / 2);
        }
        if (!levels) continue;
        t.levels = levels;
        t.data.assign(data.p, data.p + used);
        out.push_back(std::move(t));
    }
    return out;
}

// ---- FLVER0 ----

struct Member {
    uint32_t offset, type, semantic;
};

int member_size(uint32_t type) {
    switch (type) {
    case 0x01: return 8;
    case 0x02: return 12;
    case 0x03: return 16;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: case 0x2F: return 4;
    case 0x16: case 0x18: case 0x1A: case 0x2E: return 8;
    default: return 0;
    }
}

// Triangles from a strip with 0xFFFF restarts, every other one turned the other way, the degenerate ones dropped.
void strip_to_triangles(const std::vector<uint16_t>& strip, std::vector<uint32_t>& out) {
    bool flip = false;
    for (size_t i = 0; i + 2 < strip.size(); i++) {
        const uint16_t a = strip[i], b = strip[i + 1], c = strip[i + 2];
        if (a == 0xFFFF || b == 0xFFFF || c == 0xFFFF) {
            flip = false;
            continue;
        }
        if (a != b && b != c && a != c) {
            out.push_back(a);
            out.push_back(flip ? c : b);
            out.push_back(flip ? b : c);
        }
        flip = !flip;
    }
}

struct Flver {
    std::vector<Mesh> meshes;
    std::vector<std::string> bone_names;
    std::vector<std::string> mesh_textures, mesh_speculars;  // each mesh's diffuse and specular texture names, lower case
};

bool flver0(const View& d, Flver& out) {
    if (d.n < 0x80 || std::memcmp(d.p, "FLVER\0B\0", 8) != 0) return false;
    const uint32_t data = d.u32(0x0C);
    const int ndummies = d.i32(0x14), nmaterials = d.i32(0x18), nbones = d.i32(0x1C), nmeshes = d.i32(0x20);
    if (ndummies < 0 || nmaterials < 0 || nbones < 0 || nmeshes < 0 || nmaterials > 1024 || nbones > 1024 ||
        nmeshes > 1024)
        return false;
    size_t o = 0x80 + size_t(ndummies) * 0x40;

    struct Material {
        bool blend = false;
        std::string diffuse, specular;
        std::vector<std::pair<uint32_t, std::vector<Member>>> layouts;  // stride, members
    };
    std::vector<Material> materials(static_cast<size_t>(nmaterials));
    for (int m = 0; m < nmaterials; m++) {
        const size_t e = o + size_t(m) * 0x20;
        Material& mat = materials[size_t(m)];
        const std::string mtd = d.wstr(d.u32(e + 4));
        mat.blend = mtd.find("_Alp") != std::string::npos;
        const uint32_t texs = d.u32(e + 8), layouts = d.u32(e + 20);
        const int ntex = d.u8(texs);
        for (int t = 0; t < ntex && t < 16; t++) {
            const uint32_t path = d.u32(texs + 16 + size_t(t) * 8), type = d.u32(texs + 20 + size_t(t) * 8);
            if (!path) continue;
            std::string name = leaf(d.wstr(path));
            if (const size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
            // Its kind: g_Diffuse, g_Specular, g_Bumpmap, ...; without one, by the name's ending (_s, _n).
            const std::string kind = type ? d.wstr(type) : std::string();
            const auto ends = [&](const char* tail) {
                return name.size() > 2 && name.compare(name.size() - 2, 2, tail) == 0;
            };
            if (kind.find("Specular") != std::string::npos || (kind.empty() && ends("_s"))) mat.specular = name;
            else if (kind.find("Diffuse") != std::string::npos || (kind.empty() && !ends("_n") && mat.diffuse.empty()))
                mat.diffuse = name;
        }
        const int nlayouts = d.i32(layouts);
        size_t lp = d.u32(layouts + 16);
        for (int l = 0; l < nlayouts && l < 16; l++) {
            const int nmembers = d.i16(lp), stride = d.i16(lp + 2);
            std::vector<Member> members;
            for (int k = 0; k < nmembers && k < 32; k++) {
                const size_t me = lp + 16 + size_t(k) * 20;
                members.push_back({d.u32(me + 4), d.u32(me + 8), d.u32(me + 12)});
            }
            mat.layouts.push_back({uint32_t(stride), std::move(members)});
            lp += 16 + size_t(std::max(nmembers, 0)) * 20;
        }
    }
    o += size_t(nmaterials) * 0x20;
    for (int b = 0; b < nbones; b++) out.bone_names.push_back(d.wstr(d.u32(o + size_t(b) * 0x80 + 12)));
    o += size_t(nbones) * 0x80;

    for (int i = 0; i < nmeshes && !d.bad; i++) {
        const size_t e = o + size_t(i) * 0x64;
        const int mat = d.u8(e + 1);
        const int nindices = d.i32(e + 4), nvertices = d.i32(e + 8);
        int16_t local_bones[28];
        for (int k = 0; k < 28; k++) local_bones[k] = d.i16(e + 0x0E + size_t(k) * 2);
        const uint32_t indices = d.u32(e + 0x4C), vlength = d.u32(e + 0x50), vertices = d.u32(e + 0x54);
        if (mat >= nmaterials || nvertices <= 0 || nindices <= 0 || nvertices > 65536) continue;
        const Material& material = materials[size_t(mat)];
        const uint32_t stride = vlength / uint32_t(nvertices);
        const std::vector<Member>* members = nullptr;
        for (const auto& [st, ms] : material.layouts)
            if (st == stride) members = &ms;
        if (!members && !material.layouts.empty()) members = &material.layouts.front().second;
        if (!members) continue;

        Mesh mesh;
        mesh.blend = material.blend;
        std::vector<uint16_t> strip(static_cast<size_t>(nindices));
        for (int k = 0; k < nindices; k++) strip[size_t(k)] = d.u16(data + indices + size_t(k) * 2);
        strip_to_triangles(strip, mesh.indices);
        for (size_t k = 0; k + 2 < mesh.indices.size();) {  // (a triangle past the vertices: dropped)
            if (mesh.indices[k] < uint32_t(nvertices) && mesh.indices[k + 1] < uint32_t(nvertices) &&
                mesh.indices[k + 2] < uint32_t(nvertices)) {
                k += 3;
                continue;
            }
            mesh.indices.erase(mesh.indices.begin() + std::ptrdiff_t(k), mesh.indices.begin() + std::ptrdiff_t(k + 3));
        }
        mesh.vertices.resize(size_t(nvertices));
        bool skinned = false;
        for (const Member& m : *members) skinned = skinned || m.semantic == 1;
        if (skinned) {
            mesh.bones.resize(size_t(nvertices));
            mesh.weights.resize(size_t(nvertices));
        }
        for (int k = 0; k < nvertices; k++) {
            const size_t base = data + vertices + size_t(k) * stride;
            Vertex& v = mesh.vertices[size_t(k)];
            for (const Member& m : *members) {
                if (m.offset + uint32_t(member_size(m.type)) > stride) continue;
                const size_t at = base + m.offset;
                switch (m.semantic) {
                case 0:  // position
                    if (m.type == 0x02 || m.type == 0x03)
                        for (int c = 0; c < 3; c++) v.pos[c] = d.f32(at + size_t(c) * 4);
                    break;
                case 1:  // bone weights
                    if (skinned && m.type == 0x1A)
                        for (int c = 0; c < 4; c++)
                            mesh.weights[size_t(k)][size_t(c)] = float(d.i16(at + size_t(c) * 2)) / 32767.f;
                    break;
                case 2:  // bone indices, into the mesh's 28 bones
                    if (skinned && member_size(m.type) == 4)
                        for (int c = 0; c < 4; c++) {
                            const int li = d.u8(at + size_t(c));
                            const int gi = li < 28 ? local_bones[li] : -1;
                            mesh.bones[size_t(k)][size_t(c)] = uint16_t(gi >= 0 && gi < nbones ? gi : 0);
                        }
                    break;
                case 3:  // normal, bytes around 127
                    if (member_size(m.type) == 4)
                        for (int c = 0; c < 3; c++) v.normal[c] = (float(d.u8(at + size_t(c))) - 127.f) / 127.f;
                    break;
                case 5:  // texture coordinates: the first pair
                    if (m.type == 0x15 || m.type == 0x12 || m.type == 0x16)
                        for (int c = 0; c < 2; c++) v.uv[c] = float(d.i16(at + size_t(c) * 2)) / 1024.f;
                    break;
                default:
                    break;
                }
            }
        }
        out.meshes.push_back(std::move(mesh));
        out.mesh_textures.push_back(material.diffuse);
        out.mesh_speculars.push_back(material.specular);
    }
    return !d.bad && !out.meshes.empty();
}

// ---- Havok 5.5 packfiles ----

struct Pack {
    View data;                              // the __data__ section
    std::map<uint32_t, uint32_t> pointers;  // a pointer's place -> where it points (both in data)
    std::vector<std::pair<uint32_t, std::string>> objects;  // offset in data, class name
    uint32_t ptr(uint32_t at) const {
        const auto it = pointers.find(at);
        return it == pointers.end() ? 0xFFFFFFFFu : it->second;
    }
    uint32_t first(const char* cls) const {
        for (const auto& [off, name] : objects)
            if (name == cls) return off;
        return 0xFFFFFFFFu;
    }
};

bool pack(const View& v, Pack& out) {
    if (v.n < 0x40 || v.u32(0) != 0x57E0E057u || v.u32(4) != 0x10C0C010u) return false;
    const int nsections = v.i32(20);
    struct Section {
        std::string name;
        uint32_t start, local, global, virt, exports;
    };
    std::vector<Section> sections;
    for (int i = 0; i < nsections && i < 16; i++) {
        const size_t o = 0x40 + size_t(i) * 0x30;
        sections.push_back({v.cstr(o), v.u32(o + 20), v.u32(o + 24), v.u32(o + 28), v.u32(o + 32), v.u32(o + 36)});
    }
    const Section* names = nullptr;
    const Section* data = nullptr;
    for (const Section& s : sections) {
        if (s.name == "__classnames__") names = &s;
        if (s.name == "__data__") data = &s;
    }
    if (!names || !data) return false;
    std::map<uint32_t, std::string> classes;
    for (size_t p = names->start; p + 5 < size_t(names->start) + names->local;) {
        if (v.u32(p) == 0xFFFFFFFFu || v.u8(p + 4) != 9) break;
        const std::string name = v.cstr(p + 5);
        classes[uint32_t(p + 5 - names->start)] = name;
        p += 5 + name.size() + 1;
    }
    out.data = v.sub(data->start, data->local);
    for (size_t q = size_t(data->start) + data->local; q + 8 <= size_t(data->start) + data->global; q += 8) {
        const uint32_t src = v.u32(q), dst = v.u32(q + 4);
        if (src != 0xFFFFFFFFu) out.pointers[src] = dst;
    }
    for (size_t q = size_t(data->start) + data->global; q + 12 <= size_t(data->start) + data->virt; q += 12) {
        const uint32_t src = v.u32(q), dst = v.u32(q + 8);
        if (src != 0xFFFFFFFFu) out.pointers[src] = dst;
    }
    for (size_t q = size_t(data->start) + data->virt; q + 12 <= size_t(data->start) + data->exports; q += 12) {
        const uint32_t src = v.u32(q), cls = v.u32(q + 8);
        if (src == 0xFFFFFFFFu) continue;
        const auto it = classes.find(cls);
        out.objects.push_back({src, it == classes.end() ? std::string() : it->second});
    }
    std::sort(out.objects.begin(), out.objects.end());
    return !v.bad;
}

Qs read_qs(const View& v, size_t o) {
    Qs x;
    for (int c = 0; c < 3; c++) x.t[c] = v.f32(o + size_t(c) * 4);
    for (int c = 0; c < 4; c++) x.q[c] = v.f32(o + 16 + size_t(c) * 4);
    for (int c = 0; c < 3; c++) x.s[c] = v.f32(o + 32 + size_t(c) * 4);
    return x;
}

struct Skeleton {
    std::vector<std::string> names;
    std::vector<int> parents;
    std::vector<Qs> reference;
};

// hkaSkeleton (5.5, no vtable): +0 name*, +4 parentIndices*, +8 n, +12 bones**, +16 n, +20 referencePose*, +24 n.
bool skeleton(const Pack& p, Skeleton& out) {
    const uint32_t s = p.first("hkaSkeleton");
    if (s == 0xFFFFFFFFu) return false;
    const View& v = p.data;
    const int nparents = v.i32(s + 8), nbones = v.i32(s + 16), nref = v.i32(s + 24);
    if (nbones <= 0 || nbones > 1024 || nparents != nbones || nref != nbones) return false;
    const uint32_t parents = p.ptr(s + 4), bones = p.ptr(s + 12), ref = p.ptr(s + 20);
    for (int i = 0; i < nbones; i++) {
        out.parents.push_back(v.i16(parents + size_t(i) * 2));
        const uint32_t bone = p.ptr(bones + uint32_t(i) * 4);
        out.names.push_back(bone == 0xFFFFFFFFu ? std::string() : v.cstr(p.ptr(bone)));
        out.reference.push_back(read_qs(v, ref + size_t(i) * 48));
    }
    for (int i = 0; i < nbones; i++)
        if (out.parents[size_t(i)] >= i) return false;  // (parents come first)
    return !v.bad;
}

// An animation as whole frames of its tracks, and the bone each track moves.
struct Clip {
    double length = 0;
    std::vector<std::vector<Qs>> frames;  // [frame][track]
    std::vector<int> track_bones;
};

// ---- Havok 5.5 wavelet animations ----
//
// hkaWaveletSkeletalAnimation, as the game samples it, written again from its own code (no Havok source): the sampler
// 0x010241F8 (the class's vtable slot 5), the transforms from the static mask 0x0101B980, the moving values' count
// 0x0101BD70, and a block's size 0x013530E0, unpacking 0x000106C0, dequantizing 0x01352E48 and inverse wavelet
// 0x000102E0. Most of the lizard's animations are these, its dance among them.
//
// The object's fields are big-endian; its data buffer, and the quantized stream in it, are little-endian. Each track
// has a 16-bit mask: bits 0-1, 2-3 and 4-5 are the translation's, rotation's and scale's kind (2: none), and bits
// 8-6 (x y z), 12-9 (x y z w) and 15-13 (x y z) say which values move; the others are constants. The moving values
// come in blocks of blockSize frames (8 in all of the lizard's). Per value and block: `preserved` raw floats, a mask
// of the coefficients that are the quantized zero, then the others at bw bits each; dequantized, and the inverse
// wavelet gives the block's frames.

// Reads in the data buffer: little-endian, zero past its end.
struct Le {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t u8(uint64_t o) const { return o < n ? p[o] : 0; }
    uint16_t u16(uint64_t o) const { return uint16_t(u8(o) | u8(o + 1) << 8); }
    uint32_t u32(uint64_t o) const { return uint32_t(u16(o)) | uint32_t(u16(o + 2)) << 16; }
    float f32(uint64_t o) const {
        const uint32_t u = u32(o);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
};

struct WaveletFields {
    uint32_t tracks, float_tracks, poses, block, numd;
    uint8_t max_bits, preserved;
    uint32_t offsets, scales, bits, static_mask, static_values, block_index, blocks, quantized, quantized_size;
};

// One value's block (0x000106C0, 0x01352E48): its block_size coefficients into c. The stream's position after it.
uint64_t wavelet_block(const Le& b, const WaveletFields& f, uint64_t pos, uint32_t d, int bw, float* c) {
    const uint32_t n = f.block, p = f.preserved;
    const float scale = b.f32(f.scales + 4ull * d), offset = b.f32(f.offsets + 4ull * d);
    const uint32_t one = 1u << bw, mask = one - 1;
    const float zero_at = float(one) * -offset / scale;  // the quantized zero, truncated as the game does
    int32_t zq = std::isfinite(zero_at) && std::fabs(zero_at) < 2147483520.0f ? int32_t(zero_at) : 0;
    if (uint32_t(zq) == one) zq -= 1;
    const uint32_t size = p * 4 + ((uint32_t(bw) * (n - p) + 7) >> 3);
    const uint32_t nq = ((size - 4 * p) * 8) / uint32_t(bw);
    for (uint32_t k = 0; k < p; k++) c[k] = b.f32(pos + 4ull * k);
    const uint64_t zeros = pos + 4ull * p;
    uint64_t values = zeros + ((nq + 7) >> 3);
    uint32_t acc = 0, have = 0, count = 0;
    const float step = scale * std::ldexp(1.0f, -bw);
    for (uint32_t k = 0; k < nq; k++) {
        uint32_t q;
        if ((b.u8(zeros + (k >> 3)) >> (k & 7)) & 1) {
            q = uint32_t(zq) & mask;
        } else {
            if (have < uint32_t(bw)) {  // the values: lowest bit first, 16 bits at a time
                acc |= uint32_t(b.u16(values)) << have;
                values += 2;
                have += 16;
            }
            q = acc & mask;
            acc >>= bw;
            have -= uint32_t(bw);
            count++;
        }
        if (p + k < n) c[p + k] = step * (float(q) + 0.5f) + offset;
    }
    return pos + ((uint64_t(nq) + 32ull * p + uint64_t(count) * uint32_t(bw) + 7) >> 3);
}

// 0x000102E0, in place: an 8-point synthesis, then a 5/3 lifting step per doubling. The 8-point table is LeGall 5/3's
// three lifting levels multiplied out (worked out from the steps, it matches the game's table exactly).
void wavelet_inverse(float* c, uint32_t n) {
    static const float kSynthesis[7][8] = {  // the rows of c[1] .. c[7]; c[0]'s is all ones
        {-0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.5f, 0.5f, 0.5f},
        {-0.5f, 0.0625f, 0.625f, 0.1875f, -0.25f, -0.25f, -0.25f, -0.25f},
        {0.0f, -0.0625f, -0.125f, -0.1875f, -0.25f, 0.25f, 0.75f, 0.75f},
        {-0.5f, 0.625f, -0.25f, -0.125f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, -0.125f, -0.25f, 0.75f, -0.25f, -0.125f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, -0.125f, -0.25f, 0.75f, -0.25f, -0.25f},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.125f, -0.25f, 0.75f},
    };
    float o[8];
    for (int k = 0; k < 8; k++) {
        float acc = kSynthesis[0][k] * c[1] + c[0];
        for (int j = 2; j < 8; j++) acc = kSynthesis[j - 1][k] * c[j] + acc;
        o[k] = acc;
    }
    std::memcpy(c, o, sizeof o);
    std::vector<float> tmp;
    for (uint32_t l = 8; l < n; l *= 2) {
        float* a = c;
        float* d = c + l;
        for (uint32_t i = 0; i + 1 < l; i++) a[i + 1] = (d[i] + d[i + 1]) * -0.25f + a[i + 1];
        a[0] = d[0] * -0.5f + a[0];
        for (uint32_t i = 0; i + 1 < l; i++) d[i] = (a[i] + a[i + 1]) * 0.5f + d[i];
        d[l - 1] = d[l - 1] + a[l - 1];
        tmp.assign(c, c + 2 * l);
        for (uint32_t i = 0; i < l; i++) {
            c[2 * i] = tmp[i];
            c[2 * i + 1] = tmp[l + i];
        }
    }
}

// hkaWaveletSkeletalAnimation (5.5): +12 duration, +16 transform tracks, +20 float tracks, +36 poses, +40 block size,
// +44 max bit width, +45 preserved, +48 moving values, +52 .. +84 its tables' places in the data buffer, +88 the data
// buffer*, +92 its size. Every pose as a frame (the game's sampler gives exactly those at t = pose / (poses - 1)).
bool wavelet_clip(const Pack& p, uint32_t a, Clip& out) {
    const View& v = p.data;
    WaveletFields f{};
    out.length = v.f32(a + 12);
    f.tracks = v.u32(a + 16);
    f.float_tracks = v.u32(a + 20);
    f.poses = v.u32(a + 36);
    f.block = v.u32(a + 40);
    f.max_bits = v.u8(a + 44);
    f.preserved = v.u8(a + 45);
    f.numd = v.u32(a + 48);
    f.offsets = v.u32(a + 52);
    f.scales = v.u32(a + 56);
    f.bits = v.u32(a + 60);
    f.static_mask = v.u32(a + 64);
    f.static_values = v.u32(a + 68);
    f.block_index = v.u32(a + 72);
    f.blocks = v.u32(a + 76);
    f.quantized = v.u32(a + 80);
    f.quantized_size = v.u32(a + 84);
    const View data = v.sub(p.ptr(a + 88), v.u32(a + 92));
    const Le b{data.p, data.n};
    if (v.bad || !b.n || !(out.length > 0) || f.poses < 2 || f.poses > 100000 || f.tracks == 0 || f.tracks > 1024 ||
        f.float_tracks > 1024 || f.block < 8 || f.block > 4096 || (f.block & (f.block - 1)) || f.preserved > f.block ||
        f.numd > 100000 || uint64_t(f.blocks) * f.block < f.poses || f.blocks > 100000)
        return false;
    const auto in = [&](uint64_t off, uint64_t len) { return off + len <= b.n; };
    const uint32_t ntracks = f.tracks + f.float_tracks;
    if (!in(f.static_mask, 2ull * ntracks) || !in(f.offsets, 4ull * f.numd) || !in(f.scales, 4ull * f.numd) ||
        (f.bits && !in(f.bits, f.numd)) || !in(f.block_index, 4ull * f.blocks) || !in(f.quantized, f.quantized_size) ||
        f.offsets < f.static_values)
        return false;
    // 0x0101BD70: the moving values are bits 6-15 of each track's mask (and a float track whose mask is 0).
    std::vector<uint16_t> masks(ntracks);
    uint32_t moving = 0, fixed = 0;
    for (uint32_t i = 0; i < ntracks; i++) masks[i] = b.u16(f.static_mask + 2ull * i);
    for (uint32_t i = 0; i < f.tracks; i++) {
        const uint16_t m = masks[i];
        for (int bit = 6; bit < 16; bit++) moving += (m >> bit) & 1;
        if ((m & 3) != 2) fixed += 3 - (((m >> 8) & 1) + ((m >> 7) & 1) + ((m >> 6) & 1));
        if (((m >> 2) & 3) != 2) fixed += 4 - (((m >> 12) & 1) + ((m >> 11) & 1) + ((m >> 10) & 1) + ((m >> 9) & 1));
        if (((m >> 4) & 3) != 2) fixed += 3 - (((m >> 15) & 1) + ((m >> 14) & 1) + ((m >> 13) & 1));
    }
    for (uint32_t i = 0; i < f.float_tracks; i++) (masks[f.tracks + i] == 0 ? moving : fixed) += 1;
    if (moving != f.numd || !in(f.static_values, 4ull * fixed)) return false;
    std::vector<float> constants(fixed);
    for (uint32_t i = 0; i < fixed; i++) constants[i] = b.f32(f.static_values + 4ull * i);

    // Every moving value's frames, block by block.
    const uint32_t frames = f.blocks * f.block;
    std::vector<float> values(size_t(f.numd) * frames);
    std::vector<float> c(f.block);
    for (uint32_t k = 0; k < f.blocks; k++) {
        uint64_t pos = uint64_t(f.quantized) + b.u32(f.block_index + 4ull * k);
        for (uint32_t d = 0; d < f.numd; d++) {
            const int bw = f.bits ? b.u8(f.bits + d) : f.max_bits;
            if (bw < 1 || bw > 16) return false;
            pos = wavelet_block(b, f, pos, d, bw, c.data());
            wavelet_inverse(c.data(), f.block);
            std::memcpy(&values[size_t(d) * frames + size_t(k) * f.block], c.data(), f.block * sizeof(float));
        }
    }

    // 0x0101B980: each pose's transforms, the moving values and the constants taken in the mask's order. A stored w
    // of +-2 means w is to be made from x, y and z, with that sign.
    for (uint32_t pose = 0; pose < f.poses; pose++) {
        size_t di = 0, si = 0;
        const auto take = [&](uint32_t high, uint32_t bit) {
            return (high & bit) ? values[(di++) * frames + pose] : constants[si++];
        };
        std::vector<Qs> frame(f.tracks);
        for (uint32_t k = 0; k < f.tracks; k++) {
            const uint32_t m = masks[k], high = m >> 6;
            Qs& x = frame[k];
            if ((m & 3) != 2) {
                x.t[0] = take(high, 4);
                x.t[1] = take(high, 2);
                x.t[2] = take(high, 1);
            } else {
                x.t[0] = x.t[1] = x.t[2] = 0.f;
            }
            if (((m >> 2) & 3) != 2) {
                const float qx = take(high, 0x40), qy = take(high, 0x20), qz = take(high, 0x10);
                float qw = take(high, 8);
                if (std::fabs(qw) == 2.0f) {
                    const float s = qz * qz + qx * qx + qy * qy, r = s <= 1.f ? 1.f - s : 0.f;
                    qw = qw <= 0.f ? -std::sqrt(r) : std::sqrt(r);
                }
                x.q[0] = qx;
                x.q[1] = qy;
                x.q[2] = qz;
                x.q[3] = qw;
            }
            if (((m >> 4) & 3) != 2) {
                x.s[0] = take(high, 0x200);
                x.s[1] = take(high, 0x100);
                x.s[2] = take(high, 0x80);
            }
        }
        out.frames.push_back(std::move(frame));
    }
    if (out.track_bones.size() < f.tracks) out.track_bones.resize(f.tracks, -1);
    return true;
}

// hkaInterleavedSkeletalAnimation (5.5): +12 duration, +16 transform tracks, +36 transforms*, +40 n; or a wavelet one.
// hkaAnimationBinding: +4 transformTrackToBoneIndices*, +8 n.
bool clip(const Pack& p, Clip& out) {
    const View& v = p.data;
    const uint32_t binding = p.first("hkaAnimationBinding");
    if (binding == 0xFFFFFFFFu) return false;
    const int nbound = v.i32(binding + 8);
    const uint32_t bound = p.ptr(binding + 4);
    for (int i = 0; i < nbound && i < 1024; i++) out.track_bones.push_back(v.i16(bound + size_t(i) * 2));
    if (const uint32_t w = p.first("hkaWaveletSkeletalAnimation"); w != 0xFFFFFFFFu) return wavelet_clip(p, w, out);
    const uint32_t a = p.first("hkaInterleavedSkeletalAnimation");
    if (a == 0xFFFFFFFFu) return false;
    out.length = v.f32(a + 12);
    const int ntracks = v.i32(a + 16), n = v.i32(a + 40);
    const uint32_t transforms = p.ptr(a + 36);
    if (ntracks <= 0 || n < ntracks || n % ntracks || !(out.length > 0)) return false;
    for (int f = 0; f < n / ntracks; f++) {
        std::vector<Qs> frame;
        for (int t = 0; t < ntracks; t++)
            frame.push_back(read_qs(v, transforms + (size_t(f) * size_t(ntracks) + size_t(t)) * 48));
        out.frames.push_back(std::move(frame));
    }
    if (int(out.track_bones.size()) < ntracks) out.track_bones.resize(size_t(ntracks), -1);
    return !v.bad;
}

// ---- Transforms ----

struct Affine {  // rows: x' = m[0..2] . p + m[3], ...
    float m[3][4];
};

Affine from_qs(const Qs& x) {
    const float qx = x.q[0], qy = x.q[1], qz = x.q[2], qw = x.q[3];
    const float r[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                           {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                           {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
    Affine a;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) a.m[i][j] = r[i][j] * x.s[j];
        a.m[i][3] = x.t[i];
    }
    return a;
}

Affine mul(const Affine& a, const Affine& b) {
    Affine c;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float s = j == 3 ? a.m[i][3] : 0.f;
            for (int k = 0; k < 3; k++) s += a.m[i][k] * b.m[k][j];
            c.m[i][j] = s;
        }
    }
    return c;
}

Affine inverse(const Affine& a) {
    const float(*m)[4] = a.m;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Affine r{};
    if (std::fabs(det) < 1e-12f) {
        for (int i = 0; i < 3; i++) r.m[i][i] = 1;
        return r;
    }
    const float k = 1.f / det;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    for (int i = 0; i < 3; i++) r.m[i][3] = -(r.m[i][0] * m[0][3] + r.m[i][1] * m[1][3] + r.m[i][2] * m[2][3]);
    return r;
}

std::vector<Affine> world(const std::vector<int>& parents, const std::vector<Qs>& local) {
    std::vector<Affine> w(local.size());
    for (size_t i = 0; i < local.size(); i++) {
        const Affine l = from_qs(local[i]);
        w[i] = parents[i] < 0 ? l : mul(w[size_t(parents[i])], l);
    }
    return w;
}

Qs blend(const Qs& a, const Qs& b, float f) {
    Qs r;
    for (int c = 0; c < 3; c++) {
        r.t[c] = a.t[c] + (b.t[c] - a.t[c]) * f;
        r.s[c] = a.s[c] + (b.s[c] - a.s[c]) * f;
    }
    float dot = 0;
    for (int c = 0; c < 4; c++) dot += a.q[c] * b.q[c];
    const float sign = dot < 0 ? -1.f : 1.f;  // the shorter way round
    float len = 0;
    for (int c = 0; c < 4; c++) {
        r.q[c] = a.q[c] + (sign * b.q[c] - a.q[c]) * f;
        len += r.q[c] * r.q[c];
    }
    len = len > 0 ? 1.f / std::sqrt(len) : 0.f;
    for (float& c : r.q) c *= len;
    return r;
}

// ---- The models ----

std::string game_dir() {
    const char* root = std::getenv("PS3_VFS_ROOT");
    if (!root || !*root) return {};
    return std::string(root) + "/PS3_GAME/USRDIR/";
}

// The texture by name, in the model's list (added from `all` the first time); -1 if there is none.
int texture_index(Model& m, const std::vector<Texture>& all, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t t = 0; t < m.textures.size(); t++)
        if (m.textures[t].name == name) return int(t);
    for (const Texture& t : all)
        if (t.name == name) {
            m.textures.push_back(t);
            return int(m.textures.size() - 1);
        }
    return -1;
}

void textures_for(Model& m, const std::vector<Texture>& all, const Flver& f) {
    for (size_t i = 0; i < m.meshes.size() && i < f.mesh_textures.size(); i++) {
        m.meshes[i].texture = texture_index(m, all, f.mesh_textures[i]);
        m.meshes[i].specular = texture_index(m, all, f.mesh_speculars[i]);
    }
}

void bind_bounds(Model& m) {
    bool first = true;
    for (const Mesh& mesh : m.meshes)
        for (const Vertex& v : mesh.vertices)
            for (int c = 0; c < 3; c++) {
                if (first || v.pos[c] < m.lo[c]) m.lo[c] = v.pos[c];
                if (first || v.pos[c] > m.hi[c]) m.hi[c] = v.pos[c];
                if (c == 2) first = false;
            }
}

// parts/wp_a_0258.partsbnd: the weapon's FLVER0 and its textures.
std::unique_ptr<Model> load_moonlight(const std::string& dir, std::string& why) {
    std::vector<uint8_t> file;
    if (!read_file(dir + "parts/wp_a_0258.partsbnd", file)) {
        why = "parts/wp_a_0258.partsbnd could not be read";
        return nullptr;
    }
    const View v{file.data(), file.size()};
    const auto entries = bnd3(v);
    const Entry* fl = find(entries, "wp_a_0258.flver");
    const Entry* tx = find(entries, "wp_a_0258.tpf");
    Flver f;
    if (!fl || !tx || !flver0(fl->data, f)) {
        why = "wp_a_0258.partsbnd has no model that reads";
        return nullptr;
    }
    auto m = std::make_unique<Model>();
    m->meshes = std::move(f.meshes);
    textures_for(*m, tpf(tx->data), f);
    for (Mesh& mesh : m->meshes) {  // a rigid model: no skinning
        mesh.bones.clear();
        mesh.weights.clear();
    }
    bind_bounds(*m);
    return m;
}

// chr/c3110: the crystal lizard. Its model and textures (c3110.chrbnd), its skeleton and animations (c3110.anibnd).
std::unique_ptr<Model> load_lizard(const std::string& dir, std::string& why) {
    std::vector<uint8_t> chr, ani;
    if (!read_file(dir + "chr/c3110/c3110.chrbnd", chr) || !read_file(dir + "chr/c3110/c3110.anibnd", ani)) {
        why = "chr/c3110/c3110.chrbnd or .anibnd could not be read";
        return nullptr;
    }
    const auto chr_entries = bnd3(View{chr.data(), chr.size()});
    const auto ani_entries = bnd3(View{ani.data(), ani.size()});
    const Entry* fl = find(chr_entries, "c3110.flver");
    const Entry* tx = find(chr_entries, "c3110.tpf");
    const Entry* sk = find(ani_entries, "skeleton.hkx");
    Flver f;
    Pack skp;
    Skeleton skel;
    if (!fl || !tx || !sk || !flver0(fl->data, f) || !pack(sk->data, skp) || !skeleton(skp, skel)) {
        why = "c3110's model or skeleton does not read";
        return nullptr;
    }
    // The dance, a00_3301: up on its hind legs, swaying, a 3 s loop (DES_UI_LIZARD_ANIM=aNN_NNNN another of its
    // animations; a00_0000 is its idle). One that does not read falls back to the dance.
    const char* want = std::getenv("DES_UI_LIZARD_ANIM");
    Clip c;
    std::string used;
    for (std::string name : {std::string(want && *want ? want : "a00_3301"), std::string("a00_3301")}) {
        const Entry* e = find(ani_entries, lower(name) + ".hkx");
        Pack ap;
        c = Clip{};
        if (e && pack(e->data, ap) && clip(ap, c)) {
            used = name;
            break;
        }
    }
    if (used.empty()) {
        why = "c3110 has no animation that reads";
        return nullptr;
    }

    auto m = std::make_unique<Model>();
    m->meshes = std::move(f.meshes);
    textures_for(*m, tpf(tx->data), f);
    m->parents = skel.parents;
    m->reference = skel.reference;
    m->length = c.length;
    for (const auto& tracks : c.frames) {
        std::vector<Qs> bones = skel.reference;
        for (size_t t = 0; t < tracks.size(); t++) {
            const int b = c.track_bones[t];
            if (b >= 0 && b < int(bones.size())) bones[size_t(b)] = tracks[t];
        }
        m->frames.push_back(std::move(bones));
    }
    // The vertices' bones: the FLVER's, as the skeleton's (by name).
    std::vector<int> to_skeleton(f.bone_names.size(), -1);
    for (size_t i = 0; i < f.bone_names.size(); i++)
        for (size_t k = 0; k < skel.names.size(); k++)
            if (skel.names[k] == f.bone_names[i]) to_skeleton[i] = int(k);
    for (Mesh& mesh : m->meshes)
        for (auto& b : mesh.bones)
            for (uint16_t& i : b) i = uint16_t(i < to_skeleton.size() && to_skeleton[i] >= 0 ? to_skeleton[i] : 0);
    // The box: around every frame of the animation (not the rest pose, which the dance never takes), so the lizard
    // stays put in its place as it moves.
    std::vector<std::vector<Vertex>> posed;
    bool first = true;
    for (size_t fi = 0; fi < m->frames.size(); fi++) {
        pose(*m, m->length * double(fi) / double(std::max<size_t>(m->frames.size() - 1, 1)), posed);
        for (const auto& mesh : posed)
            for (const Vertex& v : mesh)
                for (int k = 0; k < 3; k++) {
                    m->lo[k] = first ? v.pos[k] : std::min(m->lo[k], v.pos[k]);
                    m->hi[k] = first ? v.pos[k] : std::max(m->hi[k], v.pos[k]);
                    if (k == 2) first = false;
                }
    }
    if (first) bind_bounds(*m);
    std::fprintf(stderr, "[ui] model: the crystal lizard, %s (%zu frames, %.2f s)\n", used.c_str(), m->frames.size(),
                 m->length);
    return m;
}

}  // namespace

const Model* get(const std::string& name) {
    static std::map<std::string, std::unique_ptr<Model>> loaded;
    static std::map<std::string, bool> tried;
    if (const auto it = loaded.find(name); it != loaded.end()) return it->second.get();
    if (tried[name]) return nullptr;
    tried[name] = true;
    const std::string dir = game_dir();
    std::string why = "no game folder (PS3_VFS_ROOT)";
    std::unique_ptr<Model> m;
    if (!dir.empty()) {
        if (name == "moonlight") m = load_moonlight(dir, why);
        else if (name == "lizard") m = load_lizard(dir, why);
        else why = "no such model";
    }
    if (!m) {
        std::fprintf(stderr, "[ui] model %s: %s\n", name.c_str(), why.c_str());
        return nullptr;
    }
    return (loaded[name] = std::move(m)).get();
}

const std::vector<Mesh>& meshes(const Model& m) { return m.meshes; }
const std::vector<Texture>& textures(const Model& m) { return m.textures; }

void bounds(const Model& m, float lo[3], float hi[3]) {
    for (int c = 0; c < 3; c++) {
        lo[c] = m.lo[c];
        hi[c] = m.hi[c];
    }
}

double duration(const Model& m) { return m.frames.empty() ? 0.0 : m.length; }

namespace {

// A turn of `degrees` about the unit axis (x, y, z), and a product of two, as rows.
void turn_about(float x, float y, float z, float degrees, float out[9]) {
    const float a = degrees * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a), k = 1.f - c;
    const float r[9] = {c + x * x * k,     x * y * k - z * s, x * z * k + y * s,  //
                        y * x * k + z * s, c + y * y * k,     y * z * k - x * s,  //
                        z * x * k - y * s, z * y * k + x * s, c + z * z * k};
    std::memcpy(out, r, sizeof r);
}

void times(const float a[9], const float b[9], float out[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::memcpy(out, r, sizeof r);
}

}  // namespace

float spin(const std::string& name) {
    const auto knob = [](const char* var) {
        const char* v = std::getenv(var);
        char* end = nullptr;
        const float f = v && *v ? std::strtof(v, &end) : 0.f;
        return end && !*end && std::isfinite(f) ? std::clamp(f, -3600.f, 3600.f) : 0.f;
    };
    static const float sword = knob("DES_CURSOR_SPIN"), lizard = knob("DES_UI_LIZARD_SPIN");
    return name == "moonlight" ? sword : name == "lizard" ? lizard : 0.f;
}

Look look(const std::string& name, float turn) {
    // The models have +Y up, the screen y down: turned over about x (which keeps them right-handed).
    static const float kOver[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    Look l{};
    float a[9], b[9];
    if (name == "moonlight") {
        // The blade lies along -Y. Turned 28 degrees about it (the light catches its flat), over, and then the tip,
        // now at the screen's bottom, turned round to the top left.
        turn_about(0, 1, 0, 28.f + turn, a);
        times(kOver, a, b);
        turn_about(0, 0, 1, 135.f, a);
        times(a, b, l.rotation);
        const float light[3] = {-0.4f, -0.6f, 0.7f}, tint[3] = {0.95f, 1.0f, 1.08f};
        std::memcpy(l.light, light, sizeof light);
        std::memcpy(l.tint, tint, sizeof tint);
        l.ambient = 0.62f;
        l.alpha_test = 0.5f;
    } else {
        // Seen from 35 degrees to its left (plus turn), from 28 degrees above.
        turn_about(0, 1, 0, -35.f + turn, a);
        turn_about(1, 0, 0, -28.f, b);
        times(b, a, b);
        times(kOver, b, l.rotation);
        const float light[3] = {-0.3f, -0.7f, 0.65f};
        std::memcpy(l.light, light, sizeof light);
        l.tint[0] = l.tint[1] = l.tint[2] = 1.f;
        l.ambient = 0.55f;
        l.alpha_test = 0.f;
    }
    return l;
}

bool pose(const Model& m, double t, std::vector<std::vector<Vertex>>& out) {
    if (m.frames.empty() || !(m.length > 0)) return false;
    // The frame pair around t, looped.
    double u = std::fmod(t, m.length);
    if (u < 0) u += m.length;
    const double at = u / m.length * double(m.frames.size() - 1);
    const size_t f0 = std::min(size_t(at), m.frames.size() - 1), f1 = std::min(f0 + 1, m.frames.size() - 1);
    const float f = float(at - double(f0));
    std::vector<Qs> local(m.reference.size());
    for (size_t b = 0; b < local.size(); b++) local[b] = blend(m.frames[f0][b], m.frames[f1][b], f);
    const std::vector<Affine> posed = world(m.parents, local), rest = world(m.parents, m.reference);
    std::vector<Affine> skin(posed.size());
    for (size_t b = 0; b < skin.size(); b++) skin[b] = mul(posed[b], inverse(rest[b]));

    out.resize(m.meshes.size());
    for (size_t i = 0; i < m.meshes.size(); i++) {
        const Mesh& mesh = m.meshes[i];
        out[i] = mesh.vertices;
        if (mesh.bones.size() != mesh.vertices.size()) continue;
        for (size_t k = 0; k < mesh.vertices.size(); k++) {
            const Vertex& src = mesh.vertices[k];
            Vertex& dst = out[i][k];
            float wsum = 0;
            for (float w : mesh.weights[k]) wsum += w;
            if (wsum <= 0) continue;
            float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            for (int j = 0; j < 4; j++) {
                const float w = mesh.weights[k][size_t(j)] / wsum;
                if (w <= 0) continue;
                const Affine& a = skin[std::min<size_t>(mesh.bones[k][size_t(j)], skin.size() - 1)];
                for (int r = 0; r < 3; r++) {
                    p[r] += w * (a.m[r][0] * src.pos[0] + a.m[r][1] * src.pos[1] + a.m[r][2] * src.pos[2] + a.m[r][3]);
                    n[r] += w * (a.m[r][0] * src.normal[0] + a.m[r][1] * src.normal[1] + a.m[r][2] * src.normal[2]);
                }
            }
            std::memcpy(dst.pos, p, sizeof p);
            std::memcpy(dst.normal, n, sizeof n);
        }
    }
    return true;
}

}  // namespace ui::models
