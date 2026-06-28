/* Demon's Souls — clean host-backed guest allocator.
 *
 * The game's FromSoftware "DL" system heap (struct at guest 0x01A1F850) is
 * statically configured but never *backed* with memory in our environment:
 * on real PS3 liblv2.sprx's prx_start sets up the libc heap before the first
 * C++ static constructor allocates, but we HLE liblv2 so that never runs ->
 * the first `operator new` hits a null heap -> std::bad_alloc -> terminate.
 *
 * Rather than reverse + replicate FromSoft's dlmalloc-style allocator (or load
 * firmware, which would break the plug-and-play / no-keys goal), we redirect the
 * game's malloc family to this clean allocator. The lifter's --override-funcs
 * replaces the chosen guest functions' bodies with a tail call to these symbols.
 *
 * Allocations come from a dedicated guest region (0x40000000..0x60000000) that
 * the flat-VM demand-pager backs on first touch. A host-side size map supports
 * realloc/calloc copying. free() currently leaks (bump allocator) — fine to get
 * boot far; a freelist can be added later if fragmentation matters.
 */
#include "ppu_recomp.h"      /* ppu_context */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

extern "C" uint8_t* vm_base; /* host base of the flat guest address space */

namespace {

constexpr uint32_t kHeapBase = 0x40000000u;
constexpr uint32_t kHeapEnd  = 0x60000000u;   /* 512 MB */
uint32_t g_next = kHeapBase;
std::unordered_map<uint32_t, uint32_t> g_sizes;   /* guest ptr -> requested size */

uint32_t heap_alloc(uint32_t size, uint32_t align)
{
    if (align < 16u) align = 16u;
    if (size == 0u)  size = 1u;
    g_next = (g_next + (align - 1u)) & ~(align - 1u);
    uint32_t p = g_next;
    g_next += (size + 15u) & ~15u;
    if (g_next >= kHeapEnd) {
        fprintf(stderr, "[ds_heap] OUT OF MEMORY (req=0x%X, used=0x%X)\n",
                size, p - kHeapBase);
        return 0u;
    }
    g_sizes[p] = size;
    return p;
}

} // namespace

extern "C" {

/* malloc(size) -> ptr   (r3 = size; return r3) */
void ds_malloc(ppu_context* ctx)
{
    ctx->gpr[3] = heap_alloc((uint32_t)ctx->gpr[3], 16u);
}

/* free(ptr)             (r3 = ptr) */
void ds_free(ppu_context* ctx)
{
    g_sizes.erase((uint32_t)ctx->gpr[3]);   /* bump allocator: space not reclaimed */
}

/* memalign(align, size) -> ptr   (r3 = align, r4 = size) */
void ds_memalign(ppu_context* ctx)
{
    ctx->gpr[3] = heap_alloc((uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[3]);
}

/* calloc(count, size) -> zeroed ptr   (r3 = count, r4 = size) */
void ds_calloc(ppu_context* ctx)
{
    uint64_t total = (uint64_t)(uint32_t)ctx->gpr[3] * (uint32_t)ctx->gpr[4];
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    uint32_t p = heap_alloc((uint32_t)total, 16u);
    if (p) memset(vm_base + p, 0, (size_t)total);
    ctx->gpr[3] = p;
}

/* realloc(ptr, size) -> ptr   (r3 = ptr, r4 = size) */
void ds_realloc(ppu_context* ctx)
{
    uint32_t old = (uint32_t)ctx->gpr[3];
    uint32_t nsz = (uint32_t)ctx->gpr[4];
    if (!old) { ctx->gpr[3] = heap_alloc(nsz, 16u); return; }
    if (!nsz) { g_sizes.erase(old); ctx->gpr[3] = 0u; return; }
    uint32_t np = heap_alloc(nsz, 16u);
    if (np) {
        auto it = g_sizes.find(old);
        uint32_t osz = (it != g_sizes.end()) ? it->second : nsz;
        memcpy(vm_base + np, vm_base + old, osz < nsz ? osz : nsz);
    }
    ctx->gpr[3] = np;
}

} // extern "C"
