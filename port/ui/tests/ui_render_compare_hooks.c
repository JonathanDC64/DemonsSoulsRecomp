/* ui_render_compare's counters: each D3D12 command list call a renderer records, counted for the renderer g_urc_who
 * names, then made as it was. The calls are patched in the list's method table, which every list of a device shares
 * (D3D12 makes them all of one class), so the lists the renderers make themselves count too. In C, for the table's
 * named members. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "ui_render_compare_hooks.h"

int g_urc_who = -1;
unsigned long long g_urc_count[URC_RENDERERS][URC_COUNT];
const char* const g_urc_name[URC_COUNT] = {
    "Reset", "Close",
    "DrawInstanced", "DrawIndexedInstanced",
    "SetPipelineState", "SetGraphicsRootSignature", "SetDescriptorHeaps",
    "SetGraphicsRootConstantBufferView", "SetGraphicsRootDescriptorTable", "IASetPrimitiveTopology",
    "IASetVertexBuffers", "IASetIndexBuffer",
    "OMSetRenderTargets", "OMSetBlendFactor", "OMSetStencilRef", "RSSetViewports", "RSSetScissorRects",
    "ResourceBarrier", "(barriers)",
    "ClearRenderTargetView", "ClearDepthStencilView",
    "CopyTextureRegion", "CopyResource", "CopyBufferRegion", "ResolveSubresource",
};

static ID3D12GraphicsCommandListVtbl g_orig;
static ID3D12GraphicsCommandListVtbl* g_table;

static void count(int k, unsigned n)
{
    if (g_urc_who >= 0 && g_urc_who < URC_RENDERERS) g_urc_count[g_urc_who][k] += n;
}

static HRESULT STDMETHODCALLTYPE h_Close(ID3D12GraphicsCommandList* t)
{
    count(URC_CLOSE, 1);
    return g_orig.Close(t);
}

static HRESULT STDMETHODCALLTYPE h_Reset(ID3D12GraphicsCommandList* t, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    count(URC_RESET, 1);
    return g_orig.Reset(t, a, p);
}

static void STDMETHODCALLTYPE h_DrawInstanced(ID3D12GraphicsCommandList* t, UINT a, UINT b, UINT c, UINT d)
{
    count(URC_DRAW, 1);
    g_orig.DrawInstanced(t, a, b, c, d);
}

static void STDMETHODCALLTYPE h_DrawIndexedInstanced(ID3D12GraphicsCommandList* t, UINT a, UINT b, UINT c, INT d, UINT e)
{
    count(URC_DRAW_INDEXED, 1);
    g_orig.DrawIndexedInstanced(t, a, b, c, d, e);
}

static void STDMETHODCALLTYPE h_SetPipelineState(ID3D12GraphicsCommandList* t, ID3D12PipelineState* p)
{
    count(URC_SET_PIPELINE, 1);
    g_orig.SetPipelineState(t, p);
}

static void STDMETHODCALLTYPE h_SetGraphicsRootSignature(ID3D12GraphicsCommandList* t, ID3D12RootSignature* r)
{
    count(URC_SET_ROOT_SIGNATURE, 1);
    g_orig.SetGraphicsRootSignature(t, r);
}

static void STDMETHODCALLTYPE h_SetDescriptorHeaps(ID3D12GraphicsCommandList* t, UINT n, ID3D12DescriptorHeap* const* h)
{
    count(URC_SET_HEAPS, 1);
    g_orig.SetDescriptorHeaps(t, n, h);
}

static void STDMETHODCALLTYPE h_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList* t, UINT i,
                                                                   D3D12_GPU_VIRTUAL_ADDRESS a)
{
    count(URC_SET_CBV, 1);
    g_orig.SetGraphicsRootConstantBufferView(t, i, a);
}

static void STDMETHODCALLTYPE h_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* t, UINT i,
                                                                D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    count(URC_SET_TABLE, 1);
    g_orig.SetGraphicsRootDescriptorTable(t, i, h);
}

static void STDMETHODCALLTYPE h_IASetPrimitiveTopology(ID3D12GraphicsCommandList* t, D3D12_PRIMITIVE_TOPOLOGY p)
{
    count(URC_SET_TOPOLOGY, 1);
    g_orig.IASetPrimitiveTopology(t, p);
}

static void STDMETHODCALLTYPE h_IASetVertexBuffers(ID3D12GraphicsCommandList* t, UINT s, UINT n,
                                                   const D3D12_VERTEX_BUFFER_VIEW* v)
{
    count(URC_SET_VB, 1);
    g_orig.IASetVertexBuffers(t, s, n, v);
}

static void STDMETHODCALLTYPE h_IASetIndexBuffer(ID3D12GraphicsCommandList* t, const D3D12_INDEX_BUFFER_VIEW* v)
{
    count(URC_SET_IB, 1);
    g_orig.IASetIndexBuffer(t, v);
}

static void STDMETHODCALLTYPE h_OMSetRenderTargets(ID3D12GraphicsCommandList* t, UINT n,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL single,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE* ds)
{
    count(URC_SET_TARGETS, 1);
    g_orig.OMSetRenderTargets(t, n, rt, single, ds);
}

static void STDMETHODCALLTYPE h_OMSetBlendFactor(ID3D12GraphicsCommandList* t, const FLOAT f[4])
{
    count(URC_SET_BLEND_FACTOR, 1);
    g_orig.OMSetBlendFactor(t, f);
}

static void STDMETHODCALLTYPE h_OMSetStencilRef(ID3D12GraphicsCommandList* t, UINT r)
{
    count(URC_SET_STENCIL_REF, 1);
    g_orig.OMSetStencilRef(t, r);
}

static void STDMETHODCALLTYPE h_RSSetViewports(ID3D12GraphicsCommandList* t, UINT n, const D3D12_VIEWPORT* v)
{
    count(URC_SET_VIEWPORTS, 1);
    g_orig.RSSetViewports(t, n, v);
}

static void STDMETHODCALLTYPE h_RSSetScissorRects(ID3D12GraphicsCommandList* t, UINT n, const D3D12_RECT* r)
{
    count(URC_SET_SCISSORS, 1);
    g_orig.RSSetScissorRects(t, n, r);
}

static void STDMETHODCALLTYPE h_ResourceBarrier(ID3D12GraphicsCommandList* t, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    count(URC_BARRIER_CALLS, 1);
    count(URC_BARRIERS, n);
    g_orig.ResourceBarrier(t, n, b);
}

static void STDMETHODCALLTYPE h_ClearRenderTargetView(ID3D12GraphicsCommandList* t, D3D12_CPU_DESCRIPTOR_HANDLE v,
                                                      const FLOAT c[4], UINT n, const D3D12_RECT* r)
{
    count(URC_CLEAR_RTV, 1);
    g_orig.ClearRenderTargetView(t, v, c, n, r);
}

static void STDMETHODCALLTYPE h_ClearDepthStencilView(ID3D12GraphicsCommandList* t, D3D12_CPU_DESCRIPTOR_HANDLE v,
                                                      D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* r)
{
    count(URC_CLEAR_DSV, 1);
    g_orig.ClearDepthStencilView(t, v, f, d, s, n, r);
}

static void STDMETHODCALLTYPE h_CopyTextureRegion(ID3D12GraphicsCommandList* t, const D3D12_TEXTURE_COPY_LOCATION* d,
                                                  UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s,
                                                  const D3D12_BOX* b)
{
    count(URC_COPY_TEXTURE, 1);
    g_orig.CopyTextureRegion(t, d, x, y, z, s, b);
}

static void STDMETHODCALLTYPE h_CopyResource(ID3D12GraphicsCommandList* t, ID3D12Resource* d, ID3D12Resource* s)
{
    count(URC_COPY_RESOURCE, 1);
    g_orig.CopyResource(t, d, s);
}

static void STDMETHODCALLTYPE h_CopyBufferRegion(ID3D12GraphicsCommandList* t, ID3D12Resource* d, UINT64 dof,
                                                 ID3D12Resource* s, UINT64 sof, UINT64 n)
{
    count(URC_COPY_BUFFER, 1);
    g_orig.CopyBufferRegion(t, d, dof, s, sof, n);
}

static void STDMETHODCALLTYPE h_ResolveSubresource(ID3D12GraphicsCommandList* t, ID3D12Resource* d, UINT ds,
                                                   ID3D12Resource* s, UINT ss, DXGI_FORMAT f)
{
    count(URC_RESOLVE, 1);
    g_orig.ResolveSubresource(t, d, ds, s, ss, f);
}

static void patch(ID3D12GraphicsCommandListVtbl* vt, const ID3D12GraphicsCommandListVtbl* with)
{
    DWORD old = 0, unused = 0;
    if (!VirtualProtect(vt, sizeof *vt, PAGE_READWRITE, &old)) return;
    *vt = *with;
    VirtualProtect(vt, sizeof *vt, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), vt, sizeof *vt);
}

int urc_hooks_install(void* list)
{
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)list;
    if (!cl || g_table) return g_table && cl && cl->lpVtbl == g_table ? 0 : -1;
    g_table = cl->lpVtbl;
    g_orig = *g_table;
    ID3D12GraphicsCommandListVtbl h = g_orig;
    h.Close = h_Close;
    h.Reset = h_Reset;
    h.DrawInstanced = h_DrawInstanced;
    h.DrawIndexedInstanced = h_DrawIndexedInstanced;
    h.SetPipelineState = h_SetPipelineState;
    h.SetGraphicsRootSignature = h_SetGraphicsRootSignature;
    h.SetDescriptorHeaps = h_SetDescriptorHeaps;
    h.SetGraphicsRootConstantBufferView = h_SetGraphicsRootConstantBufferView;
    h.SetGraphicsRootDescriptorTable = h_SetGraphicsRootDescriptorTable;
    h.IASetPrimitiveTopology = h_IASetPrimitiveTopology;
    h.IASetVertexBuffers = h_IASetVertexBuffers;
    h.IASetIndexBuffer = h_IASetIndexBuffer;
    h.OMSetRenderTargets = h_OMSetRenderTargets;
    h.OMSetBlendFactor = h_OMSetBlendFactor;
    h.OMSetStencilRef = h_OMSetStencilRef;
    h.RSSetViewports = h_RSSetViewports;
    h.RSSetScissorRects = h_RSSetScissorRects;
    h.ResourceBarrier = h_ResourceBarrier;
    h.ClearRenderTargetView = h_ClearRenderTargetView;
    h.ClearDepthStencilView = h_ClearDepthStencilView;
    h.CopyTextureRegion = h_CopyTextureRegion;
    h.CopyResource = h_CopyResource;
    h.CopyBufferRegion = h_CopyBufferRegion;
    h.ResolveSubresource = h_ResolveSubresource;
    patch(g_table, &h);
    return g_table->Close == h_Close ? 0 : -1;
}

void urc_hooks_remove(void)
{
    if (!g_table) return;
    patch(g_table, &g_orig);
    g_table = NULL;
}
