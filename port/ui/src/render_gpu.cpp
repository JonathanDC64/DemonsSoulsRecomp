// Written for Demon's Souls. The menus' renderer on ps3recomp's GPU layer (renderer.hpp, docs/120): render_d3d12.cpp,
// RmlUi's renderer on D3D12 (docs/76), with each of its D3D12 calls made through gpu.h instead, so it can draw on any
// of the layer's backends. The same shaders (on D3D12 compiled by the layer's compile service with today's FXC call,
// so the bytecode is the same), layers, filters, clip mask, HDR pass and the game's two models (docs/112), and the same
// command for each D3D12 call render_d3d12.cpp makes: on D3D12 it draws today's pixels (ui_render_compare checks it).
// Built with PS3RECOMP_RSX_HAL (port/CMakeLists.txt's route switch); without it this file is empty.
//
// As in render_d3d12.cpp: the UI is drawn into a layer, a multisampled target, all of them sharing one stencil buffer.
// Filters work in single-sampled targets of the same size, and end_frame composites the base layer over the back
// buffer. A frame's work is one recording, submitted ahead of the Present. Up to three frames are in flight, each on
// its own recorder slot with its own ring for constants and quads; what RmlUi lets go of is freed once the GPU is past
// the frames that used it.
#if defined(PS3RECOMP_RSX_HAL)

#include "renderer.hpp"
#include "souls_model.hpp"

#include "gpu.h"
#include "gpu_shader.h"
#include "gpu_vt_cmd.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ui::gpu {
namespace {

constexpr int kFrames = 3;                     // frames in flight (recorder slots)
constexpr uint32_t kSrvCapacity = 4096;        // textures and targets, in one table pool
constexpr uint32_t kNone = ~0u;                // no table entry
constexpr size_t kRingSize = size_t(4) << 20;  // a frame's constants and quads
constexpr size_t kPageSize = size_t(4) << 20;  // geometry and uploads, in blocks of powers of two
constexpr int kMinClass = 8, kMaxClass = 22;   // 256 bytes to 4 MB; larger ones get a buffer of their own
constexpr uint64_t kPlacementAlign = 512;      // a texture upload's (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)
constexpr uint32_t kPitchAlign = 256;          // a read back's rows (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)
constexpr gpu_format kColorFormat = GPU_FORMAT_R8G8B8A8_UNORM;
constexpr gpu_format kStencilFormat = GPU_FORMAT_D24_UNORM_S8_UINT;
constexpr uint32_t kLayerSamples = 2;          // RmlUi's NUM_MSAA_SAMPLES
constexpr int kMaxStops = 16;
// Upload memory: vertices, indices, constants and texture uploads.
constexpr uint32_t kUploadUse = GPU_USE_VERTEX | GPU_USE_INDEX | GPU_USE_CONSTANT | GPU_USE_COPY_SRC;

// ---- Shaders (RmlUi's, as HLSL for vs_5_0 / ps_5_0) ----

#define DES_UI_HLSL_COMMON                                                                           \
    "struct VS_IN { float2 position : POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };\n"   \
    "struct PS_IN { float4 position : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };\n" \
    "Texture2D t0 : register(t0);\n"                                                                 \
    "Texture2D t1 : register(t1);\n"                                                                 \
    "SamplerState s_wrap : register(s0);\n"                                                          \
    "SamplerState s_clamp : register(s1);\n"

// RmlUi's geometry, through its transform.
const char kVsMain[] = DES_UI_HLSL_COMMON R"(
cbuffer Common : register(b0) { float4x4 m_transform; float2 m_translate; };
PS_IN main(VS_IN v) {
    PS_IN o;
    o.position = mul(m_transform, float4(v.position + m_translate, 0.0, 1.0));
    o.color = v.color;
    o.uv = v.uv;
    return o;
}
)";

// The filters' and the composites' quads, in clip space.
const char kVsQuad[] = DES_UI_HLSL_COMMON R"(
PS_IN main(VS_IN v) {
    PS_IN o;
    o.position = float4(v.position, 0.0, 1.0);
    o.color = v.color;
    o.uv = v.uv;
    return o;
}
)";

const char kPsColor[] = DES_UI_HLSL_COMMON R"(
float4 main(PS_IN i) : SV_Target { return i.color; }
)";

const char kPsTexture[] = DES_UI_HLSL_COMMON R"(
float4 main(PS_IN i) : SV_Target { return i.color * t0.Sample(s_wrap, i.uv); }
)";

const char kPsPassthrough[] = DES_UI_HLSL_COMMON R"(
float4 main(PS_IN i) : SV_Target { return t0.Sample(s_clamp, i.uv); }
)";

// The finished UI onto an FP16 scRGB back buffer (set_hdr): decoded, scaled to the game's paper white, premultiplied
// again for the blend.
const char kPsHdrOut[] = DES_UI_HLSL_COMMON R"(
cbuffer Hdr : register(b1) { float m_scale; float m_srgb; float2 m_pad; };
float4 main(PS_IN i) : SV_Target {
    float4 c = t0.Sample(s_clamp, i.uv);
    if (c.a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    float3 u = saturate(c.rgb / c.a);
    float3 l = m_srgb > 0.5 ? (u <= 0.04045 ? u / 12.92 : pow((u + 0.055) / 1.055, 2.4)) : pow(u, 2.2);
    return float4(l * m_scale * c.a, c.a);
}
)";

// The colour matrix filters, in premultiplied space (RmlUi's: rgb only).
const char kPsColorMatrix[] = DES_UI_HLSL_COMMON R"(
cbuffer ColorMatrix : register(b1) { float4x4 m_color_matrix; };
float4 main(PS_IN i) : SV_Target {
    float4 c = t0.Sample(s_clamp, i.uv);
    return float4(mul(m_color_matrix, c).rgb, c.a);
}
)";

const char kPsBlendMask[] = DES_UI_HLSL_COMMON R"(
float4 main(PS_IN i) : SV_Target { return t0.Sample(s_clamp, i.uv) * t1.Sample(s_clamp, i.uv).a; }
)";

// One pass of the separable Gaussian: seven taps along m_texel, none from outside [m_min, m_max].
const char kPsBlur[] = DES_UI_HLSL_COMMON R"(
cbuffer Blur : register(b1) { float2 m_texel; float2 m_min; float2 m_max; float2 m_pad; float4 m_weights; };
float4 tap(float2 uv, float w) {
    float2 r = step(m_min, uv) * step(uv, m_max);
    return t0.Sample(s_clamp, uv) * (r.x * r.y * w);
}
float4 main(PS_IN i) : SV_Target {
    return tap(i.uv - 3.0 * m_texel, m_weights.w) + tap(i.uv - 2.0 * m_texel, m_weights.z) +
           tap(i.uv - m_texel, m_weights.y) + tap(i.uv, m_weights.x) + tap(i.uv + m_texel, m_weights.y) +
           tap(i.uv + 2.0 * m_texel, m_weights.z) + tap(i.uv + 3.0 * m_texel, m_weights.w);
}
)";

const char kPsDropShadow[] = DES_UI_HLSL_COMMON R"(
cbuffer DropShadow : register(b1) { float2 m_min; float2 m_max; float4 m_color; };
float4 main(PS_IN i) : SV_Target {
    float2 r = step(m_min, i.uv) * step(i.uv, m_max);
    return t0.Sample(s_clamp, i.uv).a * r.x * r.y * m_color;
}
)";

// RmlUi's gradients: linear (0), radial (1) and conic (2), and the same repeating (3 to 5). One stop's position to a
// register (.x), and the stops unrolled, so the shader indexes nothing at run time.
const char kPsGradient[] = DES_UI_HLSL_COMMON R"(
cbuffer Gradient : register(b0) {
    float4x4 m_transform; float2 m_translate; int m_func; int m_num_stops;
    float2 m_p; float2 m_v;
    float2 m_range; float2 m_pad;
    float4 m_stop_colors[16];
    float4 m_stop_positions[16];
};
#define STOP(k) if (k < m_num_stops) color = lerp(color, m_stop_colors[k], \
    smoothstep(m_stop_positions[k - 1].x, m_stop_positions[k].x, t));
float4 main(PS_IN i) : SV_Target {
    float2 V = i.uv - m_p;
    float t;
    if (m_func == 0 || m_func == 3) {
        t = dot(m_v, V) / dot(m_v, m_v);
    } else if (m_func == 1 || m_func == 4) {
        t = length(m_v * V);
    } else {
        float2 R = float2(V.x * m_v.x + V.y * m_v.y, V.y * m_v.x - V.x * m_v.y);
        t = 0.5 + atan2(-R.x, R.y) / 6.28318531;
    }
    if (m_func >= 3) {
        float d = m_range.y - m_range.x;
        float x = t - m_range.x;
        t = m_range.x + x - d * floor(x / d);
    }
    float4 color = m_stop_colors[0];
    STOP(1) STOP(2) STOP(3) STOP(4) STOP(5) STOP(6) STOP(7) STOP(8)
    STOP(9) STOP(10) STOP(11) STOP(12) STOP(13) STOP(14) STOP(15)
    return i.color * color;
}
)";

// The game's models (docs/112), shaded as the previews were: the diffuse texture under a light from the upper left
// (ambient plus Lambert, both sides of a face lit as the side toward the viewer), the specular texture's colour as a
// highlight. View space: x right, y down, z toward the viewer. An opaque mesh drops its texels under the alpha test;
// a blended one (the blade) keeps only its faces toward the viewer, as a single sheet of glass, premultiplied. The
// effects (docs/112) are drawn with the same shaders: the glow's mask, the gems' glitter, the star glints.
#define DES_UI_HLSL_MODEL                                                                                  \
    "struct MODEL_IN { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD; };\n"     \
    "struct MODEL_PS { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD; };\n"  \
    "Texture2D t0 : register(t0);\n"                                                                       \
    "Texture2D t1 : register(t1);\n"                                                                       \
    "SamplerState s_wrap : register(s0);\n"                                                                \
    "cbuffer Model : register(b0) { float4x4 m_clip; float4 m_view[3]; float4 m_light; float4 m_tint;\n"   \
    "                                float4 m_flags; float4 m_fx; };\n"

const char kVsModel[] = DES_UI_HLSL_MODEL R"(
MODEL_PS main(MODEL_IN v) {
    MODEL_PS o;
    o.position = mul(m_clip, float4(v.position, 1.0));
    o.normal = float3(dot(m_view[0].xyz, v.normal), dot(m_view[1].xyz, v.normal), dot(m_view[2].xyz, v.normal));
    o.uv = v.uv;
    return o;
}
)";

// m_light: toward the light, and the ambient part. m_tint: a colour, and the opacity. m_flags: x faces toward the
// viewer only, y opaque, z the alpha test. m_fx: x the time in seconds, y the gems' glitter (0: none), z what is drawn:
// 0 the model; 1 the glow's mask, the mesh's alpha in the tint's colour; 2 star glints (uv from -1 to 1 across each,
// the normal's x its brightness), added to what is there.
const char kPsModel[] = DES_UI_HLSL_MODEL R"(
float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float4 main(MODEL_PS i) : SV_Target {
    if (m_fx.z > 1.5) {
        // Four thin rays, narrowing to their ends, over a bright core and a faint halo.
        float2 p = abs(i.uv);
        float rays = pow(saturate(1.0 - p.x), 3.0) * exp(-p.y * p.y * 220.0) +
                     pow(saturate(1.0 - p.y), 3.0) * exp(-p.x * p.x * 220.0);
        float r2 = dot(p, p);
        float glint = saturate((rays * 1.2 + exp(-r2 * 60.0) + exp(-r2 * 8.0) * 0.25) * i.normal.x);
        return float4(m_tint.rgb * (glint * m_tint.a), 0.0);
    }
    float4 d = t0.Sample(s_wrap, i.uv);
    float3 s = t1.Sample(s_wrap, i.uv).rgb;
    float len = length(i.normal);
    float3 n = len > 0.000001 ? i.normal / len : float3(0.0, 0.0, 1.0);
    if (m_flags.x > 0.5 && n.z <= 0.0) discard;
    if (m_flags.y > 0.5 && d.a < m_flags.z) discard;
    if (m_fx.z > 0.5) {
        float m = d.a * m_tint.a;
        return float4(m_tint.rgb * m, m);
    }
    if (n.z < 0.0) n = -n;
    float3 L = normalize(m_light.xyz);
    float3 H = normalize(L + float3(0.0, 0.0, 1.0));
    float3 rgb = d.rgb * (m_light.w + (1.0 - m_light.w) * saturate(dot(n, L)));
    rgb = saturate((rgb + s * pow(saturate(dot(n, H)), 24.0) * 0.9) * m_tint.rgb);
    if (m_fx.y > 0.0) {
        // A gem flashes white now and then: the texture in cells, each on its own clock and not at every turn of
        // it. A gem is a texel more blue, green or violet than red (the skin is grey, brown and pink).
        float gem = smoothstep(0.05, 0.2, max(d.g, d.b) - d.r) *
                    smoothstep(0.1, 0.25, max(d.r, max(d.g, d.b)) - min(d.r, min(d.g, d.b)));
        float2 cell = floor(i.uv * float2(64.0, 32.0));
        float period = 1.5 + 2.5 * hash(cell + 17.0);
        float phase = m_fx.x / period + hash(cell);
        float on = step(hash(cell + floor(phase) * 3.1), 0.4);
        float flash = on * pow(saturate(1.0 - abs(frac(phase) - 0.5) * 12.0), 2.0);
        rgb = saturate(rgb + gem * flash * m_fx.y);
    }
    float a = (m_flags.y > 0.5 ? 1.0 : d.a) * m_tint.a;
    return float4(rgb * a, a);
}
)";

enum Program {
    kColor, kTexture, kGradient, kPassthrough, kColorMatrix, kBlendMask, kBlur, kDropShadow, kHdrOut,
    kModel, kModelBlend,  // (the model shaders: their own vertices, and the depth buffer)
    kProgramCount
};
const char* const kProgramPs[kProgramCount] = {kPsColor,       kPsTexture,   kPsGradient, kPsPassthrough,
                                               kPsColorMatrix, kPsBlendMask, kPsBlur,     kPsDropShadow,
                                               kPsHdrOut,      kPsModel,     kPsModel};
const char* const kProgramName[kProgramCount] = {"color",        "texture",    "gradient", "passthrough",
                                                 "color matrix", "blend mask", "blur",     "drop shadow",
                                                 "hdr out",      "model",      "model blend"};

enum Blend { kPremultiplied, kReplace, kFactor, kNoColor };
enum Stencil { kStencilOff, kStencilTest, kStencilSet, kStencilIntersect, kStencilAuto };

struct CommonConstants {
    float transform[16];
    float translate[2];
    float pad[2];
};
struct GradientConstants {
    float transform[16];
    float translate[2];
    int32_t func;
    int32_t num_stops;
    float p[2];
    float v[2];
    float range[2];
    float pad[2];
    float colors[kMaxStops][4];
    float positions[kMaxStops][4];
};
struct BlurConstants {
    float texel[2];
    float min[2];
    float max[2];
    float pad[2];
    float weights[4];
};
struct DropShadowConstants {
    float min[2];
    float max[2];
    float color[4];
};
struct ModelConstants {
    float clip[16];     // model space to clip space (column major)
    float view[3][4];   // model space to view space, the rows (the normals)
    float light[4];
    float tint[4];
    float flags[4];
    float fx[4];
};

// A Matrix4f as HLSL reads it (column major).
void store(const Rml::Matrix4f& m, float* out) {
    if constexpr (std::is_same_v<Rml::Matrix4f, Rml::RowMajorMatrix4f>) std::memcpy(out, m.Transpose().data(), 64);
    else std::memcpy(out, m.data(), 64);
}

Rml::Colourf to_float(Rml::ColourbPremultiplied c) {
    Rml::Colourf f;
    for (int i = 0; i < 4; i++) f[i] = float(c[i]) / 255.f;
    return f;
}

// ---- Filters and shaders (RmlUi's) ----

enum class FilterType { Invalid, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };
struct CompiledFilter {
    FilterType type = FilterType::Invalid;
    float blend_factor = 1.f;  // Passthrough (opacity)
    float sigma = 0.f;         // Blur, DropShadow
    Rml::Vector2f offset;      // DropShadow
    Rml::ColourbPremultiplied color;
    Rml::Matrix4f color_matrix;  // ColorMatrix
};

enum class GradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };
struct CompiledShader {
    GradientFunction function = GradientFunction::Linear;
    Rml::Vector2f p, v;
    std::vector<float> stop_positions;
    std::vector<Rml::Colourf> stop_colors;
    std::string model;  // decorator: shader("model:<name>"): one of the game's models, fitted into the element
};

// SigmaToParameters and SetBlurWeights, RmlUi's.
void sigma_to_parameters(float desired, int& pass_level, float& sigma) {
    constexpr int kMaxPasses = 10;
    constexpr float kMaxSinglePassSigma = 3.0f;
    pass_level = Rml::Math::Clamp(Rml::Math::Log2(int(desired * (2.f / kMaxSinglePassSigma))), 0, kMaxPasses);
    sigma = Rml::Math::Clamp(desired / float(1 << pass_level), 0.0f, kMaxSinglePassSigma);
}

void blur_weights(float* out, float sigma) {
    float w[4], sum = 0.f;
    for (int i = 0; i < 4; i++) {
        w[i] = Rml::Math::Absolute(sigma) < 0.1f
                   ? float(i == 0)
                   : Rml::Math::Exp(-float(i * i) / (2.0f * sigma * sigma)) /
                         (Rml::Math::SquareRoot(2.f * Rml::Math::RMLUI_PI) * sigma);
        sum += (i == 0 ? 1.f : 2.f) * w[i];
    }
    for (int i = 0; i < 4; i++) out[i] = w[i] / sum;
}

// The texture coordinates of a region's outermost texel centres, so bilinear taps take nothing from outside it.
void region_limits(float* min, float* max, Rml::Rectanglei r, int w, int h) {
    min[0] = (float(r.Left()) + 0.5f) / float(w);
    min[1] = (float(r.Top()) + 0.5f) / float(h);
    max[0] = (float(r.Right()) - 0.5f) / float(w);
    max[1] = (float(r.Bottom()) - 0.5f) / float(h);
}

// ---- GPU objects ----

struct Block {                 // a piece of an upload page, or a buffer of its own (cls < 0)
    gpu_buffer buffer{};
    size_t offset = 0;
    int cls = -1;
    uint8_t* cpu = nullptr;    // at offset
    gpu_addr gpu = 0;          // at offset
};

struct Geometry {
    Block block;
    gpu_addr vb = 0, ib = 0;
    uint32_t vb_size = 0, ib_size = 0;
    uint32_t count = 0;
    Rml::Vector2f lo, hi;  // its vertices' box, and the first one's alpha (a decorator's: the element's opacity)
    float alpha = 1.f;
};

struct Texture {
    gpu_texture tex{};
    uint32_t srv = kNone;
    gpu_table table{};     // its entry, for bind_table
    gpu_state state = GPU_STATE_COPY_DEST;
};

// A model's GPU side: its textures, and one block with every mesh's indices and then its vertices as they rest (an
// animated model's vertices are posed into the frame's ring instead).
struct GpuModel {
    const ui::models::Model* model = nullptr;  // null: not there (said once)
    std::vector<Texture*> textures;            // as models::textures (null where one could not be made)
    Block block;
    struct Part {
        size_t indices = 0, vertices = 0;  // where, in the block
        uint32_t index_count = 0, vertex_count = 0;
    };
    std::vector<Part> parts;
};

struct Target {
    gpu_texture tex{};
    gpu_view rtv{};
    uint32_t srv = kNone;  // (no texture entry when multisampled)
    gpu_table table{};
    gpu_state state = GPU_STATE_RENDER_TARGET;
    int w = 0, h = 0;
    uint32_t samples = 1;
    gpu_format format = kColorFormat;
    bool layer = false;    // drawn with the stencil buffer
};

// A back buffer met before: a texture of the caller's taken in (gpu_d3d12_texture_on) or named by BackBuffers, and
// its colour target view.
struct Output {
    gpu_texture tex{};
    gpu_view rtv{};
    int w = 0, h = 0;
    gpu_format format = GPU_FORMAT_UNKNOWN;
    bool owned = false;    // taken in here: let go of in release_back_buffers
};

// The table pool's entries, handed out and given back (render_d3d12's Descriptors).
class Slots {
public:
    explicit Slots(uint32_t capacity) : mCapacity(capacity) {}
    uint32_t alloc() {
        if (!mFree.empty()) {
            const uint32_t i = mFree.back();
            mFree.pop_back();
            return i;
        }
        return mNext < mCapacity ? mNext++ : kNone;
    }
    void free(uint32_t i) {
        if (i != kNone) mFree.push_back(i);
    }

private:
    uint32_t mCapacity = 0, mNext = 0;
    std::vector<uint32_t> mFree;
};

// A view's description: a whole 2D texture (its own format, every mip), or a target of it.
gpu_view_desc view_of(gpu_texture t, uint8_t kind, uint32_t samples) {
    gpu_view_desc v;
    std::memset(&v, 0, sizeof v);
    v.texture = t;
    v.kind = kind;
    v.dim = kind != GPU_VIEW_SRV && samples > 1 ? GPU_DIM_2DMS : GPU_DIM_2D;
    return v;
}

class RendererImpl final : public Renderer, public Rml::RenderInterface {
public:
    RendererImpl(gpu_device* device, bool own, unsigned first_slot, BackBuffers back_buffers, ImageSource images)
        : mDev(device), mOwn(own), mFirstSlot(first_slot), mBackBuffers(std::move(back_buffers)),
          mImages(std::move(images)) {}
    ~RendererImpl() override;
    bool init();

    // ---- Renderer ----
    Rml::RenderInterface* rml() override { return this; }
    bool begin_frame(void* back_buffer, unsigned index, int width, int height) override;
    void end_frame() override;
    bool read_back(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba) override;
    void release_back_buffers() override;
    void set_hdr(float scale, bool srgb) override {
        mHdrScale = scale;
        mHdrSrgb = srgb;
    }
    bool draw_model(const char* name, const ModelView& view) override;

    // ---- Rml::RenderInterface ----
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
    void SetTransform(const Rml::Matrix4f* transform) override;
    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

private:
    struct Frame {
        gpu_buffer ring{};
        uint8_t* cpu = nullptr;
        gpu_addr gpu = 0;
        size_t used = 0;
        gpu_fence fence = 0;  // its last submission
    };
    struct Retired {
        gpu_fence fence;
        std::function<void()> fn;
    };

    // Resources
    gpu_buffer make_buffer(uint32_t memory, size_t size, gpu_state state, uint32_t use);
    gpu_texture_desc texture_desc(int w, int h, uint32_t levels, gpu_format format, uint32_t samples, uint32_t use,
                                  gpu_state state) const;
    gpu_texture make_texture(const gpu_texture_desc& desc);
    Block alloc_block(size_t size);
    void free_block(const Block& b);
    std::pair<uint8_t*, gpu_addr> ring(size_t size, size_t align);
    gpu_addr constants(const void* data, size_t size);
    Texture* make_image(const uint8_t* rgba, int w, int h);
    std::unique_ptr<Target> make_target(uint32_t samples, bool layer);
    void drop(std::unique_ptr<Target>& t);
    void resize(int width, int height);
    Target* layer(int i) { return i >= 0 && i < mLayerCount && i < int(mLayers.size()) ? mLayers[i].get() : nullptr; }
    Target* post(int i);
    bool back_buffer(void* res, unsigned index, Target& out);
    unsigned char hdr_to_byte(const uint8_t* half, bool alpha) const;
    gpu_pipeline pipeline(Program p, Blend b, Stencil s, const Target& t);

    // Recording and the GPU's progress
    bool open();
    void submit();
    void finish();
    void wait(gpu_fence value);
    void reclaim(bool all);
    // Freed once the GPU is past the next submission (render_d3d12's mFenceNext): it takes the fence then.
    void retire(std::function<void()> fn) { mPending.push_back(std::move(fn)); }

    // Drawing
    void transition(gpu_texture t, gpu_state& state, gpu_state to);
    void transition(Target& t, gpu_state to) { transition(t.tex, t.state, to); }
    void bind(Target& t);
    void sample(uint32_t slot, Target& t);
    void set_viewport(int x, int y, int w, int h);
    void set_scissor(Rml::Rectanglei r);
    Rml::Rectanglei region() const;
    bool prepare(Program p, Stencil stencil = kStencilAuto);
    bool set_common(Rml::Vector2f translation);
    void draw(const Geometry& g);
    void draw_quad(float u0, float v0, float u1, float v1);
    void blit(Target& from, Rml::Rectanglei src, Rml::Rectanglei dst);
    void resolve(Target& from, Target& to);
    void render_filters(Rml::Span<const Rml::CompiledFilterHandle> filters);
    void render_blur(float sigma, Target& source_destination, Target& temp, Rml::Rectanglei window);

    // The game's models
    Texture* make_block_texture(const ui::models::Texture& src);
    GpuModel* gpu_model(const std::string& name);
    bool render_model(const std::string& name, const ModelView& view, float opacity);

    gpu_device* const mDev;
    const bool mOwn;              // the device is this renderer's (create_d3d12): destroyed with it
    const unsigned mFirstSlot;    // the first of its kFrames recorder slots
    BackBuffers mBackBuffers;     // empty: back buffers are D3D12 resources, taken in (gpu_d3d12_texture_on)
    ImageSource mImages;

    gpu_fence mLastFence = 0;     // the last submission's
    Frame mFrame[kFrames];
    int mFrameIndex = 0;
    gpu_cmd* mCmd = nullptr;
    bool mOpen = false;
    std::deque<Retired> mRetired;
    std::vector<std::function<void()>> mPending;  // retired since the last submission

    gpu_layout mLayout{};
    gpu_shader mVs[3] = {};       // main, quad, model
    gpu_shader mPs[kProgramCount] = {};
    std::map<uint64_t, gpu_pipeline> mPipelines;
    gpu_table_pool mPool{};
    Slots mSrv{kSrvCapacity};
    uint32_t mNullSrv = kNone;
    gpu_table mNullTable{};

    std::vector<Block> mFreeBlocks[kMaxClass + 1];
    std::vector<gpu_buffer> mPages;
    uint8_t* mPageCpu = nullptr;
    gpu_addr mPageGpu = 0;
    size_t mPageUsed = kPageSize;

    uint32_t mSamples = 1;
    int mWidth = 0, mHeight = 0;
    void* mFrameBuffer = nullptr;  // the frame's back buffer and its index, from begin_frame to end_frame
    unsigned mFrameBufferIndex = 0;
    std::vector<std::unique_ptr<Target>> mLayers;
    int mLayerCount = 0;
    gpu_texture mStencil{};
    gpu_view mStencilDsv{};
    std::unique_ptr<Target> mPost[4];  // primary, secondary, tertiary, blend mask
    std::map<void*, Output> mOutputs;

    // Render state
    Rml::Matrix4f mProjection, mTransform;
    Rml::Rectanglei mScissor = Rml::Rectanglei::MakeInvalid();
    Blend mBlend = kPremultiplied;
    float mBlendFactor = 1.f;
    bool mClip = false;
    uint32_t mStencilRef = 0;
    // What the recording has
    Target* mBound = nullptr;
    bool mScissorDirty = true;
    gpu_pipeline mPso{};
    uint32_t mRefSet = kNone;
    float mFactorSet = -1.f;
    // HDR output (set_hdr)
    float mHdrScale = 1.f;
    bool mHdrSrgb = false;
    // The game's models, by name, and an animated one's vertices this frame
    std::map<std::string, GpuModel> mModels;
    std::vector<std::vector<ui::models::Vertex>> mPosed;
    const std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
};

// ---- Set-up ----

bool RendererImpl::init() {
    if (!mDev) return false;
    for (Frame& f : mFrame) {
        f.ring = make_buffer(GPU_MEM_UPLOAD, kRingSize, GPU_STATE_GENERIC_READ, kUploadUse);
        if (!GPU_VALID(f.ring) || !(f.cpu = static_cast<uint8_t*>(gpu_buffer_map(mDev, f.ring, 0, 0)))) return false;
        f.gpu = gpu_buffer_addr(mDev, f.ring);
    }
    // One table of one entry a texture or target (Vulkan makes them all up front).
    gpu_table_pool_desc pd;
    std::memset(&pd, 0, sizeof pd);
    pd.kind = GPU_TABLE_TEX;
    pd.count = kSrvCapacity;
    pd.nregions = 1;
    pd.region[0] = {0, kSrvCapacity, 1};
    mPool = gpu_table_pool_create(mDev, &pd);
    if (!GPU_VALID(mPool)) return false;
    mNullSrv = mSrv.alloc();
    gpu_table_write_tex(mDev, mPool, mNullSrv, nullptr);  // (an empty RGBA8 entry, as render_d3d12's null view)
    mNullTable = gpu_table_ref(mDev, mPool, mNullSrv);

    // b0 (everything), b1 (the filters), t0, t1; s0 wraps (images), s1 clamps (the filters' targets).
    gpu_layout_desc ld;
    std::memset(&ld, 0, sizeof ld);
    ld.flags = GPU_LAYOUT_VERTEX_INPUT;
    ld.nslots = 4;
    ld.slot[0] = {GPU_SLOT_CB, 0, 0, GPU_VIS_ALL};
    ld.slot[1] = {GPU_SLOT_CB, 1, 0, GPU_VIS_PIXEL};
    ld.slot[2] = {GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL};
    ld.slot[3] = {GPU_SLOT_TABLE_TEX, 1, 1, GPU_VIS_PIXEL};
    ld.nstatic = 2;
    for (uint32_t i = 0; i < 2; i++) {
        gpu_static_sampler& s = ld.statics[i];
        s.desc.filter = GPU_FILTER(GPU_FILTER_LINEAR, GPU_FILTER_LINEAR, GPU_FILTER_LINEAR, GPU_REDUCE_STANDARD);
        s.desc.address_u = s.desc.address_v = s.desc.address_w = i == 0 ? GPU_ADDRESS_WRAP : GPU_ADDRESS_CLAMP;
        s.desc.max_aniso = 1;
        s.desc.compare = GPU_CMP_NEVER;
        s.desc.max_lod = 1000.0f;
        s.reg = i;
        s.visibility = GPU_VIS_PIXEL;
    }
    mLayout = gpu_layout_create(mDev, &ld);
    if (!GPU_VALID(mLayout)) {
        std::fprintf(stderr, "[ui] the menu's layout failed\n");
        return false;
    }

    // The compile service's target for this device: dxbc is today's FXC call (D3DCompile, flags 0, no cache), so the
    // bytecode is render_d3d12's; Vulkan's spv puts the registers where this layout has them.
    const bool spv = mDev->caps.api == GPU_API_VULKAN;
    const auto compile = [&](const char* source, gpu_sc_stage stage, uint32_t kind, const char* name) -> gpu_shader {
        gpu_sc_blob blob{};
        char log[2048];
        const int r = gpu_sc_compile_layout(spv ? GPU_SC_SPV : GPU_SC_DXBC, stage, source, std::strlen(source), name,
                                            nullptr, &ld, GPU_SC_NO_CACHE, &blob, log, sizeof log);
        if (r != GPU_SC_OK) {
            std::fprintf(stderr, "[ui] the menu's %s shader did not compile (%d)%s%s\n", name, r, *log ? ": " : "", log);
            return {};
        }
        gpu_shader_desc sd;
        std::memset(&sd, 0, sizeof sd);
        sd.stage = kind;
        sd.code = spv ? GPU_CODE_SPIRV : GPU_CODE_DXBC;
        sd.bytes = blob.data;
        sd.size = blob.size;
        const gpu_shader s = gpu_shader_create(mDev, &sd);
        gpu_sc_blob_release(&blob);
        return s;
    };
    mVs[0] = compile(kVsMain, GPU_SC_VS, GPU_STAGE_VS, "main");
    mVs[1] = compile(kVsQuad, GPU_SC_VS, GPU_STAGE_VS, "quad");
    mVs[2] = compile(kVsModel, GPU_SC_VS, GPU_STAGE_VS, "model");
    for (int p = 0; p < kProgramCount; p++) mPs[p] = compile(kProgramPs[p], GPU_SC_PS, GPU_STAGE_PS, kProgramName[p]);
    // What the menu cannot do without; the effects only fall away if theirs are missing.
    if (!GPU_VALID(mVs[0]) || !GPU_VALID(mVs[1]) || !GPU_VALID(mPs[kColor]) || !GPU_VALID(mPs[kTexture]) ||
        !GPU_VALID(mPs[kPassthrough]))
        return false;

    // RmlUi's two samples to a pixel (smooth edges for rounded and turned shapes), if the device has them.
    const bool msaa = (gpu_format_support(mDev, kColorFormat, kLayerSamples) & GPU_FMT_MSAA) &&
                      (gpu_format_support(mDev, kStencilFormat, kLayerSamples) & GPU_FMT_MSAA);
    mSamples = msaa ? kLayerSamples : 1;
    return true;
}

RendererImpl::~RendererImpl() {
    if (!mDev) return;
    finish();
    reclaim(true);
    for (auto& [name, g] : mModels) {
        for (Texture* t : g.textures) {
            if (!t) continue;
            gpu_texture_destroy(mDev, t->tex, 0);
            mSrv.free(t->srv);
            delete t;
        }
        free_block(g.block);
    }
    for (auto& t : mLayers) drop(t);
    for (auto& t : mPost) drop(t);
    if (GPU_VALID(mStencil)) gpu_texture_destroy(mDev, mStencil, 0);
    reclaim(true);
    for (const auto& [res, o] : mOutputs)
        if (o.owned) gpu_texture_destroy(mDev, o.tex, 0);
    for (const gpu_buffer p : mPages) gpu_buffer_destroy(mDev, p, 0);
    // (Pipelines, the layout and the table pool live as long as the device: gpu.h.)
    for (const gpu_shader s : mVs)
        if (GPU_VALID(s)) gpu_shader_destroy(mDev, s);
    for (const gpu_shader s : mPs)
        if (GPU_VALID(s)) gpu_shader_destroy(mDev, s);
    for (Frame& f : mFrame)
        if (GPU_VALID(f.ring)) gpu_buffer_destroy(mDev, f.ring, 0);
    if (mOwn) gpu_destroy(mDev);
}

gpu_buffer RendererImpl::make_buffer(uint32_t memory, size_t size, gpu_state state, uint32_t use) {
    gpu_buffer_desc d;
    std::memset(&d, 0, sizeof d);
    d.size = size;
    d.use = use;
    d.memory = memory;
    d.state = state;
    return gpu_buffer_create(mDev, &d);
}

gpu_texture_desc RendererImpl::texture_desc(int w, int h, uint32_t levels, gpu_format format, uint32_t samples,
                                            uint32_t use, gpu_state state) const {
    gpu_texture_desc d;
    std::memset(&d, 0, sizeof d);
    d.width = uint32_t(w);
    d.height = uint32_t(h);
    d.layers = 1;
    d.mips = uint16_t(levels);
    d.format = format;
    d.samples = samples;
    d.use = use;
    d.state = state;
    return d;
}

gpu_texture RendererImpl::make_texture(const gpu_texture_desc& desc) {
    const gpu_texture t = gpu_texture_create(mDev, &desc);
    if (!GPU_VALID(t)) std::fprintf(stderr, "[ui] could not make a %ux%u texture for the menu\n", desc.width, desc.height);
    return t;
}

// Upload memory in blocks of powers of two, carved from 4 MB pages and reused once the GPU is done with them. A block
// starts at a multiple of its size (up to 64 KB), so blocks of 512 bytes and up can hold a texture's upload.
Block RendererImpl::alloc_block(size_t size) {
    int cls = kMinClass;
    while (cls <= kMaxClass && (size_t(1) << cls) < size) cls++;
    Block b;
    if (cls > kMaxClass) {
        b.buffer = make_buffer(GPU_MEM_UPLOAD, size, GPU_STATE_GENERIC_READ, kUploadUse);
        void* p = GPU_VALID(b.buffer) ? gpu_buffer_map(mDev, b.buffer, 0, 0) : nullptr;
        if (!p) {
            if (GPU_VALID(b.buffer)) gpu_buffer_destroy(mDev, b.buffer, 0);
            return {};
        }
        b.cpu = static_cast<uint8_t*>(p);
        b.gpu = gpu_buffer_addr(mDev, b.buffer);
        return b;
    }
    if (!mFreeBlocks[cls].empty()) {
        b = mFreeBlocks[cls].back();
        mFreeBlocks[cls].pop_back();
        return b;
    }
    const size_t bytes = size_t(1) << cls, align = std::min<size_t>(bytes, 65536);
    size_t at = (mPageUsed + align - 1) & ~(align - 1);
    if (mPages.empty() || at + bytes > kPageSize) {
        const gpu_buffer page = make_buffer(GPU_MEM_UPLOAD, kPageSize, GPU_STATE_GENERIC_READ, kUploadUse);
        void* p = GPU_VALID(page) ? gpu_buffer_map(mDev, page, 0, 0) : nullptr;
        if (!p) {
            if (GPU_VALID(page)) gpu_buffer_destroy(mDev, page, 0);
            return {};
        }
        mPages.push_back(page);
        mPageCpu = static_cast<uint8_t*>(p);
        mPageGpu = gpu_buffer_addr(mDev, page);
        at = 0;
    }
    b.buffer = mPages.back();
    b.offset = at;
    b.cls = cls;
    b.cpu = mPageCpu + at;
    b.gpu = mPageGpu + at;
    mPageUsed = at + bytes;
    return b;
}

void RendererImpl::free_block(const Block& b) {
    if (!GPU_VALID(b.buffer)) return;
    if (b.cls < 0) gpu_buffer_destroy(mDev, b.buffer, 0);
    else mFreeBlocks[b.cls].push_back(b);
}

// Space in this frame's ring; past its end, a block that goes with the frame.
std::pair<uint8_t*, gpu_addr> RendererImpl::ring(size_t size, size_t align) {
    Frame& f = mFrame[mFrameIndex];
    const size_t at = (f.used + align - 1) & ~(align - 1);
    if (at + size <= kRingSize) {
        f.used = at + size;
        return {f.cpu + at, f.gpu + at};
    }
    const Block b = alloc_block(std::max<size_t>(size, 256));  // (blocks of 256 bytes and up are 256-aligned)
    if (!GPU_VALID(b.buffer)) return {nullptr, 0};
    retire([this, b] { free_block(b); });
    return {b.cpu, b.gpu};
}

// (256: render_d3d12's, D3D12's constant buffer alignment; a device asking more gets its own.)
gpu_addr RendererImpl::constants(const void* data, size_t size) {
    const auto [cpu, gpu] = ring(size, std::max<size_t>(256, mDev->caps.cb_align));
    if (cpu) std::memcpy(cpu, data, size);
    return cpu ? gpu : 0;
}

std::unique_ptr<Target> RendererImpl::make_target(uint32_t samples, bool layer) {
    auto t = std::make_unique<Target>();
    t->w = mWidth;
    t->h = mHeight;
    t->samples = samples;
    t->layer = layer;
    gpu_texture_desc d = texture_desc(mWidth, mHeight, 1, kColorFormat, samples,
                                      GPU_USE_RT | GPU_USE_COPY_SRC | GPU_USE_COPY_DST |
                                          (samples == 1 ? GPU_USE_SAMPLED : 0u),
                                      GPU_STATE_RENDER_TARGET);
    d.clear_format = kColorFormat;
    t->tex = make_texture(d);
    if (GPU_VALID(t->tex)) {
        const gpu_view_desc v = view_of(t->tex, GPU_VIEW_RTV, samples);
        t->rtv = gpu_view_get(mDev, &v);
    }
    if (samples == 1) t->srv = mSrv.alloc();
    if (!GPU_VALID(t->tex) || !GPU_VALID(t->rtv) || (samples == 1 && t->srv == kNone)) {
        drop(t);
        return nullptr;
    }
    if (samples == 1) {
        const gpu_view_desc v = view_of(t->tex, GPU_VIEW_SRV, 1);
        gpu_table_write_tex(mDev, mPool, t->srv, &v);
        t->table = gpu_table_ref(mDev, mPool, t->srv);
    }
    return t;
}

void RendererImpl::drop(std::unique_ptr<Target>& t) {
    if (!t) return;
    const gpu_texture tex = t->tex;
    const uint32_t srv = t->srv;
    retire([this, tex, srv] {
        if (GPU_VALID(tex)) gpu_texture_destroy(mDev, tex, 0);  // (its views go with it)
        mSrv.free(srv);
    });
    t.reset();
}

// A new size: the targets and the stencil buffer are made again at it, as they are needed.
void RendererImpl::resize(int width, int height) {
    for (auto& t : mLayers) drop(t);
    mLayers.clear();
    for (auto& t : mPost) drop(t);
    if (GPU_VALID(mStencil)) {
        const gpu_texture s = mStencil;
        retire([this, s] { gpu_texture_destroy(mDev, s, 0); });
    }
    mStencil = {};
    mStencilDsv = {};
    mWidth = width;
    mHeight = height;
}

Target* RendererImpl::post(int i) {
    if (!mPost[i]) mPost[i] = make_target(1, false);
    return mPost[i].get();
}

// The back buffer as a target: named by mBackBuffers, or a D3D12 resource of the caller's taken in once for each
// buffer, with its view.
bool RendererImpl::back_buffer(void* res, unsigned index, Target& out) {
    Output o;
    if (mBackBuffers) {
        const BackBuffer b = mBackBuffers(res, index);
        if (!b.texture) return false;
        o.tex.id = b.texture;
        o.w = b.width;
        o.h = b.height;
        o.format = b.format;
        const gpu_view_desc v = view_of(o.tex, GPU_VIEW_RTV, 1);
        o.rtv = gpu_view_get(mDev, &v);  // (made once and kept by the device)
        if (!GPU_VALID(o.rtv)) return false;
    } else {
        auto it = mOutputs.find(res);
        if (it == mOutputs.end()) {
            if (mOutputs.size() >= 8) {  // (buffers changed without release_back_buffers: let go once the GPU is past)
                for (auto& [r, old] : mOutputs) {
                    const gpu_texture t = old.tex;
                    retire([this, t] { gpu_texture_destroy(mDev, t, 0); });
                }
                mOutputs.clear();
            }
#ifdef _WIN32
            gpu_texture_desc d;
            std::memset(&d, 0, sizeof d);
            if (res && mDev->caps.api == GPU_API_D3D12) o.tex = gpu_d3d12_texture_on(mDev, res, &d);
            o.w = int(d.width);
            o.h = int(d.height);
            o.format = d.format;
#endif
            if (!GPU_VALID(o.tex)) return false;
            o.owned = true;
            const gpu_view_desc v = view_of(o.tex, GPU_VIEW_RTV, 1);
            o.rtv = gpu_view_get(mDev, &v);
            if (!GPU_VALID(o.rtv)) {
                gpu_texture_destroy(mDev, o.tex, 0);  // (never recorded)
                return false;
            }
            it = mOutputs.emplace(res, o).first;
        }
        o = it->second;
    }
    out.tex = o.tex;
    out.rtv = o.rtv;
    out.srv = kNone;
    out.state = GPU_STATE_PRESENT;
    out.w = o.w;
    out.h = o.h;
    out.samples = 1;
    out.format = o.format;
    out.layer = false;
    return true;
}

gpu_pipeline RendererImpl::pipeline(Program p, Blend b, Stencil s, const Target& t) {
    const bool model = p == kModel || p == kModelBlend;
    if (!GPU_VALID(mPs[p]) || (model && !GPU_VALID(mVs[2]))) return {};
    const uint64_t key = uint64_t(p) | uint64_t(b) << 4 | uint64_t(s) << 8 | uint64_t(t.format) << 12 |
                         uint64_t(t.samples) << 24 | uint64_t(t.layer) << 32;
    if (const auto it = mPipelines.find(key); it != mPipelines.end()) return it->second;
    static_assert(sizeof(Rml::Vertex) == 20, "the vertex layout below");
    static_assert(sizeof(ui::models::Vertex) == 32, "the model vertex layout below");
    const auto attr = [](gpu_vertex_attr& a, const char* semantic, gpu_format format, uint32_t offset) {
        std::snprintf(a.semantic, sizeof a.semantic, "%s", semantic);
        a.semantic_index = 0;
        a.format = format;
        a.offset = offset;
    };
    gpu_pipeline_desc d;
    std::memset(&d, 0, sizeof d);
    d.layout = mLayout;
    d.vs = mVs[model ? 2 : p < kPassthrough ? 0 : 1];
    d.ps = mPs[p];
    d.vertex.count = 3;
    if (model) {
        d.vertex.stride = sizeof(ui::models::Vertex);
        attr(d.vertex.attr[0], "POSITION", GPU_FORMAT_R32G32B32_FLOAT, 0);
        attr(d.vertex.attr[1], "NORMAL", GPU_FORMAT_R32G32B32_FLOAT, 12);
        attr(d.vertex.attr[2], "TEXCOORD", GPU_FORMAT_R32G32_FLOAT, 24);
    } else {
        d.vertex.stride = sizeof(Rml::Vertex);
        attr(d.vertex.attr[0], "POSITION", GPU_FORMAT_R32G32_FLOAT, 0);
        attr(d.vertex.attr[1], "COLOR", GPU_FORMAT_R8G8B8A8_UNORM, 8);
        attr(d.vertex.attr[2], "TEXCOORD", GPU_FORMAT_R32G32_FLOAT, 12);
    }
    gpu_blend_state& rt = d.blend;
    rt.op = rt.op_alpha = GPU_BLEND_OP_ADD;
    rt.src = rt.src_alpha = GPU_BLEND_ONE;
    rt.dst = rt.dst_alpha = GPU_BLEND_ZERO;
    rt.write_mask = b == kNoColor ? 0 : GPU_WRITE_ALL;
    if (b == kPremultiplied) {  // (RmlUi's colours and textures are premultiplied)
        rt.enable = 1;
        rt.dst = rt.dst_alpha = GPU_BLEND_INV_SRC_ALPHA;
    } else if (b == kFactor) {  // the opacity filter: the source times the blend factor
        rt.enable = 1;
        rt.src = rt.src_alpha = GPU_BLEND_BLEND_FACTOR;
    }
    d.sample_mask = 0xFFFFFFFFu;
    d.cull = GPU_CULL_NONE;
    d.depth_clip = 0;
    d.multisample = t.samples > 1;
    d.depth_test = model && t.layer;  // (the models only: the rest of the UI keeps to the stencil)
    d.depth_write = p == kModel;
    d.depth_func = model ? GPU_CMP_LESS_EQUAL : GPU_CMP_ALWAYS;
    if (t.layer && s != kStencilOff) {
        d.stencil_test = 1;
        d.stencil_read_mask = 0xFF;
        d.stencil_write_mask = s == kStencilTest ? 0 : 0xFF;
        gpu_stencil_face op{GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                            uint8_t(s == kStencilTest ? GPU_CMP_EQUAL : GPU_CMP_ALWAYS)};
        if (s == kStencilSet) op.pass = GPU_STENCIL_REPLACE;
        if (s == kStencilIntersect) op.pass = GPU_STENCIL_INCR_SAT;
        d.stencil_front = d.stencil_back = op;
    }
    d.topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    d.nrt = 1;
    d.rt_format[0] = t.format;
    d.ds_format = t.layer ? kStencilFormat : GPU_FORMAT_UNKNOWN;
    d.samples = t.samples;
    const gpu_pipeline pso = gpu_pipeline_create(mDev, &d, 0, nullptr);
    if (!pso.v) std::fprintf(stderr, "[ui] the menu's %s pipeline failed\n", kProgramName[p]);
    mPipelines[key] = pso;
    return pso;
}

// ---- Recording ----

bool RendererImpl::open() {
    if (mOpen) return true;
    const int next = (mFrameIndex + 1) % kFrames;
    Frame& f = mFrame[next];
    wait(f.fence);
    reclaim(false);
    gpu_cmd* const c = gpu_cmd_begin(mDev, mFirstSlot + unsigned(next));
    if (!c) {
        std::fprintf(stderr, "[ui] could not start the menu's commands\n");
        return false;
    }
    mCmd = c;
    mFrameIndex = next;
    f.used = 0;
    mOpen = true;
    gpu_cmd_set_table_pools(c, mPool, gpu_table_pool{});
    gpu_cmd_set_layout(c, mLayout);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_bind_cb(c, 0, f.gpu);
    gpu_cmd_bind_cb(c, 1, f.gpu);
    gpu_cmd_bind_table(c, 2, mNullTable);
    gpu_cmd_bind_table(c, 3, mNullTable);
    mBound = nullptr;
    mScissorDirty = true;
    mPso = {};
    mRefSet = kNone;
    mFactorSet = -1.f;
    return true;
}

void RendererImpl::submit() {
    if (!mOpen) return;
    mOpen = false;
    mBound = nullptr;
    gpu_cmd_end(mDev, mCmd);
    gpu_submit_desc how;
    std::memset(&how, 0, sizeof how);
    how.flags = GPU_SUBMIT_SIGNAL;
    const gpu_fence f = gpu_submit(mDev, &mCmd, 1, &how);
    mCmd = nullptr;
    mFrame[mFrameIndex].fence = f;
    mLastFence = f;
    for (auto& fn : mPending) mRetired.push_back({f, std::move(fn)});
    mPending.clear();
}

void RendererImpl::finish() {
    submit();
    wait(mLastFence);
    reclaim(false);
}

// Bounded waits (a lost device's fence reads past every value), until the GPU is there or the device is lost.
void RendererImpl::wait(gpu_fence value) {
    if (!value || gpu_fence_completed(mDev) >= value) return;
    while (gpu_fence_wait(mDev, value, 2000))
        if (gpu_lost(mDev)) break;
}

void RendererImpl::reclaim(bool all) {
    const uint64_t done = all ? UINT64_MAX : gpu_fence_completed(mDev);
    while (!mRetired.empty() && mRetired.front().fence <= done) {
        std::function<void()> fn = std::move(mRetired.front().fn);
        mRetired.pop_front();
        fn();
    }
    if (all) {
        std::vector<std::function<void()>> pending;
        pending.swap(mPending);
        for (auto& fn : pending) fn();
    }
}

// ---- Drawing ----

void RendererImpl::transition(gpu_texture t, gpu_state& state, gpu_state to) {
    if (state == to) return;
    gpu_cmd_barrier(mCmd, t, state, to);  // (every subresource)
    state = to;
}

void RendererImpl::bind(Target& t) {
    transition(t, GPU_STATE_RENDER_TARGET);
    gpu_pass pass;
    std::memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = t.rtv;
    if (t.layer) pass.ds = mStencilDsv;
    gpu_cmd_pass_begin(mCmd, &pass);
    mBound = &t;
    set_viewport(0, 0, t.w, t.h);
    mScissorDirty = true;
}

void RendererImpl::sample(uint32_t slot, Target& t) {
    transition(t, GPU_STATE_PIXEL_SHADER_RESOURCE);
    gpu_cmd_bind_table(mCmd, slot, t.table);
}

void RendererImpl::set_viewport(int x, int y, int w, int h) {
    const gpu_viewport v{float(x), float(y), float(std::max(w, 1)), float(std::max(h, 1)), 0.f, 1.f};
    gpu_cmd_set_viewport(mCmd, &v);
}

void RendererImpl::set_scissor(Rml::Rectanglei r) {
    if (r == mScissor) return;
    mScissor = r;
    mScissorDirty = true;
}

// The scissor region inside the targets (all of them if there is none).
Rml::Rectanglei RendererImpl::region() const {
    if (!mScissor.Valid()) return Rml::Rectanglei::FromSize({mWidth, mHeight});
    return Rml::Rectanglei::FromCorners(
        {Rml::Math::Clamp(mScissor.Left(), 0, mWidth), Rml::Math::Clamp(mScissor.Top(), 0, mHeight)},
        {Rml::Math::Clamp(mScissor.Right(), 0, mWidth), Rml::Math::Clamp(mScissor.Bottom(), 0, mHeight)});
}

// The pipeline and state for a draw into the bound target. False: there is nothing to draw with.
bool RendererImpl::prepare(Program p, Stencil stencil) {
    if (!mOpen || !mBound) return false;
    transition(*mBound, GPU_STATE_RENDER_TARGET);  // (a resolve or a copy may have taken it)
    const Stencil s = !mBound->layer ? kStencilOff : stencil != kStencilAuto ? stencil : mClip ? kStencilTest : kStencilOff;
    const gpu_pipeline pso = pipeline(p, mBlend, s, *mBound);
    if (!pso.v) return false;
    if (pso.v != mPso.v) {
        gpu_cmd_set_pipeline(mCmd, pso);
        mPso = pso;
    }
    if (mBlend == kFactor && mFactorSet != mBlendFactor) {
        const float f[4] = {mBlendFactor, mBlendFactor, mBlendFactor, mBlendFactor};
        gpu_cmd_set_blend_factor(mCmd, f);
        mFactorSet = mBlendFactor;
    }
    const uint32_t ref = s == kStencilTest ? mStencilRef : 1;
    if (s != kStencilOff && ref != mRefSet) {
        gpu_cmd_set_stencil_ref(mCmd, ref);
        mRefSet = ref;
    }
    if (mScissorDirty) {
        gpu_rect r{0, 0, mBound->w, mBound->h};
        if (mScissor.Valid()) {
            r.left = Rml::Math::Clamp(mScissor.Left(), 0, mBound->w);
            r.top = Rml::Math::Clamp(mScissor.Top(), 0, mBound->h);
            r.right = Rml::Math::Clamp(mScissor.Right(), 0, mBound->w);
            r.bottom = Rml::Math::Clamp(mScissor.Bottom(), 0, mBound->h);
        }
        gpu_cmd_set_scissor(mCmd, &r);
        mScissorDirty = false;
    }
    return true;
}

bool RendererImpl::set_common(Rml::Vector2f translation) {
    CommonConstants c{};
    store(mTransform, c.transform);
    c.translate[0] = translation.x;
    c.translate[1] = translation.y;
    const gpu_addr gpu = constants(&c, sizeof c);
    if (gpu) gpu_cmd_bind_cb(mCmd, 0, gpu);
    return gpu != 0;
}

void RendererImpl::draw(const Geometry& g) {
    gpu_cmd_set_vb(mCmd, 0, g.vb, g.vb_size, uint32_t(sizeof(Rml::Vertex)));
    gpu_cmd_set_ib(mCmd, g.ib, g.ib_size, GPU_FORMAT_R32_UINT);
    gpu_cmd_draw_indexed(mCmd, g.count, 1, 0, 0, 0);
}

// A quad over the whole viewport, with these texture coordinates at its top left and bottom right.
void RendererImpl::draw_quad(float u0, float v0, float u1, float v1) {
    struct V {
        float x, y;
        uint32_t color;
        float u, v;
    };
    const V vertices[4] = {{-1, 1, ~0u, u0, v0}, {1, 1, ~0u, u1, v0}, {1, -1, ~0u, u1, v1}, {-1, -1, ~0u, u0, v1}};
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    const auto [cpu, gpu] = ring(sizeof vertices + sizeof indices, 16);
    if (!cpu) return;
    std::memcpy(cpu, vertices, sizeof vertices);
    std::memcpy(cpu + sizeof vertices, indices, sizeof indices);
    gpu_cmd_set_vb(mCmd, 0, gpu, uint32_t(sizeof vertices), uint32_t(sizeof(V)));
    gpu_cmd_set_ib(mCmd, gpu + sizeof vertices, uint32_t(sizeof indices), GPU_FORMAT_R32_UINT);
    gpu_cmd_draw_indexed(mCmd, 6, 1, 0, 0, 0);
}

// The src rectangle of `from` scaled into the dst rectangle of the bound target (bilinear), through the scissor.
void RendererImpl::blit(Target& from, Rml::Rectanglei src, Rml::Rectanglei dst) {
    if (dst.Width() <= 0 || dst.Height() <= 0 || src.Width() <= 0 || src.Height() <= 0) return;
    sample(2, from);
    if (!prepare(kPassthrough)) return;
    set_viewport(dst.Left(), dst.Top(), dst.Width(), dst.Height());
    draw_quad(float(src.Left()) / float(from.w), float(src.Top()) / float(from.h), float(src.Right()) / float(from.w),
              float(src.Bottom()) / float(from.h));
    set_viewport(0, 0, mBound->w, mBound->h);
}

void RendererImpl::resolve(Target& from, Target& to) {
    if (from.samples > 1) {
        transition(from, GPU_STATE_RESOLVE_SOURCE);
        transition(to, GPU_STATE_RESOLVE_DEST);
        gpu_cmd_resolve(mCmd, to.tex, 0, from.tex, 0, kColorFormat);
    } else {
        transition(from, GPU_STATE_COPY_SOURCE);
        transition(to, GPU_STATE_COPY_DEST);
        gpu_cmd_copy_texture_whole(mCmd, to.tex, from.tex);
    }
}

// ---- Frames ----

// Always a frame: if the recording or the stencil buffer could not be made, the drawing calls and end_frame skip
// their work.
bool RendererImpl::begin_frame(void* back_buffer, unsigned index, int width, int height) {
    mFrameBuffer = back_buffer;
    mFrameBufferIndex = index;
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (!open()) return true;
    if (width != mWidth || height != mHeight) resize(width, height);
    mProjection = Rml::Matrix4f::ProjectOrtho(0, float(width), float(height), 0, -10000, 10000);
    mTransform = mProjection;
    mScissor = Rml::Rectanglei::MakeInvalid();
    mBlend = kPremultiplied;
    mClip = false;
    mStencilRef = 0;
    mLayerCount = 0;
    if (!GPU_VALID(mStencil)) {
        gpu_texture_desc d = texture_desc(width, height, 1, kStencilFormat, mSamples, GPU_USE_DS, GPU_STATE_DEPTH_WRITE);
        d.clear_format = kStencilFormat;
        d.clear_depth = 1.f;
        mStencil = make_texture(d);
        if (!GPU_VALID(mStencil)) return true;
        const gpu_view_desc v = view_of(mStencil, GPU_VIEW_DSV, mSamples);
        mStencilDsv = gpu_view_get(mDev, &v);
        if (!GPU_VALID(mStencilDsv)) {
            gpu_texture_destroy(mDev, mStencil, 0);  // (never recorded)
            mStencil = {};
            return true;
        }
    }
    gpu_cmd_clear_depth(mCmd, mStencilDsv, GPU_CLEAR_STENCIL, 1.f, 0, nullptr);
    PushLayer();
    return true;
}

void RendererImpl::end_frame() {
    void* const back_buffer = mFrameBuffer;
    mFrameBuffer = nullptr;
    if (!mOpen) return;
    Target* base = GPU_VALID(mStencil) ? layer(0) : nullptr;
    Target* primary = base ? post(0) : nullptr;
    Target out;
    if (base && primary && back_buffer && this->back_buffer(back_buffer, mFrameBufferIndex, out)) {
        resolve(*base, *primary);
        bind(out);
        mBlend = kPremultiplied;
        mScissor = Rml::Rectanglei::MakeInvalid();
        sample(2, *primary);
        if (out.format == GPU_FORMAT_R16G16B16A16_FLOAT) {  // HDR: scRGB, white at paper white (set_hdr)
            const float hdr[4] = {mHdrScale, mHdrSrgb ? 1.f : 0.f, 0.f, 0.f};
            if (prepare(kHdrOut))
                if (const auto gpu = constants(hdr, sizeof hdr)) {
                    gpu_cmd_bind_cb(mCmd, 1, gpu);
                    draw_quad(0, 0, 1, 1);
                }
        } else if (prepare(kPassthrough)) {
            draw_quad(0, 0, 1, 1);
        }
        transition(out, GPU_STATE_PRESENT);
    }
    mLayerCount = 0;
    submit();
}

// One channel of an FP16 scRGB back buffer as the 8-bit SDR value it stands for: divided by paper white and encoded
// the way set_hdr decodes. Alpha as it is.
unsigned char RendererImpl::hdr_to_byte(const uint8_t* half, bool alpha) const {
    uint16_t h;
    std::memcpy(&h, half, 2);
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v = e == 0 ? float(m) / 16777216.f : e == 31 ? 65504.f : std::ldexp(1.f + float(m) / 1024.f, int(e) - 15);
    if (h & 0x8000u) v = 0.f;
    if (!alpha) {
        v = std::clamp(v / std::max(mHdrScale, 1e-3f), 0.f, 1.f);
        v = mHdrSrgb ? (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f) : std::pow(v, 1.f / 2.2f);
    }
    return static_cast<unsigned char>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
}

bool RendererImpl::read_back(void* back_buffer, unsigned index, int width, int height, std::vector<unsigned char>& rgba) {
    Target src;
    if (!back_buffer || width <= 0 || height <= 0 || !open() || !this->back_buffer(back_buffer, index, src)) return false;
    width = std::min(width, src.w);
    height = std::min(height, src.h);
    const bool half = src.format == GPU_FORMAT_R16G16B16A16_FLOAT;  // HDR (set_hdr): read back as the SDR picture
    const uint32_t pitch = (uint32_t(width) * (half ? 8 : 4) + kPitchAlign - 1) & ~(kPitchAlign - 1);
    const size_t bytes = size_t(pitch) * size_t(height);
    const gpu_buffer buffer = make_buffer(GPU_MEM_READBACK, bytes, GPU_STATE_COPY_DEST, GPU_USE_COPY_DST);
    if (!GPU_VALID(buffer)) return false;
    gpu_state state = GPU_STATE_PRESENT;
    transition(src.tex, state, GPU_STATE_COPY_SOURCE);
    gpu_footprint to;
    std::memset(&to, 0, sizeof to);
    to.offset = 0;
    to.format = src.format;
    to.width = uint32_t(width);
    to.height = uint32_t(height);
    to.depth = 1;
    to.row_pitch = pitch;
    const gpu_box box{0, 0, 0, uint32_t(width), uint32_t(height), 1};
    gpu_cmd_copy_tex_to_buf(mCmd, buffer, &to, src.tex, 0, &box);
    transition(src.tex, state, GPU_STATE_PRESENT);
    finish();
    void* const p = gpu_buffer_map(mDev, buffer, 0, bytes);
    const bool ok = p != nullptr;
    if (ok) {
        const bool bgra = src.format == GPU_FORMAT_B8G8R8A8_UNORM || src.format == GPU_FORMAT_B8G8R8A8_UNORM_SRGB;
        const size_t row = size_t(width) * 4;
        rgba.resize(row * size_t(height));
        for (int y = 0; y < height; y++) {
            unsigned char* out = rgba.data() + row * size_t(y);
            const uint8_t* in = static_cast<const uint8_t*>(p) + size_t(pitch) * size_t(y);
            if (half) {
                for (int x = 0; x < width * 4; x++) out[x] = hdr_to_byte(in + size_t(x) * 2, x % 4 == 3);
                continue;
            }
            std::memcpy(out, in, row);
            if (bgra)
                for (size_t x = 0; x < row; x += 4) std::swap(out[x], out[x + 2]);
        }
        gpu_buffer_unmap(mDev, buffer, 0, 0);
    }
    gpu_buffer_destroy(mDev, buffer, 0);  // (the GPU is past it: finish)
    return ok;
}

void RendererImpl::release_back_buffers() {
    finish();
    for (const auto& [res, o] : mOutputs)
        if (o.owned) gpu_texture_destroy(mDev, o.tex, 0);  // (the GPU is past them: finish)
    mOutputs.clear();
}

// ---- Geometry and textures ----

Rml::CompiledGeometryHandle RendererImpl::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    auto* g = new Geometry;
    const size_t vb = vertices.size() * sizeof(Rml::Vertex), ib = indices.size() * sizeof(int);
    if (vb && ib) {
        g->block = alloc_block(vb + ib);
        if (GPU_VALID(g->block.buffer)) {
            std::memcpy(g->block.cpu, vertices.data(), vb);
            std::memcpy(g->block.cpu + vb, indices.data(), ib);
            g->vb = g->block.gpu;
            g->vb_size = uint32_t(vb);
            g->ib = g->block.gpu + vb;
            g->ib_size = uint32_t(ib);
            g->count = uint32_t(indices.size());
        }
        g->lo = g->hi = vertices[0].position;
        for (const Rml::Vertex& v : vertices) {
            g->lo = Rml::Math::Min(g->lo, v.position);
            g->hi = Rml::Math::Max(g->hi, v.position);
        }
        g->alpha = float(vertices[0].colour.alpha) / 255.f;
    }
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RendererImpl::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
    const auto* g = reinterpret_cast<const Geometry*>(geometry);
    const auto* t = reinterpret_cast<const Texture*>(texture);
    if (!g || !g->count) return;
    if (t) {
        if (!mOpen) return;
        gpu_cmd_bind_table(mCmd, 2, t->table);
    }
    if (prepare(t ? kTexture : kColor) && set_common(translation)) draw(*g);
}

void RendererImpl::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    auto* g = reinterpret_cast<Geometry*>(geometry);
    if (!g) return;
    const Block b = g->block;
    delete g;
    if (GPU_VALID(b.buffer)) retire([this, b] { free_block(b); });
}

// A texture from RGBA (premultiplied) with its mip levels, or without pixels, for SaveLayerAsTexture (one level, in
// the copy destination state).
Texture* RendererImpl::make_image(const uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !open()) return nullptr;
    uint32_t levels = 1;
    if (rgba)
        while ((w >> levels) || (h >> levels)) levels++;
    const gpu_texture_desc desc = texture_desc(w, h, levels, kColorFormat, 1, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                               GPU_STATE_COPY_DEST);
    const gpu_texture tex = make_texture(desc);
    if (!GPU_VALID(tex)) return nullptr;
    auto* t = new Texture{tex, mSrv.alloc(), {}, GPU_STATE_COPY_DEST};
    if (t->srv == kNone) {
        gpu_texture_destroy(mDev, tex, 0);  // (never recorded)
        delete t;
        return nullptr;
    }
    gpu_view_desc sd = view_of(tex, GPU_VIEW_SRV, 1);
    sd.format = kColorFormat;
    sd.swizzle = GPU_SWIZZLE_IDENTITY;
    sd.mips = uint8_t(levels);
    gpu_table_write_tex(mDev, mPool, t->srv, &sd);
    t->table = gpu_table_ref(mDev, mPool, t->srv);
    if (!rgba) return t;

    std::vector<gpu_footprint> fp(levels);
    std::vector<uint32_t> rows(levels);
    std::vector<uint64_t> row_bytes(levels);
    const uint64_t total = gpu_texture_footprints(mDev, &desc, 0, levels, 0, fp.data(), rows.data(), row_bytes.data());
    const Block b = alloc_block(std::max<size_t>(size_t(total), kPlacementAlign));
    if (GPU_VALID(b.buffer)) {
        // Level 0 as given, each next level the 2x2 average of the one before (premultiplied, so a plain average).
        std::vector<uint8_t> level(rgba, rgba + size_t(w) * size_t(h) * 4), next;
        int lw = w, lh = h;
        for (uint32_t m = 0; m < levels; m++) {
            for (int y = 0; y < lh; y++)
                std::memcpy(b.cpu + fp[m].offset + size_t(fp[m].row_pitch) * size_t(y),
                            level.data() + size_t(lw) * 4 * size_t(y), size_t(lw) * 4);
            gpu_footprint from = fp[m];
            from.offset += b.offset;
            gpu_cmd_copy_buf_to_tex(mCmd, tex, m, 0, 0, 0, b.buffer, &from);
            if (m + 1 == levels) break;
            const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
            next.resize(size_t(nw) * size_t(nh) * 4);
            for (int y = 0; y < nh; y++) {
                const int y0 = std::min(2 * y, lh - 1), y1 = std::min(2 * y + 1, lh - 1);
                for (int x = 0; x < nw; x++) {
                    const int x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
                    for (int c = 0; c < 4; c++) {
                        const auto at = [&](int xx, int yy) { return unsigned(level[(size_t(yy) * size_t(lw) + size_t(xx)) * 4 + size_t(c)]); };
                        next[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] =
                            uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                    }
                }
            }
            level.swap(next);
            lw = nw;
            lh = nh;
        }
        retire([this, b] { free_block(b); });
    }
    transition(t->tex, t->state, GPU_STATE_PIXEL_SHADER_RESOURCE);
    return t;
}

Rml::TextureHandle RendererImpl::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    std::vector<unsigned char> file;
    if (!mImages || !mImages(source, file)) {
        Rml::String data;
        if (!Rml::GetFileInterface() || !Rml::GetFileInterface()->LoadFile(source, data) || data.empty()) return {};
        file.assign(data.begin(), data.end());
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!px) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Could not decode the image %s.", source.c_str());
        return {};
    }
    for (size_t i = 0, end = size_t(w) * size_t(h) * 4; i < end; i += 4) {  // (RmlUi's textures are premultiplied)
        const unsigned a = px[i + 3];
        for (int c = 0; c < 3; c++) px[i + c] = stbi_uc((unsigned(px[i + c]) * a + 127) / 255);
    }
    Texture* t = make_image(px, w, h);
    stbi_image_free(px);
    if (t) dimensions = {w, h};
    return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::TextureHandle RendererImpl::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (source.size() < size_t(dimensions.x) * size_t(dimensions.y) * 4) return {};
    return reinterpret_cast<Rml::TextureHandle>(make_image(source.data(), dimensions.x, dimensions.y));
}

void RendererImpl::ReleaseTexture(Rml::TextureHandle texture) {
    auto* t = reinterpret_cast<Texture*>(texture);
    if (!t) return;
    const gpu_texture tex = t->tex;
    const uint32_t srv = t->srv;
    delete t;
    retire([this, tex, srv] {
        gpu_texture_destroy(mDev, tex, 0);
        mSrv.free(srv);
    });
}

// ---- Scissor, transform, clip mask ----

void RendererImpl::EnableScissorRegion(bool enable) {
    if (!enable) set_scissor(Rml::Rectanglei::MakeInvalid());  // (an enable is followed by SetScissorRegion)
}

void RendererImpl::SetScissorRegion(Rml::Rectanglei region) {
    set_scissor(region);
}

void RendererImpl::SetTransform(const Rml::Matrix4f* transform) {
    mTransform = transform ? mProjection * *transform : mProjection;
}

void RendererImpl::EnableClipMask(bool enable) {
    mClip = enable;
}

// Set and SetInverse clear the stencil and mark the geometry 1, then test for 1 (inside) or 0 (outside); Intersect
// adds 1 under the geometry and tests for one more than before.
void RendererImpl::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                                    Rml::Vector2f translation) {
    const auto* g = reinterpret_cast<const Geometry*>(geometry);
    if (!mOpen || !mBound || !mBound->layer) return;
    Stencil s = kStencilSet;
    uint32_t ref = 1;
    switch (operation) {
    case Rml::ClipMaskOperation::Set: break;
    case Rml::ClipMaskOperation::SetInverse: ref = 0; break;
    case Rml::ClipMaskOperation::Intersect:
        s = kStencilIntersect;
        ref = mStencilRef + 1;
        break;
    }
    if (s == kStencilSet) gpu_cmd_clear_depth(mCmd, mStencilDsv, GPU_CLEAR_STENCIL, 1.f, 0, nullptr);
    if (g && g->count) {
        const Blend saved = mBlend;
        mBlend = kNoColor;
        if (prepare(kColor, s) && set_common(translation)) draw(*g);
        mBlend = saved;
    }
    mStencilRef = ref;
}

// ---- Layers ----

Rml::LayerHandle RendererImpl::PushLayer() {
    if (mLayerCount == int(mLayers.size())) mLayers.emplace_back();
    std::unique_ptr<Target>& t = mLayers[size_t(mLayerCount++)];
    if (!t) t = make_target(mSamples, true);
    if (!mOpen || !t || !GPU_VALID(mStencil)) {
        mBound = nullptr;  // (nothing draws until a layer that exists is bound again)
    } else {
        bind(*t);
        const float clear[4] = {0.f, 0.f, 0.f, 0.f};
        gpu_cmd_clear_color(mCmd, t->rtv, clear, nullptr);
    }
    return Rml::LayerHandle(mLayerCount - 1);
}

void RendererImpl::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                                   Rml::Span<const Rml::CompiledFilterHandle> filters) {
    Target* from = layer(int(source));
    Target* to = layer(int(destination));
    if (!mOpen || !from || !to || !post(0)) return;
    resolve(*from, *post(0));
    render_filters(filters);
    bind(*to);
    const Blend saved = mBlend;
    mBlend = blend_mode == Rml::BlendMode::Replace ? kReplace : kPremultiplied;
    sample(2, *post(0));
    if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
    mBlend = saved;
    if (int(destination) != mLayerCount - 1) {
        if (Target* top = layer(mLayerCount - 1)) bind(*top);
        else mBound = nullptr;
    }
}

void RendererImpl::PopLayer() {
    if (mLayerCount > 0) mLayerCount--;
    Target* top = layer(mLayerCount - 1);
    if (mOpen && top) bind(*top);
    else mBound = nullptr;
}

// The top layer's pixels under the scissor, as a texture with its top left at (0, 0).
Rml::TextureHandle RendererImpl::SaveLayerAsTexture() {
    Target* top = layer(mLayerCount - 1);
    const Rml::Rectanglei r = region();
    if (!mOpen || !top || !post(0) || r.Width() <= 0 || r.Height() <= 0) return {};
    Texture* t = make_image(nullptr, r.Width(), r.Height());
    if (!t) return {};
    Target& primary = *post(0);
    resolve(*top, primary);
    transition(primary, GPU_STATE_COPY_SOURCE);
    const gpu_box box{uint32_t(r.Left()), uint32_t(r.Top()), 0, uint32_t(r.Right()), uint32_t(r.Bottom()), 1};
    gpu_cmd_copy_texture(mCmd, t->tex, 0, 0, 0, 0, primary.tex, 0, &box);
    transition(t->tex, t->state, GPU_STATE_PIXEL_SHADER_RESOURCE);
    return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::CompiledFilterHandle RendererImpl::SaveLayerAsMaskImage() {
    Target* top = layer(mLayerCount - 1);
    if (!mOpen || !top || !post(0) || !post(3)) return {};
    resolve(*top, *post(0));
    bind(*post(3));
    const Blend saved = mBlend;
    mBlend = kReplace;
    sample(2, *post(0));
    if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
    mBlend = saved;
    bind(*top);
    auto* f = new CompiledFilter;
    f->type = FilterType::MaskImage;
    return reinterpret_cast<Rml::CompiledFilterHandle>(f);
}

// ---- Filters ----

Rml::CompiledFilterHandle RendererImpl::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) {
    CompiledFilter filter;
    if (name == "opacity") {
        filter.type = FilterType::Passthrough;
        filter.blend_factor = Rml::Get(parameters, "value", 1.0f);
    } else if (name == "blur") {
        filter.type = FilterType::Blur;
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
    } else if (name == "drop-shadow") {
        filter.type = FilterType::DropShadow;
        filter.sigma = Rml::Get(parameters, "sigma", 0.f);
        filter.color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
    } else if (name == "brightness") {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
    } else if (name == "contrast") {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
        filter.color_matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.f));
    } else if (name == "invert") {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
        const float inverted = 1.f - 2.f * value;
        filter.color_matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f);
        filter.color_matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.f));
    } else if (name == "grayscale") {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev = 1.f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        filter.color_matrix = Rml::Matrix4f::FromRows({gray.x + rev, gray.y, gray.z, 0.f}, {gray.x, gray.y + rev, gray.z, 0.f},
                                                      {gray.x, gray.y, gray.z + rev, 0.f}, {0.f, 0.f, 0.f, 1.f});
    } else if (name == "sepia") {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev = 1.f - value;
        const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        filter.color_matrix = Rml::Matrix4f::FromRows({r.x + rev, r.y, r.z, 0.f}, {g.x, g.y + rev, g.z, 0.f},
                                                      {b.x, b.y, b.z + rev, 0.f}, {0.f, 0.f, 0.f, 1.f});
    } else if (name == "hue-rotate") {
        // https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float s = Rml::Math::Sin(value), c = Rml::Math::Cos(value);
        filter.color_matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.f},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.f},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.f},
            {0.f, 0.f, 0.f, 1.f});
    } else if (name == "saturate") {
        filter.type = FilterType::ColorMatrix;
        const float v = Rml::Get(parameters, "value", 1.0f);
        filter.color_matrix = Rml::Matrix4f::FromRows({0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v, 0.f},
                                                      {0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v, 0.f},
                                                      {0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v, 0.f},
                                                      {0.f, 0.f, 0.f, 1.f});
    }
    if (filter.type != FilterType::Invalid) return reinterpret_cast<Rml::CompiledFilterHandle>(new CompiledFilter(filter));
    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
    return {};
}

void RendererImpl::ReleaseFilter(Rml::CompiledFilterHandle filter) {
    delete reinterpret_cast<CompiledFilter*>(filter);
}

// The filters, in order, on the primary postprocess target: each reads the primary and writes the secondary, and the
// two swap. Everything stays under the scissor, as the composite after them does.
void RendererImpl::render_filters(Rml::Span<const Rml::CompiledFilterHandle> filters) {
    for (const Rml::CompiledFilterHandle handle : filters) {
        const auto* filter = reinterpret_cast<const CompiledFilter*>(handle);
        Target* primary = post(0);
        Target* secondary = post(1);
        if (!filter || !primary || !secondary) continue;
        const Blend saved = mBlend;
        switch (filter->type) {
        case FilterType::Passthrough:  // opacity: the source times the factor
            bind(*secondary);
            mBlend = kFactor;
            mBlendFactor = filter->blend_factor;
            sample(2, *primary);
            if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
            std::swap(mPost[0], mPost[1]);
            break;
        case FilterType::Blur:
            mBlend = kReplace;
            render_blur(filter->sigma, *primary, *secondary, region());
            break;
        case FilterType::DropShadow: {
            // The shadow (the alpha in the shadow's colour, moved by the offset) into the secondary, blurred, then the
            // element over it.
            mBlend = kReplace;
            bind(*secondary);
            DropShadowConstants k{};
            const Rml::Colourf color = to_float(filter->color);
            for (int i = 0; i < 4; i++) k.color[i] = color[i];
            const Rml::Rectanglei window = region();
            region_limits(k.min, k.max, window, mWidth, mHeight);
            const float ox = filter->offset.x / float(mWidth), oy = filter->offset.y / float(mHeight);
            sample(2, *primary);
            if (prepare(kDropShadow)) {
                if (const auto gpu = constants(&k, sizeof k)) {
                    gpu_cmd_bind_cb(mCmd, 1, gpu);
                    draw_quad(-ox, -oy, 1.f - ox, 1.f - oy);
                }
            }
            if (filter->sigma >= 0.5f) {
                if (Target* tertiary = post(2)) render_blur(filter->sigma, *secondary, *tertiary, window);
            }
            bind(*secondary);
            mBlend = kPremultiplied;
            sample(2, *primary);
            if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
            std::swap(mPost[0], mPost[1]);
            break;
        }
        case FilterType::ColorMatrix: {
            mBlend = kReplace;
            bind(*secondary);
            float m[16];
            store(filter->color_matrix, m);
            sample(2, *primary);
            if (prepare(kColorMatrix)) {
                if (const auto gpu = constants(m, sizeof m)) {
                    gpu_cmd_bind_cb(mCmd, 1, gpu);
                    draw_quad(0, 0, 1, 1);
                }
            }
            std::swap(mPost[0], mPost[1]);
            break;
        }
        case FilterType::MaskImage: {
            Target* mask = post(3);
            if (!mask) break;
            mBlend = kReplace;
            bind(*secondary);
            sample(2, *primary);
            sample(3, *mask);
            if (prepare(kBlendMask)) draw_quad(0, 0, 1, 1);
            gpu_cmd_bind_table(mCmd, 3, mNullTable);
            std::swap(mPost[0], mPost[1]);
            break;
        }
        case FilterType::Invalid: break;
        }
        mBlend = saved;
    }
}

// RmlUi's blur: the window halved (bilinear) until a pass of sigma 3 or less does, a vertical and a horizontal pass,
// then scaled back into the window. The result is in source_destination; temp is used up.
void RendererImpl::render_blur(float sigma, Target& source_destination, Target& temp, Rml::Rectanglei window) {
    if (window.Width() <= 0 || window.Height() <= 0) return;
    int level = 0;
    sigma_to_parameters(sigma, level, sigma);
    if (sigma == 0) return;
    Target& sd = source_destination;
    const Rml::Rectanglei original = mScissor;
    const Blend saved = mBlend;
    mBlend = kReplace;

    // Downscale: each pass draws the whole of its source into the top left quarter, so after pass i the picture is
    // in the top left 1 / 2^i of the target. With an odd size the texture coordinates are scaled to land between
    // texels, a 50% blend of neighbours.
    Rml::Rectanglei scissor = window;
    const float su = sd.w % 2 ? 1.f - 1.f / float(sd.w) : 1.f, sv = sd.h % 2 ? 1.f - 1.f / float(sd.h) : 1.f;
    for (int i = 0; i < level; i++) {
        scissor.p0 = (scissor.p0 + Rml::Vector2i(1)) / 2;
        scissor.p1 = Rml::Math::Max(scissor.p1 / 2, scissor.p0);
        const bool from_source = i % 2 == 0;
        bind(from_source ? temp : sd);
        set_scissor(scissor);
        sample(2, from_source ? sd : temp);
        if (!prepare(kPassthrough)) break;
        set_viewport(0, 0, sd.w / 2, sd.h / 2);
        draw_quad(0, 0, su, sv);
    }
    set_scissor(scissor);
    if (level % 2 == 0) {  // the picture in temp for the vertical pass
        bind(temp);
        sample(2, sd);
        if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
    }

    BlurConstants k{};
    blur_weights(k.weights, sigma);
    region_limits(k.min, k.max, scissor, sd.w, sd.h);
    k.texel[1] = 1.f / float(temp.h);
    bind(sd);
    sample(2, temp);
    if (prepare(kBlur)) {
        if (const auto gpu = constants(&k, sizeof k)) {
            gpu_cmd_bind_cb(mCmd, 1, gpu);
            draw_quad(0, 0, 1, 1);
        }
    }

    // The horizontal pass, with a transparent pixel around the window first: the upscale's taps at the edge take
    // that, not what was there before (RmlUi's fix for some drivers' blits).
    bind(temp);
    const Rml::Rectanglei padded = scissor.Extend(1);
    const gpu_rect clear_rect{Rml::Math::Clamp(padded.Left(), 0, temp.w), Rml::Math::Clamp(padded.Top(), 0, temp.h),
                              Rml::Math::Clamp(padded.Right(), 0, temp.w), Rml::Math::Clamp(padded.Bottom(), 0, temp.h)};
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    if (clear_rect.right > clear_rect.left && clear_rect.bottom > clear_rect.top)
        gpu_cmd_clear_color(mCmd, temp.rtv, zero, &clear_rect);
    k.texel[0] = 1.f / float(sd.w);
    k.texel[1] = 0.f;
    sample(2, sd);
    if (prepare(kBlur)) {
        if (const auto gpu = constants(&k, sizeof k)) {
            gpu_cmd_bind_cb(mCmd, 1, gpu);
            draw_quad(0, 0, 1, 1);
        }
    }

    // Back up into the window, then once more by an exact power of two where that does not fill it the same way
    // (steadier at large sigmas).
    bind(sd);
    set_scissor(window);
    blit(temp, scissor, window);
    const Rml::Vector2i target_min = scissor.p0 * (1 << level), target_max = scissor.p1 * (1 << level);
    if (target_min != window.p0 || target_max != window.p1)
        blit(temp, scissor, Rml::Rectanglei::FromCorners(target_min, target_max));

    set_scissor(original);
    mBlend = saved;
}

// ---- Gradients ----

Rml::CompiledShaderHandle RendererImpl::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) {
    CompiledShader shader;
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if (name == "shader") {  // decorator: shader("model:lizard"), one of the game's models (docs/112)
        const Rml::String value = Rml::Get(parameters, "value", Rml::String());
        if (value.rfind("model:", 0) != 0 || value.size() <= 6) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader '%s'.", value.c_str());
            return {};
        }
        shader.model = value.substr(6);
        return reinterpret_cast<Rml::CompiledShaderHandle>(new CompiledShader(std::move(shader)));
    }
    if (name == "linear-gradient") {
        shader.function = repeating ? GradientFunction::RepeatingLinear : GradientFunction::Linear;
        shader.p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        shader.v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - shader.p;
    } else if (name == "radial-gradient") {
        shader.function = repeating ? GradientFunction::RepeatingRadial : GradientFunction::Radial;
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        shader.v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
    } else if (name == "conic-gradient") {
        shader.function = repeating ? GradientFunction::RepeatingConic : GradientFunction::Conic;
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        const float angle = Rml::Get(parameters, "angle", 0.f);
        shader.v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
    } else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
        return {};
    }
    const auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) return {};
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    const int n = std::min(int(stops.size()), kMaxStops);
    if (n == 0) return {};
    for (int i = 0; i < n; i++) {
        shader.stop_positions.push_back(stops[size_t(i)].position.number);
        shader.stop_colors.push_back(to_float(stops[size_t(i)].color));
    }
    return reinterpret_cast<Rml::CompiledShaderHandle>(new CompiledShader(std::move(shader)));
}

void RendererImpl::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                                Rml::Vector2f translation, Rml::TextureHandle) {
    const auto* s = reinterpret_cast<const CompiledShader*>(shader);
    const auto* g = reinterpret_cast<const Geometry*>(geometry);
    if (s && g && !s->model.empty()) {
        // The element's box on the target, through the transform, and the model fitted into it.
        Rml::Vector2f lo(1e9f), hi(-1e9f);
        for (const Rml::Vector2f c : {g->lo, Rml::Vector2f(g->hi.x, g->lo.y), g->hi, Rml::Vector2f(g->lo.x, g->hi.y)}) {
            const Rml::Vector4f q = mTransform * Rml::Vector4f(c.x + translation.x, c.y + translation.y, 0.f, 1.f);
            if (q.w <= 0.f) return;
            const Rml::Vector2f px((q.x / q.w + 1.f) * 0.5f * float(mWidth), (1.f - q.y / q.w) * 0.5f * float(mHeight));
            lo = Rml::Math::Min(lo, px);
            hi = Rml::Math::Max(hi, px);
        }
        ModelView view;
        view.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
        const float spin = ui::models::spin(s->model);
        std::memcpy(view.rotation, ui::models::look(s->model, float(std::fmod(double(spin) * view.time, 360.0))).rotation,
                    sizeof view.rotation);
        view.turning = spin != 0.f;
        view.at[0] = lo.x;
        view.at[1] = lo.y;
        view.size[0] = hi.x - lo.x;
        view.size[1] = hi.y - lo.y;
        render_model(s->model, view, g->alpha);
        return;
    }
    if (!s || !g || !g->count || !prepare(kGradient)) return;
    GradientConstants c{};
    store(mTransform, c.transform);
    c.translate[0] = translation.x;
    c.translate[1] = translation.y;
    c.func = int32_t(s->function);
    c.num_stops = int32_t(s->stop_positions.size());
    c.p[0] = s->p.x;
    c.p[1] = s->p.y;
    c.v[0] = s->v.x;
    c.v[1] = s->v.y;
    c.range[0] = s->stop_positions.front();
    c.range[1] = s->stop_positions.back();
    for (size_t i = 0; i < s->stop_positions.size(); i++) {
        c.positions[i][0] = s->stop_positions[i];
        for (int k = 0; k < 4; k++) c.colors[i][k] = s->stop_colors[i][k];
    }
    if (const auto gpu = constants(&c, sizeof c)) {
        gpu_cmd_bind_cb(mCmd, 0, gpu);
        draw(*g);
    }
}

void RendererImpl::ReleaseShader(Rml::CompiledShaderHandle shader) {
    delete reinterpret_cast<CompiledShader*>(shader);
}

// ---- The game's models (docs/112) ----

// One of a model's textures: its BC1/2/3 blocks as they are, with the mip levels it has.
Texture* RendererImpl::make_block_texture(const ui::models::Texture& src) {
    if (src.width <= 0 || src.height <= 0 || src.width % 4 || src.height % 4 || src.levels < 1 || !mOpen) return nullptr;
    const gpu_format format = src.format == ui::models::TexFormat::BC1   ? GPU_FORMAT_BC1_UNORM
                              : src.format == ui::models::TexFormat::BC2 ? GPU_FORMAT_BC2_UNORM
                                                                         : GPU_FORMAT_BC3_UNORM;
    const size_t block = src.format == ui::models::TexFormat::BC1 ? 8 : 16;
    const uint32_t levels = uint32_t(src.levels);
    const gpu_texture_desc desc = texture_desc(src.width, src.height, levels, format, 1, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                               GPU_STATE_COPY_DEST);
    const gpu_texture tex = make_texture(desc);
    if (!GPU_VALID(tex)) return nullptr;
    auto* t = new Texture{tex, mSrv.alloc(), {}, GPU_STATE_COPY_DEST};
    if (t->srv == kNone) {
        gpu_texture_destroy(mDev, tex, 0);  // (never recorded)
        delete t;
        return nullptr;
    }
    gpu_view_desc sd = view_of(tex, GPU_VIEW_SRV, 1);
    sd.format = format;
    sd.swizzle = GPU_SWIZZLE_IDENTITY;
    sd.mips = uint8_t(levels);
    gpu_table_write_tex(mDev, mPool, t->srv, &sd);
    t->table = gpu_table_ref(mDev, mPool, t->srv);

    std::vector<gpu_footprint> fp(levels);
    std::vector<uint32_t> rows(levels);
    std::vector<uint64_t> row_bytes(levels);
    const uint64_t total = gpu_texture_footprints(mDev, &desc, 0, levels, 0, fp.data(), rows.data(), row_bytes.data());
    const Block b = alloc_block(std::max<size_t>(size_t(total), kPlacementAlign));
    if (GPU_VALID(b.buffer)) {
        size_t from = 0;  // each level's rows of blocks, one level after the other
        int lw = src.width, lh = src.height;
        for (uint32_t m = 0; m < levels; m++) {
            const size_t row = size_t(std::max(1, (lw + 3) / 4)) * block, nrows = size_t(std::max(1, (lh + 3) / 4));
            if (from + row * nrows > src.data.size() || row > row_bytes[m] || nrows > rows[m]) break;
            for (size_t y = 0; y < nrows; y++)
                std::memcpy(b.cpu + fp[m].offset + size_t(fp[m].row_pitch) * y, src.data.data() + from + row * y, row);
            from += row * nrows;
            gpu_footprint from_buffer = fp[m];
            from_buffer.offset += b.offset;
            gpu_cmd_copy_buf_to_tex(mCmd, tex, m, 0, 0, 0, b.buffer, &from_buffer);
            lw = std::max(1, lw / 2);
            lh = std::max(1, lh / 2);
        }
        retire([this, b] { free_block(b); });
    }
    transition(t->tex, t->state, GPU_STATE_PIXEL_SHADER_RESOURCE);
    return t;
}

// The model's GPU side, made the first time it is drawn (its files read then, souls_model.cpp).
GpuModel* RendererImpl::gpu_model(const std::string& name) {
    if (const auto it = mModels.find(name); it != mModels.end()) return &it->second;
    GpuModel& g = mModels[name];
    g.model = ui::models::get(name);
    if (!g.model) return &g;
    size_t size = 0;
    for (const ui::models::Mesh& mesh : ui::models::meshes(*g.model)) {
        GpuModel::Part part;
        part.indices = size;
        part.index_count = uint32_t(mesh.indices.size());
        size = (size + mesh.indices.size() * sizeof(uint32_t) + 15) & ~size_t(15);
        part.vertices = size;
        part.vertex_count = uint32_t(mesh.vertices.size());
        size += mesh.vertices.size() * sizeof(ui::models::Vertex);
        g.parts.push_back(part);
    }
    g.block = alloc_block(std::max<size_t>(size, 256));
    if (!GPU_VALID(g.block.buffer)) {
        g.model = nullptr;
        return &g;
    }
    const auto& meshes = ui::models::meshes(*g.model);
    for (size_t i = 0; i < meshes.size(); i++) {
        std::memcpy(g.block.cpu + g.parts[i].indices, meshes[i].indices.data(), meshes[i].indices.size() * sizeof(uint32_t));
        std::memcpy(g.block.cpu + g.parts[i].vertices, meshes[i].vertices.data(),
                    meshes[i].vertices.size() * sizeof(ui::models::Vertex));
    }
    for (const ui::models::Texture& t : ui::models::textures(*g.model)) g.textures.push_back(make_block_texture(t));
    return &g;
}

// The model into the bound layer, under the scissor and clip as they are: its opaque meshes with the depth buffer, then
// its blended ones over them. The depth is cleared first in the square the model can reach.
bool RendererImpl::render_model(const std::string& name, const ModelView& view, float opacity) {
    if (!mOpen || !mBound || !mBound->layer || !GPU_VALID(mStencil) || !(opacity > 0.f)) return false;
    GpuModel* g = gpu_model(name);
    if (!g || !g->model) return false;
    const ui::models::Model& m = *g->model;
    const ui::models::Look look = ui::models::look(name, 0.f);
    float lo[3], hi[3];
    ui::models::bounds(m, lo, hi);
    const float* R = view.rotation;

    // Where: the pivot at a point, or the middle of the model's box at the middle of the box given, fitted into it.
    float pivot[3], at[2], scale = view.scale;
    if (scale > 0.f) {
        std::memcpy(pivot, view.pivot, sizeof pivot);
        at[0] = view.at[0];
        at[1] = view.at[1];
    } else {
        for (int k = 0; k < 3; k++) pivot[k] = 0.5f * (lo[k] + hi[k]);
        float ext[2] = {0.f, 0.f};
        if (view.turning) {
            // Fitted to the upright cylinder around its box, which looks the same at every turn: the model keeps its
            // size as it turns.
            const float r = 0.5f * std::hypot(hi[0] - lo[0], hi[2] - lo[2]), h = 0.5f * (hi[1] - lo[1]);
            for (int k = 0; k < 2; k++) ext[k] = std::fabs(R[k * 3 + 1]) * h + r * std::hypot(R[k * 3], R[k * 3 + 2]);
        } else {
            for (int c = 0; c < 8; c++) {
                const float d[3] = {(c & 1 ? hi[0] : lo[0]) - pivot[0], (c & 2 ? hi[1] : lo[1]) - pivot[1],
                                    (c & 4 ? hi[2] : lo[2]) - pivot[2]};
                for (int k = 0; k < 2; k++)
                    ext[k] = std::max(ext[k], std::fabs(R[k * 3] * d[0] + R[k * 3 + 1] * d[1] + R[k * 3 + 2] * d[2]));
            }
        }
        if (!(ext[0] > 0.f) || !(ext[1] > 0.f) || view.size[0] < 1.f || view.size[1] < 1.f) return false;
        scale = std::min(view.size[0] / (2.f * ext[0]), view.size[1] / (2.f * ext[1]));
        at[0] = view.at[0] + 0.5f * view.size[0];
        at[1] = view.at[1] + 0.5f * view.size[1];
    }
    // How far the model reaches from the pivot, in pixels: the depth's range, and the square whose depth is cleared.
    float reach = 0.f;
    for (int c = 0; c < 8; c++) {
        const float d[3] = {(c & 1 ? hi[0] : lo[0]) - pivot[0], (c & 2 ? hi[1] : lo[1]) - pivot[1],
                            (c & 4 ? hi[2] : lo[2]) - pivot[2]};
        reach = std::max(reach, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    // The effects reach a little further: the glow's blur, half a star glint.
    const float length = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * scale;
    const float sigma = look.glow_strength > 0.f ? look.glow_radius * length : 0.f;
    const float glint = look.sparkles > 0 ? look.sparkle_size * length : 0.f;
    reach = reach * scale + std::max(3.f * sigma, 0.5f * glint) + 2.f;
    if (!(reach < 100000.f)) return false;
    const gpu_rect square{Rml::Math::Clamp(int(std::floor(at[0] - reach)), 0, mBound->w),
                          Rml::Math::Clamp(int(std::floor(at[1] - reach)), 0, mBound->h),
                          Rml::Math::Clamp(int(std::ceil(at[0] + reach)), 0, mBound->w),
                          Rml::Math::Clamp(int(std::ceil(at[1] + reach)), 0, mBound->h)};
    if (square.right <= square.left || square.bottom <= square.top) return true;  // (all of it off the target)
    gpu_cmd_clear_depth(mCmd, mStencilDsv, GPU_CLEAR_DEPTH, 1.f, 0, &square);

    // Model space to pixels: R (p - pivot) scale + (at, 0); then x and y to clip space, and z (toward the viewer, within
    // +-reach) to a depth from 0 at the front to 1 at the back.
    ModelConstants k{};
    const float W = float(mBound->w), H = float(mBound->h);
    const float out[3][2] = {{2.f / W, -1.f}, {-2.f / H, 1.f}, {-0.5f / reach, 0.5f}};  // (pixels to clip: a scale, an offset)
    const float origin[3] = {at[0], at[1], 0.f};
    for (int r = 0; r < 3; r++) {
        float row[4];
        for (int c = 0; c < 3; c++) row[c] = R[r * 3 + c] * scale;
        row[3] = origin[r] - (row[0] * pivot[0] + row[1] * pivot[1] + row[2] * pivot[2]);
        for (int c = 0; c < 4; c++) k.clip[c * 4 + r] = row[c] * out[r][0] + (c == 3 ? out[r][1] : 0.f);
        for (int c = 0; c < 3; c++) k.view[r][c] = R[r * 3 + c];
    }
    k.clip[15] = 1.f;
    std::memcpy(k.light, look.light, sizeof look.light);
    k.light[3] = look.ambient;
    k.fx[0] = float(std::fmod(view.time, 3600.0));
    k.fx[1] = look.glitter;

    // The vertices: an animated model's, posed now, in the ring; a rigid one's, in its block.
    const auto& meshes = ui::models::meshes(m);
    std::vector<gpu_addr> vertices(meshes.size());
    const bool posed =
        ui::models::duration(m) > 0 && ui::models::pose(m, view.time, mPosed) && mPosed.size() == meshes.size();
    if (posed) {
        size_t total = 0;
        for (const auto& p : mPosed) total += p.size() * sizeof(ui::models::Vertex);
        const auto [cpu, gpu] = ring(std::max<size_t>(total, 16), 16);
        if (!cpu) return false;
        size_t used = 0;
        for (size_t i = 0; i < mPosed.size(); i++) {
            std::memcpy(cpu + used, mPosed[i].data(), mPosed[i].size() * sizeof(ui::models::Vertex));
            vertices[i] = gpu + used;
            used += mPosed[i].size() * sizeof(ui::models::Vertex);
        }
    } else {
        for (size_t i = 0; i < meshes.size(); i++) vertices[i] = g->block.gpu + g->parts[i].vertices;
    }

    const Blend saved = mBlend;
    mBlend = kPremultiplied;
    const auto pass = [&](bool blended) {
        k.flags[0] = blended ? 1.f : 0.f;  // a blended mesh: its faces toward the viewer only
        k.flags[1] = blended ? 0.f : 1.f;
        k.flags[2] = look.alpha_test;
        for (int c = 0; c < 3; c++) k.tint[c] = blended ? look.tint[c] : 1.f;
        k.tint[3] = opacity;
    };
    const auto draw_meshes = [&](bool blended) {
        gpu_addr constants_at = 0;
        for (size_t i = 0; i < meshes.size(); i++) {
            const ui::models::Mesh& mesh = meshes[i];
            const GpuModel::Part& part = g->parts[i];
            if (mesh.blend != blended || !part.index_count || !part.vertex_count) continue;
            if (!prepare(blended ? kModelBlend : kModel)) break;
            if (!constants_at && !(constants_at = constants(&k, sizeof k))) break;
            gpu_cmd_bind_cb(mCmd, 0, constants_at);
            const auto table = [&](int t) {
                return t >= 0 && size_t(t) < g->textures.size() && g->textures[size_t(t)] ? g->textures[size_t(t)]->table
                                                                                         : mNullTable;
            };
            gpu_cmd_bind_table(mCmd, 2, table(mesh.texture));
            gpu_cmd_bind_table(mCmd, 3, table(mesh.specular));
            gpu_cmd_set_vb(mCmd, 0, vertices[i], uint32_t(part.vertex_count * sizeof(ui::models::Vertex)),
                           uint32_t(sizeof(ui::models::Vertex)));
            gpu_cmd_set_ib(mCmd, g->block.gpu + part.indices, uint32_t(part.index_count * sizeof(uint32_t)),
                           GPU_FORMAT_R32_UINT);
            gpu_cmd_draw_indexed(mCmd, part.index_count, 1, 0, 0, 0);
        }
    };

    // The glow (the sword's moonlight): the blended meshes' alpha in the glow's colour, drawn into the first
    // postprocess target, blurred there, and put on the layer under the model.
    Target* layer = mBound;
    Target* mask = sigma >= 0.5f ? post(0) : nullptr;
    Target* temp = mask ? post(1) : nullptr;
    const Rml::Rectanglei area = region();
    const Rml::Rectanglei window = Rml::Rectanglei::FromCorners(
        {std::max(int(square.left), area.Left()), std::max(int(square.top), area.Top())},
        {std::min(int(square.right), area.Right()), std::min(int(square.bottom), area.Bottom())});
    if (mask && temp && window.Width() > 0 && window.Height() > 0) {
        const Rml::Rectanglei scissor = mScissor;
        bind(*mask);
        const gpu_rect rect{window.Left(), window.Top(), window.Right(), window.Bottom()};
        const float zero[4] = {0.f, 0.f, 0.f, 0.f};
        gpu_cmd_clear_color(mCmd, mask->rtv, zero, &rect);
        set_scissor(window);
        pass(true);
        std::memcpy(k.tint, look.glow, sizeof look.glow);
        k.tint[3] = look.glow_strength * opacity;
        k.fx[2] = 1.f;
        draw_meshes(true);
        k.fx[2] = 0.f;
        render_blur(sigma, *mask, *temp, window);
        bind(*layer);
        mBlend = kPremultiplied;
        sample(2, *mask);
        if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
        set_scissor(scissor);
    }

    pass(false);
    draw_meshes(false);
    pass(true);
    draw_meshes(true);

    // The twinkle (the lizard's): star glints, each for a moment on one of its gems and then on another, where the
    // gem is in the pose and a little in front of it, so the model's depth hides those behind it; at the outline too,
    // where they show against what is behind the model, but none on a gem turned well away.
    const auto& spots = ui::models::spots(m);
    if (glint > 0.f && !spots.empty()) {
        struct Glint {
            float x, y, z, bright;  // (it grows as it brightens, and shrinks as it fades)
        };
        std::vector<Glint> glints;
        for (int j = 0; j < look.sparkles; j++) {
            const double period = 1.1 + 0.37 * j, t = view.time / period + 0.41 * j;
            const double turn = std::floor(t), f = t - turn;
            if (f >= 0.5) continue;
            uint32_t h = uint32_t(int64_t(turn)) * 2654435761u ^ uint32_t(j + 1) * 40503u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            const ui::models::Spot& spot = spots[h % spots.size()];
            if (spot.mesh >= meshes.size()) continue;
            const std::vector<ui::models::Vertex>& vs = posed ? mPosed[spot.mesh] : meshes[spot.mesh].vertices;
            if (spot.vertex >= vs.size()) continue;
            const ui::models::Vertex& v = vs[spot.vertex];
            const float d[3] = {v.pos[0] - pivot[0], v.pos[1] - pivot[1], v.pos[2] - pivot[2]};
            float p[3];
            for (int r = 0; r < 3; r++)
                p[r] = (R[r * 3] * d[0] + R[r * 3 + 1] * d[1] + R[r * 3 + 2] * d[2]) * scale + origin[r];
            const float* nv = v.normal;
            const float nz = R[6] * nv[0] + R[7] * nv[1] + R[8] * nv[2];
            const float nl = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            if (!(nl > 0.f) || nz < -0.2f * nl) continue;
            glints.push_back({p[0], p[1], p[2] + 0.5f * glint, float(std::sin(3.14159265 * f / 0.5))});
        }
        const size_t n = glints.size();
        const auto [cpu, gpu] = n ? ring(n * (4 * sizeof(ui::models::Vertex) + 6 * sizeof(uint32_t)), 16)
                                  : std::pair<uint8_t*, gpu_addr>{nullptr, 0};
        if (cpu) {
            auto* quad = reinterpret_cast<ui::models::Vertex*>(cpu);
            auto* index = reinterpret_cast<uint32_t*>(cpu + n * 4 * sizeof(ui::models::Vertex));
            for (size_t i = 0; i < n; i++) {
                const Glint& gl = glints[i];
                const float half = 0.5f * glint * (0.5f + 0.5f * gl.bright);
                for (int c = 0; c < 4; c++) {
                    const float u = c == 1 || c == 2 ? 1.f : -1.f, w = c >= 2 ? 1.f : -1.f;
                    quad[i * 4 + c] = {{gl.x + u * half, gl.y + w * half, gl.z}, {gl.bright, 0.f, 0.f}, {u, w}};
                }
                const uint32_t b = uint32_t(i * 4), six[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
                std::memcpy(index + i * 6, six, sizeof six);
            }
            // Pixels to clip space as above, with no turn.
            ModelConstants q{};
            for (int r = 0; r < 3; r++) {
                q.clip[r * 4 + r] = out[r][0];
                q.clip[12 + r] = out[r][1];
                q.view[r][r] = 1.f;
            }
            q.clip[15] = 1.f;
            const float white[4] = {0.9f, 0.95f, 1.f, opacity};
            std::memcpy(q.tint, white, sizeof white);
            q.fx[2] = 2.f;
            mBlend = kPremultiplied;
            if (prepare(kModelBlend)) {
                if (const auto at_gpu = constants(&q, sizeof q)) {
                    gpu_cmd_bind_cb(mCmd, 0, at_gpu);
                    gpu_cmd_set_vb(mCmd, 0, gpu, uint32_t(n * 4 * sizeof(ui::models::Vertex)),
                                   uint32_t(sizeof(ui::models::Vertex)));
                    gpu_cmd_set_ib(mCmd, gpu + n * 4 * sizeof(ui::models::Vertex), uint32_t(n * 6 * sizeof(uint32_t)),
                                   GPU_FORMAT_R32_UINT);
                    gpu_cmd_draw_indexed(mCmd, uint32_t(n * 6), 1, 0, 0, 0);
                }
            }
        }
    }
    gpu_cmd_bind_table(mCmd, 3, mNullTable);  // (t1 is the mask filter's otherwise)
    mBlend = saved;
    return true;
}

// From host.cpp, after the context's Render: on the base layer, with no scissor or clip.
bool RendererImpl::draw_model(const char* name, const ModelView& view) {
    if (!name || !mOpen) return false;
    const Rml::Rectanglei scissor = mScissor;
    const bool clip = mClip;
    set_scissor(Rml::Rectanglei::MakeInvalid());
    mClip = false;
    const bool drawn = render_model(name, view, 1.f);
    set_scissor(scissor);
    mClip = clip;
    return drawn;
}

}  // namespace

std::unique_ptr<Renderer> create(gpu_device* device, unsigned first_slot, BackBuffers back_buffers, ImageSource images) {
    if (!device || !back_buffers) return nullptr;
    auto r = std::make_unique<RendererImpl>(device, false, first_slot, std::move(back_buffers), std::move(images));
    if (!r->init()) return nullptr;
    return r;
}

std::unique_ptr<Renderer> create_d3d12(void* device, void* queue, ImageSource images) {
#ifdef _WIN32
    if (!device || !queue) return nullptr;
    gpu_device_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.cmd_slots = kFrames;
    gpu_device* const dev = gpu_d3d12_create_on(device, queue, &desc);
    if (!dev) return nullptr;
    auto r = std::make_unique<RendererImpl>(dev, true, 0, BackBuffers{}, std::move(images));
    if (!r->init()) return nullptr;  // (the renderer's end destroys the device)
    return r;
#else
    (void)device;
    (void)queue;
    (void)images;
    return nullptr;
#endif
}

}  // namespace ui::gpu

#endif  // PS3RECOMP_RSX_HAL
