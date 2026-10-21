// Written for Demon's Souls. RmlUi's renderer on D3D12 (render_d3d12.hpp, docs/76). RmlUi's own D3D11 renderer
// (Backends/RmlUi_Renderer_DX11.cpp, MIT) rewritten for D3D12, and in D3D's conventions throughout (rows top down, no
// flips): its shaders, its layers and filters (opacity, blur, drop shadow, the colour matrices, mask images), its
// gradients and its clip mask in the stencil buffer, all in premultiplied alpha.
//
// The UI is drawn into a layer: a multisampled target, all of them sharing one stencil buffer. Filters work in
// single-sampled targets of the same size, and end_frame composites the base layer over the back buffer. A frame's
// work is one command list, submitted to the game's queue ahead of its Present. Up to three frames are in flight, each
// with its own allocator and its own ring for constants and quads; what RmlUi lets go of (geometry, textures, the
// targets after a resize) is released once the GPU is past the frames that used it.
//
// Not here: RmlUi's "shader" decorator demo ("creation"), which the UI does not use.
#include "render_d3d12.hpp"

#ifdef _WIN32
#include <d3d12.h>
#include <d3dcompiler.h>
#else
#define WIDL_EXPLICIT_AGGREGATE_RETURNS  // (the methods that return a struct, as vkd3d implements them)
#include <vkd3d_utils.h>                 // D3D12 (through vkd3d.h), D3DCompile, D3D12SerializeRootSignature
#include <vkd3d_d3dcompiler.h>
#endif

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <thread>
#include <type_traits>
#include <utility>

namespace ui::d3d12 {
namespace {

#ifdef _WIN32
#define DES_PPV(pp) IID_PPV_ARGS(pp)
#else
#define DES_PPV(pp) \
    __vkd3d_uuidof<std::remove_pointer_t<std::remove_reference_t<decltype(*(pp))>>>(), reinterpret_cast<void**>(pp)
#endif

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

constexpr int kFrames = 3;                     // frames in flight
constexpr UINT kSrvCapacity = 4096;            // textures and targets, shader visible
constexpr UINT kRtvCapacity = 64;
constexpr UINT kNone = ~0u;                    // no descriptor
constexpr size_t kRingSize = size_t(4) << 20;  // a frame's constants and quads
constexpr size_t kPageSize = size_t(4) << 20;  // geometry and uploads, in blocks of powers of two
constexpr int kMinClass = 8, kMaxClass = 22;   // 256 bytes to 4 MB; larger ones get a buffer of their own
constexpr UINT64 kPlacementAlign = 512;        // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
constexpr UINT kPitchAlign = 256;              // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
constexpr UINT kDefaultMapping = 0x1688;       // D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr UINT kLayerSamples = 2;              // RmlUi's NUM_MSAA_SAMPLES
constexpr int kMaxStops = 16;

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

enum Program { kColor, kTexture, kGradient, kPassthrough, kColorMatrix, kBlendMask, kBlur, kDropShadow, kProgramCount };
const char* const kProgramPs[kProgramCount] = {kPsColor,       kPsTexture,   kPsGradient, kPsPassthrough,
                                               kPsColorMatrix, kPsBlendMask, kPsBlur,     kPsDropShadow};
const char* const kProgramName[kProgramCount] = {"color",        "texture",    "gradient", "passthrough",
                                                 "color matrix", "blend mask", "blur",     "drop shadow"};

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

struct Block {                         // a piece of an upload page, or a buffer of its own (cls < 0)
    ID3D12Resource* buffer = nullptr;
    size_t offset = 0;
    int cls = -1;
    uint8_t* cpu = nullptr;             // at offset
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;  // at offset
};

struct Geometry {
    Block block;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    UINT count = 0;
};

struct Texture {
    ID3D12Resource* res = nullptr;
    UINT srv = kNone;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

struct Target {
    ID3D12Resource* res = nullptr;
    UINT rtv = kNone, srv = kNone;  // (no shader view when multisampled)
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    int w = 0, h = 0;
    UINT samples = 1;
    DXGI_FORMAT format = kColorFormat;
    bool layer = false;             // drawn with the stencil buffer
};

class Descriptors {
public:
    bool create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool visible) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = type;
        d.NumDescriptors = capacity;
        d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&d, DES_PPV(&heap)))) return false;
        mInc = device->GetDescriptorHandleIncrementSize(type);
        mCpu = heap->GetCPUDescriptorHandleForHeapStart();
        if (visible) mGpu = heap->GetGPUDescriptorHandleForHeapStart();
        mCapacity = capacity;
        return true;
    }
    UINT alloc() {
        if (!mFree.empty()) {
            const UINT i = mFree.back();
            mFree.pop_back();
            return i;
        }
        return mNext < mCapacity ? mNext++ : kNone;
    }
    void free(UINT i) {
        if (i != kNone) mFree.push_back(i);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        h.ptr = mCpu.ptr + SIZE_T(i) * mInc;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h;
        h.ptr = mGpu.ptr + UINT64(i) * mInc;
        return h;
    }
    ID3D12DescriptorHeap* heap = nullptr;

private:
    D3D12_CPU_DESCRIPTOR_HANDLE mCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mGpu{};
    UINT mInc = 0, mCapacity = 0, mNext = 0;
    std::vector<UINT> mFree;
};

class RendererImpl final : public Renderer {
public:
    RendererImpl(ID3D12Device* device, ID3D12CommandQueue* queue, ImageSource images)
        : mDevice(device), mQueue(queue), mImages(std::move(images)) {
        mDevice->AddRef();
        mQueue->AddRef();
    }
    ~RendererImpl() override;
    bool init();

    // ---- Renderer ----
    void begin_frame(int width, int height) override;
    void end_frame(void* back_buffer) override;
    bool read_back(void* back_buffer, int width, int height, std::vector<unsigned char>& rgba) override;
    void release_back_buffers() override;

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
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12Resource* ring = nullptr;
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        size_t used = 0;
        uint64_t fence = 0;  // its last submission
    };
    struct Retired {
        uint64_t fence;
        std::function<void()> fn;
    };

    // Resources
    ID3D12Resource* make_buffer(D3D12_HEAP_TYPE heap, size_t size, D3D12_RESOURCE_STATES state);
    ID3D12Resource* make_texture(int w, int h, UINT levels, DXGI_FORMAT format, UINT samples, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear);
    Block alloc_block(size_t size);
    void free_block(const Block& b);
    std::pair<uint8_t*, D3D12_GPU_VIRTUAL_ADDRESS> ring(size_t size, size_t align);
    D3D12_GPU_VIRTUAL_ADDRESS constants(const void* data, size_t size);
    Texture* make_image(const uint8_t* rgba, int w, int h);
    std::unique_ptr<Target> make_target(UINT samples, bool layer);
    void drop(std::unique_ptr<Target>& t);
    void resize(int width, int height);
    Target* layer(int i) { return i >= 0 && i < mLayerCount && i < int(mLayers.size()) ? mLayers[i].get() : nullptr; }
    Target* post(int i);
    bool back_buffer(ID3D12Resource* res, Target& out);
    ID3D12PipelineState* pipeline(Program p, Blend b, Stencil s, const Target& t);

    // Command lists and the GPU's progress
    bool open();
    void submit();
    void finish();
    void wait(uint64_t value);
    void reclaim(bool all);
    void retire(std::function<void()> fn) { mRetired.push_back({mFenceNext, std::move(fn)}); }

    // Drawing
    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to);
    void transition(Target& t, D3D12_RESOURCE_STATES to) { transition(t.res, t.state, to); }
    void bind(Target& t);
    void sample(UINT param, Target& t);
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

    ID3D12Device* mDevice;
    ID3D12CommandQueue* mQueue;
    ImageSource mImages;

    ID3D12Fence* mFence = nullptr;
    uint64_t mFenceNext = 1;  // the value the next submission signals
    Frame mFrame[kFrames];
    int mFrameIndex = 0;
    ID3D12GraphicsCommandList* mList = nullptr;
    bool mOpen = false;
    std::deque<Retired> mRetired;

    ID3D12RootSignature* mRoot = nullptr;
    ID3DBlob* mVs[2] = {};  // main, quad
    ID3DBlob* mPs[kProgramCount] = {};
    std::map<uint64_t, ID3D12PipelineState*> mPipelines;
    Descriptors mSrv, mRtv, mDsv;
    UINT mNullSrv = kNone;

    std::vector<Block> mFreeBlocks[kMaxClass + 1];
    std::vector<ID3D12Resource*> mPages;
    uint8_t* mPageCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS mPageGpu = 0;
    size_t mPageUsed = kPageSize;

    UINT mSamples = 1;
    int mWidth = 0, mHeight = 0;
    std::vector<std::unique_ptr<Target>> mLayers;
    int mLayerCount = 0;
    ID3D12Resource* mStencil = nullptr;
    UINT mStencilDsv = kNone;
    std::unique_ptr<Target> mPost[4];  // primary, secondary, tertiary, blend mask
    std::map<ID3D12Resource*, UINT> mBackBufferRtv;

    // Render state
    Rml::Matrix4f mProjection, mTransform;
    Rml::Rectanglei mScissor = Rml::Rectanglei::MakeInvalid();
    Blend mBlend = kPremultiplied;
    float mBlendFactor = 1.f;
    bool mClip = false;
    UINT mStencilRef = 0;
    // What the command list has
    Target* mBound = nullptr;
    bool mScissorDirty = true;
    ID3D12PipelineState* mPso = nullptr;
    UINT mRefSet = kNone;
    float mFactorSet = -1.f;
};

// ---- Set-up ----

bool RendererImpl::init() {
    if (FAILED(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, DES_PPV(&mFence)))) return false;
    for (Frame& f : mFrame) {
        if (FAILED(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, DES_PPV(&f.alloc)))) return false;
        f.ring = make_buffer(D3D12_HEAP_TYPE_UPLOAD, kRingSize, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (!f.ring || FAILED(f.ring->Map(0, &none, &p))) return false;
        f.cpu = static_cast<uint8_t*>(p);
        f.gpu = f.ring->GetGPUVirtualAddress();
    }
    if (FAILED(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mFrame[0].alloc, nullptr, DES_PPV(&mList))))
        return false;
    mList->Close();
    if (!mSrv.create(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCapacity, true) ||
        !mRtv.create(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCapacity, false) ||
        !mDsv.create(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false))
        return false;
    mStencilDsv = mDsv.alloc();
    mNullSrv = mSrv.alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC none{};
    none.Format = kColorFormat;
    none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    none.Shader4ComponentMapping = kDefaultMapping;
    none.Texture2D.MipLevels = 1;
    mDevice->CreateShaderResourceView(nullptr, &none, mSrv.cpu(mNullSrv));

    // b0 (everything), b1 (the filters), t0, t1; s0 wraps (images), s1 clamps (the filters' targets).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    for (UINT i = 0; i < 2; i++) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
    }
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < 2; i++) {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.MaxLOD = 1000.0f;
        s.ShaderRegister = i;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 4;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr))
        hr = mDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), DES_PPV(&mRoot));
    release(blob);
    release(errors);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[ui] the menu's root signature failed (0x%08X)\n", unsigned(hr));
        return false;
    }

    auto compile = [](const char* source, const char* target, const char* name) -> ID3DBlob* {
        ID3DBlob* code = nullptr;
        ID3DBlob* log = nullptr;
        const HRESULT r = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", target, 0, 0, &code, &log);
        if (FAILED(r)) {
            std::fprintf(stderr, "[ui] the menu's %s shader did not compile (0x%08X)%s%s\n", name, unsigned(r),
                         log ? ": " : "", log ? static_cast<const char*>(log->GetBufferPointer()) : "");
            release(code);
        }
        release(log);
        return code;
    };
    mVs[0] = compile(kVsMain, "vs_5_0", "main");
    mVs[1] = compile(kVsQuad, "vs_5_0", "quad");
    for (int p = 0; p < kProgramCount; p++) mPs[p] = compile(kProgramPs[p], "ps_5_0", kProgramName[p]);
    // What the menu cannot do without; the effects only fall away if theirs are missing.
    if (!mVs[0] || !mVs[1] || !mPs[kColor] || !mPs[kTexture] || !mPs[kPassthrough]) return false;

    // RmlUi's two samples to a pixel (smooth edges for rounded and turned shapes), if the device has them.
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
    bool msaa = true;
    for (DXGI_FORMAT f : {kColorFormat, kStencilFormat}) {
        q = {};
        q.Format = f;
        q.SampleCount = kLayerSamples;
        msaa = msaa && SUCCEEDED(mDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) &&
               q.NumQualityLevels > 0;
    }
    mSamples = msaa ? kLayerSamples : 1;
    return true;
}

RendererImpl::~RendererImpl() {
    if (mList) finish();
    reclaim(true);
    for (auto& t : mLayers) drop(t);
    for (auto& t : mPost) drop(t);
    release(mStencil);
    reclaim(true);
    for (ID3D12Resource* p : mPages) p->Release();
    for (auto& [key, pso] : mPipelines) release(pso);
    for (ID3DBlob*& b : mVs) release(b);
    for (ID3DBlob*& b : mPs) release(b);
    release(mRoot);
    release(mSrv.heap);
    release(mRtv.heap);
    release(mDsv.heap);
    for (Frame& f : mFrame) {
        release(f.ring);
        release(f.alloc);
    }
    release(mList);
    release(mFence);
    mQueue->Release();
    mDevice->Release();
}

ID3D12Resource* RendererImpl::make_buffer(D3D12_HEAP_TYPE heap, size_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(mDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, DES_PPV(&r)))) return nullptr;
    return r;
}

ID3D12Resource* RendererImpl::make_texture(int w, int h, UINT levels, DXGI_FORMAT format, UINT samples,
                                           D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = UINT64(w);
    d.Height = UINT(h);
    d.DepthOrArraySize = 1;
    d.MipLevels = UINT16(levels);
    d.Format = format;
    d.SampleDesc.Count = samples;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(mDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, DES_PPV(&r)))) {
        std::fprintf(stderr, "[ui] could not make a %dx%d texture for the menu\n", w, h);
        return nullptr;
    }
    return r;
}

// Upload memory in blocks of powers of two, carved from 4 MB pages and reused once the GPU is done with them. A block
// starts at a multiple of its size (up to 64 KB), so blocks of 512 bytes and up can hold a texture's upload.
Block RendererImpl::alloc_block(size_t size) {
    int cls = kMinClass;
    while (cls <= kMaxClass && (size_t(1) << cls) < size) cls++;
    Block b;
    if (cls > kMaxClass) {
        b.buffer = make_buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (!b.buffer || FAILED(b.buffer->Map(0, &none, &p))) {
            release(b.buffer);
            return {};
        }
        b.cpu = static_cast<uint8_t*>(p);
        b.gpu = b.buffer->GetGPUVirtualAddress();
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
        ID3D12Resource* page = make_buffer(D3D12_HEAP_TYPE_UPLOAD, kPageSize, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (!page || FAILED(page->Map(0, &none, &p))) {
            release(page);
            return {};
        }
        mPages.push_back(page);
        mPageCpu = static_cast<uint8_t*>(p);
        mPageGpu = page->GetGPUVirtualAddress();
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
    if (!b.buffer) return;
    if (b.cls < 0) b.buffer->Release();
    else mFreeBlocks[b.cls].push_back(b);
}

// Space in this frame's ring; past its end, a block that goes with the frame.
std::pair<uint8_t*, D3D12_GPU_VIRTUAL_ADDRESS> RendererImpl::ring(size_t size, size_t align) {
    Frame& f = mFrame[mFrameIndex];
    const size_t at = (f.used + align - 1) & ~(align - 1);
    if (at + size <= kRingSize) {
        f.used = at + size;
        return {f.cpu + at, f.gpu + at};
    }
    const Block b = alloc_block(std::max<size_t>(size, 256));  // (blocks of 256 bytes and up are 256-aligned)
    if (!b.buffer) return {nullptr, 0};
    retire([this, b] { free_block(b); });
    return {b.cpu, b.gpu};
}

D3D12_GPU_VIRTUAL_ADDRESS RendererImpl::constants(const void* data, size_t size) {
    const auto [cpu, gpu] = ring(size, 256);
    if (cpu) std::memcpy(cpu, data, size);
    return cpu ? gpu : 0;
}

std::unique_ptr<Target> RendererImpl::make_target(UINT samples, bool layer) {
    auto t = std::make_unique<Target>();
    t->w = mWidth;
    t->h = mHeight;
    t->samples = samples;
    t->layer = layer;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    t->res = make_texture(mWidth, mHeight, 1, kColorFormat, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    t->rtv = mRtv.alloc();
    if (samples == 1) t->srv = mSrv.alloc();
    if (!t->res || t->rtv == kNone || (samples == 1 && t->srv == kNone)) {
        drop(t);
        return nullptr;
    }
    mDevice->CreateRenderTargetView(t->res, nullptr, mRtv.cpu(t->rtv));
    if (samples == 1) mDevice->CreateShaderResourceView(t->res, nullptr, mSrv.cpu(t->srv));
    return t;
}

void RendererImpl::drop(std::unique_ptr<Target>& t) {
    if (!t) return;
    ID3D12Resource* res = t->res;
    const UINT rtv = t->rtv, srv = t->srv;
    retire([this, res, rtv, srv] {
        if (res) res->Release();
        mRtv.free(rtv);
        mSrv.free(srv);
    });
    t.reset();
}

// A new size: the targets and the stencil buffer are made again at it, as they are needed.
void RendererImpl::resize(int width, int height) {
    for (auto& t : mLayers) drop(t);
    mLayers.clear();
    for (auto& t : mPost) drop(t);
    if (ID3D12Resource* s = mStencil) retire([s] { s->Release(); });
    mStencil = nullptr;
    mWidth = width;
    mHeight = height;
}

Target* RendererImpl::post(int i) {
    if (!mPost[i]) mPost[i] = make_target(1, false);
    return mPost[i].get();
}

// The back buffer as a target: its view, made once for each buffer.
bool RendererImpl::back_buffer(ID3D12Resource* res, Target& out) {
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    auto it = mBackBufferRtv.find(res);
    if (it == mBackBufferRtv.end()) {
        if (mBackBufferRtv.size() >= 8) {  // (buffers changed without release_back_buffers; views are read as recorded)
            for (auto& [r, slot] : mBackBufferRtv) mRtv.free(slot);
            mBackBufferRtv.clear();
        }
        const UINT slot = mRtv.alloc();
        if (slot == kNone) return false;
        mDevice->CreateRenderTargetView(res, nullptr, mRtv.cpu(slot));
        it = mBackBufferRtv.emplace(res, slot).first;
    }
    out.res = res;
    out.rtv = it->second;
    out.state = D3D12_RESOURCE_STATE_PRESENT;
    out.w = int(d.Width);
    out.h = int(d.Height);
    out.samples = 1;
    out.format = d.Format;
    out.layer = false;
    return true;
}

ID3D12PipelineState* RendererImpl::pipeline(Program p, Blend b, Stencil s, const Target& t) {
    if (!mPs[p]) return nullptr;
    const uint64_t key = uint64_t(p) | uint64_t(b) << 4 | uint64_t(s) << 8 | uint64_t(t.format) << 12 |
                         uint64_t(t.samples) << 24 | uint64_t(t.layer) << 32;
    if (const auto it = mPipelines.find(key); it != mPipelines.end()) return it->second;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(Rml::Vertex) == 20, "the input layout above");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = mRoot;
    ID3DBlob* vs = mVs[p < kPassthrough ? 0 : 1];
    d.VS.pShaderBytecode = vs->GetBufferPointer();
    d.VS.BytecodeLength = vs->GetBufferSize();
    d.PS.pShaderBytecode = mPs[p]->GetBufferPointer();
    d.PS.BytecodeLength = mPs[p]->GetBufferSize();
    D3D12_RENDER_TARGET_BLEND_DESC& rt = d.BlendState.RenderTarget[0];
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = b == kNoColor ? 0 : 0xF;
    if (b == kPremultiplied) {  // (RmlUi's colours and textures are premultiplied)
        rt.BlendEnable = TRUE;
        rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    } else if (b == kFactor) {  // the opacity filter: the source times the blend factor
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    }
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = FALSE;
    d.RasterizerState.MultisampleEnable = t.samples > 1;
    D3D12_DEPTH_STENCIL_DESC& ds = d.DepthStencilState;
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (t.layer && s != kStencilOff) {
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = 0xFF;
        ds.StencilWriteMask = s == kStencilTest ? 0 : 0xFF;
        D3D12_DEPTH_STENCILOP_DESC op{};
        op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        op.StencilFunc = s == kStencilTest ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        if (s == kStencilSet) op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        if (s == kStencilIntersect) op.StencilPassOp = D3D12_STENCIL_OP_INCR_SAT;
        ds.FrontFace = ds.BackFace = op;
    }
    d.InputLayout.pInputElementDescs = layout;
    d.InputLayout.NumElements = 3;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = t.format;
    d.DSVFormat = t.layer ? kStencilFormat : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = t.samples;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(mDevice->CreateGraphicsPipelineState(&d, DES_PPV(&pso)))) {
        std::fprintf(stderr, "[ui] the menu's %s pipeline failed\n", kProgramName[p]);
        pso = nullptr;
    }
    mPipelines[key] = pso;
    return pso;
}

// ---- Command lists ----

bool RendererImpl::open() {
    if (mOpen) return true;
    const int next = (mFrameIndex + 1) % kFrames;
    Frame& f = mFrame[next];
    wait(f.fence);
    reclaim(false);
    if (FAILED(f.alloc->Reset()) || FAILED(mList->Reset(f.alloc, nullptr))) {
        std::fprintf(stderr, "[ui] could not start the menu's commands\n");
        return false;
    }
    mFrameIndex = next;
    f.used = 0;
    mOpen = true;
    ID3D12DescriptorHeap* heaps[] = {mSrv.heap};
    mList->SetDescriptorHeaps(1, heaps);
    mList->SetGraphicsRootSignature(mRoot);
    mList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mList->SetGraphicsRootConstantBufferView(0, f.gpu);
    mList->SetGraphicsRootConstantBufferView(1, f.gpu);
    mList->SetGraphicsRootDescriptorTable(2, mSrv.gpu(mNullSrv));
    mList->SetGraphicsRootDescriptorTable(3, mSrv.gpu(mNullSrv));
    mBound = nullptr;
    mScissorDirty = true;
    mPso = nullptr;
    mRefSet = kNone;
    mFactorSet = -1.f;
    return true;
}

void RendererImpl::submit() {
    if (!mOpen) return;
    mOpen = false;
    mBound = nullptr;
    if (FAILED(mList->Close())) {
        std::fprintf(stderr, "[ui] the menu's commands could not be closed\n");
        return;
    }
    ID3D12CommandList* lists[] = {mList};
    mQueue->ExecuteCommandLists(1, lists);
    mQueue->Signal(mFence, mFenceNext);
    mFrame[mFrameIndex].fence = mFenceNext++;
}

void RendererImpl::finish() {
    submit();
    wait(mFenceNext - 1);
    reclaim(false);
}

// With no event (a blocking wait, where the device has one), else by looking every 0.1 ms.
void RendererImpl::wait(uint64_t value) {
    if (!value || mFence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(mFence->SetEventOnCompletion(value, nullptr)) && mFence->GetCompletedValue() >= value) return;
    while (mFence->GetCompletedValue() < value) std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void RendererImpl::reclaim(bool all) {
    const uint64_t done = all ? UINT64_MAX : mFence->GetCompletedValue();
    while (!mRetired.empty() && mRetired.front().fence <= done) {
        std::function<void()> fn = std::move(mRetired.front().fn);
        mRetired.pop_front();
        fn();
    }
}

// ---- Drawing ----

void RendererImpl::transition(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
    if (state == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = 0xFFFFFFFFu;  // all
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = to;
    mList->ResourceBarrier(1, &b);
    state = to;
}

void RendererImpl::bind(Target& t) {
    transition(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = mRtv.cpu(t.rtv);
    if (t.layer) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = mDsv.cpu(mStencilDsv);
        mList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    } else {
        mList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    mBound = &t;
    set_viewport(0, 0, t.w, t.h);
    mScissorDirty = true;
}

void RendererImpl::sample(UINT param, Target& t) {
    transition(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mList->SetGraphicsRootDescriptorTable(param, mSrv.gpu(t.srv));
}

void RendererImpl::set_viewport(int x, int y, int w, int h) {
    D3D12_VIEWPORT v{float(x), float(y), float(std::max(w, 1)), float(std::max(h, 1)), 0.f, 1.f};
    mList->RSSetViewports(1, &v);
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
    transition(*mBound, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (a resolve or a copy may have taken it)
    const Stencil s = !mBound->layer ? kStencilOff : stencil != kStencilAuto ? stencil : mClip ? kStencilTest : kStencilOff;
    ID3D12PipelineState* pso = pipeline(p, mBlend, s, *mBound);
    if (!pso) return false;
    if (pso != mPso) {
        mList->SetPipelineState(pso);
        mPso = pso;
    }
    if (mBlend == kFactor && mFactorSet != mBlendFactor) {
        const float f[4] = {mBlendFactor, mBlendFactor, mBlendFactor, mBlendFactor};
        mList->OMSetBlendFactor(f);
        mFactorSet = mBlendFactor;
    }
    const UINT ref = s == kStencilTest ? mStencilRef : 1;
    if (s != kStencilOff && ref != mRefSet) {
        mList->OMSetStencilRef(ref);
        mRefSet = ref;
    }
    if (mScissorDirty) {
        D3D12_RECT r{0, 0, mBound->w, mBound->h};
        if (mScissor.Valid()) {
            r.left = Rml::Math::Clamp(mScissor.Left(), 0, mBound->w);
            r.top = Rml::Math::Clamp(mScissor.Top(), 0, mBound->h);
            r.right = Rml::Math::Clamp(mScissor.Right(), 0, mBound->w);
            r.bottom = Rml::Math::Clamp(mScissor.Bottom(), 0, mBound->h);
        }
        mList->RSSetScissorRects(1, &r);
        mScissorDirty = false;
    }
    return true;
}

bool RendererImpl::set_common(Rml::Vector2f translation) {
    CommonConstants c{};
    store(mTransform, c.transform);
    c.translate[0] = translation.x;
    c.translate[1] = translation.y;
    const D3D12_GPU_VIRTUAL_ADDRESS gpu = constants(&c, sizeof c);
    if (gpu) mList->SetGraphicsRootConstantBufferView(0, gpu);
    return gpu != 0;
}

void RendererImpl::draw(const Geometry& g) {
    mList->IASetVertexBuffers(0, 1, &g.vbv);
    mList->IASetIndexBuffer(&g.ibv);
    mList->DrawIndexedInstanced(g.count, 1, 0, 0, 0);
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
    D3D12_VERTEX_BUFFER_VIEW vb{gpu, UINT(sizeof vertices), UINT(sizeof(V))};
    D3D12_INDEX_BUFFER_VIEW ib{gpu + sizeof vertices, UINT(sizeof indices), DXGI_FORMAT_R32_UINT};
    mList->IASetVertexBuffers(0, 1, &vb);
    mList->IASetIndexBuffer(&ib);
    mList->DrawIndexedInstanced(6, 1, 0, 0, 0);
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
        transition(from, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transition(to, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        mList->ResolveSubresource(to.res, 0, from.res, 0, kColorFormat);
    } else {
        transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(to, D3D12_RESOURCE_STATE_COPY_DEST);
        mList->CopyResource(to.res, from.res);
    }
}

// ---- Frames ----

void RendererImpl::begin_frame(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (!open()) return;
    if (width != mWidth || height != mHeight) resize(width, height);
    mProjection = Rml::Matrix4f::ProjectOrtho(0, float(width), float(height), 0, -10000, 10000);
    mTransform = mProjection;
    mScissor = Rml::Rectanglei::MakeInvalid();
    mBlend = kPremultiplied;
    mClip = false;
    mStencilRef = 0;
    mLayerCount = 0;
    if (!mStencil) {
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kStencilFormat;
        clear.DepthStencil.Depth = 1.f;
        mStencil = make_texture(width, height, 1, kStencilFormat, mSamples, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        if (!mStencil) return;
        mDevice->CreateDepthStencilView(mStencil, nullptr, mDsv.cpu(mStencilDsv));
    }
    mList->ClearDepthStencilView(mDsv.cpu(mStencilDsv), D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    PushLayer();
}

void RendererImpl::end_frame(void* back_buffer) {
    if (!mOpen) return;
    Target* base = mStencil ? layer(0) : nullptr;
    Target* primary = base ? post(0) : nullptr;
    Target out;
    if (base && primary && back_buffer && this->back_buffer(static_cast<ID3D12Resource*>(back_buffer), out)) {
        resolve(*base, *primary);
        bind(out);
        mBlend = kPremultiplied;
        mScissor = Rml::Rectanglei::MakeInvalid();
        sample(2, *primary);
        if (prepare(kPassthrough)) draw_quad(0, 0, 1, 1);
        transition(out, D3D12_RESOURCE_STATE_PRESENT);
    }
    mLayerCount = 0;
    submit();
}

bool RendererImpl::read_back(void* back_buffer, int width, int height, std::vector<unsigned char>& rgba) {
    auto* res = static_cast<ID3D12Resource*>(back_buffer);
    if (!res || width <= 0 || height <= 0 || !open()) return false;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    width = std::min(width, int(d.Width));
    height = std::min(height, int(d.Height));
    const UINT pitch = (UINT(width) * 4 + kPitchAlign - 1) & ~(kPitchAlign - 1);
    ID3D12Resource* buffer = make_buffer(D3D12_HEAP_TYPE_READBACK, size_t(pitch) * size_t(height), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!buffer) return false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
    transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = d.Format;
    dst.PlacedFootprint.Footprint.Width = UINT(width);
    dst.PlacedFootprint.Footprint.Height = UINT(height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    const D3D12_BOX box{0, 0, 0, UINT(width), UINT(height), 1};
    mList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    transition(res, state, D3D12_RESOURCE_STATE_PRESENT);
    finish();
    void* p = nullptr;
    const D3D12_RANGE all{0, size_t(pitch) * size_t(height)};
    const bool ok = SUCCEEDED(buffer->Map(0, &all, &p));
    if (ok) {
        const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const size_t row = size_t(width) * 4;
        rgba.resize(row * size_t(height));
        for (int y = 0; y < height; y++) {
            unsigned char* out = rgba.data() + row * size_t(y);
            std::memcpy(out, static_cast<const uint8_t*>(p) + size_t(pitch) * size_t(y), row);
            if (bgra)
                for (size_t x = 0; x < row; x += 4) std::swap(out[x], out[x + 2]);
        }
        const D3D12_RANGE none{0, 0};
        buffer->Unmap(0, &none);
    }
    buffer->Release();
    return ok;
}

void RendererImpl::release_back_buffers() {
    finish();
    for (auto& [res, slot] : mBackBufferRtv) mRtv.free(slot);
    mBackBufferRtv.clear();
}

// ---- Geometry and textures ----

Rml::CompiledGeometryHandle RendererImpl::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    auto* g = new Geometry;
    const size_t vb = vertices.size() * sizeof(Rml::Vertex), ib = indices.size() * sizeof(int);
    if (vb && ib) {
        g->block = alloc_block(vb + ib);
        if (g->block.buffer) {
            std::memcpy(g->block.cpu, vertices.data(), vb);
            std::memcpy(g->block.cpu + vb, indices.data(), ib);
            g->vbv = {g->block.gpu, UINT(vb), UINT(sizeof(Rml::Vertex))};
            g->ibv = {g->block.gpu + vb, UINT(ib), DXGI_FORMAT_R32_UINT};
            g->count = UINT(indices.size());
        }
    }
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RendererImpl::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
    const auto* g = reinterpret_cast<const Geometry*>(geometry);
    const auto* t = reinterpret_cast<const Texture*>(texture);
    if (!g || !g->count) return;
    if (t) {
        if (!mOpen) return;
        mList->SetGraphicsRootDescriptorTable(2, mSrv.gpu(t->srv));
    }
    if (prepare(t ? kTexture : kColor) && set_common(translation)) draw(*g);
}

void RendererImpl::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    auto* g = reinterpret_cast<Geometry*>(geometry);
    if (!g) return;
    const Block b = g->block;
    delete g;
    if (b.buffer) retire([this, b] { free_block(b); });
}

// A texture from RGBA (premultiplied) with its mip levels, or without pixels, for SaveLayerAsTexture (one level, in
// the copy destination state).
Texture* RendererImpl::make_image(const uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !open()) return nullptr;
    UINT levels = 1;
    if (rgba)
        while ((w >> levels) || (h >> levels)) levels++;
    ID3D12Resource* res = make_texture(w, h, levels, kColorFormat, 1, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    if (!res) return nullptr;
    auto* t = new Texture{res, mSrv.alloc(), D3D12_RESOURCE_STATE_COPY_DEST};
    if (t->srv == kNone) {
        res->Release();
        delete t;
        return nullptr;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = kColorFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = kDefaultMapping;
    sd.Texture2D.MipLevels = levels;
    mDevice->CreateShaderResourceView(res, &sd, mSrv.cpu(t->srv));
    if (!rgba) return t;

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(levels);
    std::vector<UINT> rows(levels);
    std::vector<UINT64> row_bytes(levels);
    UINT64 total = 0;
    mDevice->GetCopyableFootprints(&desc, 0, levels, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    const Block b = alloc_block(std::max<size_t>(size_t(total), kPlacementAlign));
    if (b.buffer) {
        // Level 0 as given, each next level the 2x2 average of the one before (premultiplied, so a plain average).
        std::vector<uint8_t> level(rgba, rgba + size_t(w) * size_t(h) * 4), next;
        int lw = w, lh = h;
        for (UINT m = 0; m < levels; m++) {
            for (int y = 0; y < lh; y++)
                std::memcpy(b.cpu + fp[m].Offset + size_t(fp[m].Footprint.RowPitch) * size_t(y),
                            level.data() + size_t(lw) * 4 * size_t(y), size_t(lw) * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;
            src.pResource = b.buffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[m];
            src.PlacedFootprint.Offset += b.offset;
            mList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
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
    transition(t->res, t->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
    ID3D12Resource* res = t->res;
    const UINT srv = t->srv;
    delete t;
    retire([this, res, srv] {
        res->Release();
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
    UINT ref = 1;
    switch (operation) {
    case Rml::ClipMaskOperation::Set: break;
    case Rml::ClipMaskOperation::SetInverse: ref = 0; break;
    case Rml::ClipMaskOperation::Intersect:
        s = kStencilIntersect;
        ref = mStencilRef + 1;
        break;
    }
    if (s == kStencilSet) mList->ClearDepthStencilView(mDsv.cpu(mStencilDsv), D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
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
    if (!mOpen || !t || !mStencil) {
        mBound = nullptr;  // (nothing draws until a layer that exists is bound again)
    } else {
        bind(*t);
        const float clear[4] = {0.f, 0.f, 0.f, 0.f};
        mList->ClearRenderTargetView(mRtv.cpu(t->rtv), clear, 0, nullptr);
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
    transition(primary, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = primary.res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{UINT(r.Left()), UINT(r.Top()), 0, UINT(r.Right()), UINT(r.Bottom()), 1};
    mList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    transition(t->res, t->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
                    mList->SetGraphicsRootConstantBufferView(1, gpu);
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
                    mList->SetGraphicsRootConstantBufferView(1, gpu);
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
            mList->SetGraphicsRootDescriptorTable(3, mSrv.gpu(mNullSrv));
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
            mList->SetGraphicsRootConstantBufferView(1, gpu);
            draw_quad(0, 0, 1, 1);
        }
    }

    // The horizontal pass, with a transparent pixel around the window first: the upscale's taps at the edge take
    // that, not what was there before (RmlUi's fix for some drivers' blits).
    bind(temp);
    const Rml::Rectanglei padded = scissor.Extend(1);
    const D3D12_RECT clear_rect{Rml::Math::Clamp(padded.Left(), 0, temp.w), Rml::Math::Clamp(padded.Top(), 0, temp.h),
                                Rml::Math::Clamp(padded.Right(), 0, temp.w), Rml::Math::Clamp(padded.Bottom(), 0, temp.h)};
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    if (clear_rect.right > clear_rect.left && clear_rect.bottom > clear_rect.top)
        mList->ClearRenderTargetView(mRtv.cpu(temp.rtv), zero, 1, &clear_rect);
    k.texel[0] = 1.f / float(sd.w);
    k.texel[1] = 0.f;
    sample(2, sd);
    if (prepare(kBlur)) {
        if (const auto gpu = constants(&k, sizeof k)) {
            mList->SetGraphicsRootConstantBufferView(1, gpu);
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
        mList->SetGraphicsRootConstantBufferView(0, gpu);
        draw(*g);
    }
}

void RendererImpl::ReleaseShader(Rml::CompiledShaderHandle shader) {
    delete reinterpret_cast<CompiledShader*>(shader);
}

}  // namespace

std::unique_ptr<Renderer> Renderer::create(void* device, void* queue, ImageSource images) {
    if (!device || !queue) return nullptr;
    auto r = std::make_unique<RendererImpl>(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue),
                                            std::move(images));
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace ui::d3d12
