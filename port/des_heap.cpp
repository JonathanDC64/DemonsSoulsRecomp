/* Demon's Souls — clean host-backed guest allocator (real free-list).
 *
 * The game's FromSoftware "DL" system heap can't be backed in our environment
 * (liblv2.sprx prx_start doesn't run), so we redirect the game's malloc family
 * to this allocator via the lifter's --override-funcs. Plug-and-play: our own
 * code, no firmware/keys.
 *
 * v2 — proper free-list allocator (was a leaking bump allocator). The game runs
 * its own memory-pool / free-list management on top of malloc'd blocks; a bump
 * allocator that never reclaimed filled its region and the game's free-list
 * walker ran off the end into uninitialized memory and span forever. This
 * version reclaims + coalesces on free, so the working set stays bounded.
 *
 * Region: 0x60000000..0xC0000000 (1.5 GB), demand-paged by the flat VM. Chosen
 * to NOT collide with: the game image (<0x1850000), TLS (0x10F00000), the lv2
 * mmapper (0x11000000+), sys_memory_allocate HLE (0x40000000-0x50000000), or
 * thread stacks (0xD0000000+). Metadata is host-side (no in-band headers), so
 * the game can never corrupt it and there's no header/alignment interference.
 */
#include "ps3emu/ppu_lift.h"   /* ppu_context */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <mutex>
#include <utility>

extern "C" uint8_t* vm_base; /* host base of the flat guest address space */

namespace {

constexpr uint32_t HBASE = 0x60000000u;
constexpr uint32_t HEND  = 0xC0000000u;     /* 1.5 GB */
constexpr uint32_t ALIGNB = 16u;
constexpr uint32_t MIN_SPLIT = 32u;

std::mutex g_mtx;
std::map<uint32_t,uint32_t>            g_free_addr;  /* addr  -> size (sorted; coalescing) */
std::multimap<uint32_t,uint32_t>       g_free_size;  /* size  -> addr (best-fit search)    */
std::unordered_map<uint32_t,std::pair<uint32_t,uint32_t>> g_used; /* userptr -> (block,size) */
bool g_init = false;
uint64_t g_peak = 0, g_cur = 0;

inline uint32_t alnup(uint32_t v, uint32_t a) { return (v + (a - 1)) & ~(a - 1); }

void add_free(uint32_t a, uint32_t s) { g_free_addr[a] = s; g_free_size.insert({s, a}); }
void rm_free(uint32_t a, uint32_t s) {
    g_free_addr.erase(a);
    auto r = g_free_size.equal_range(s);
    for (auto it = r.first; it != r.second; ++it)
        if (it->second == a) { g_free_size.erase(it); break; }
}
void ensure_init() { if (!g_init) { g_init = true; add_free(HBASE, HEND - HBASE); } }

/* Carve a block of at least `need` bytes. Returns (block_addr, block_size) or (0,0). */
std::pair<uint32_t,uint32_t> carve(uint32_t need) {
    ensure_init();
    need = alnup(need < 16u ? 16u : need, ALIGNB);
    auto it = g_free_size.lower_bound(need);
    if (it == g_free_size.end()) {
        fprintf(stderr, "[ds_heap] OUT OF MEMORY need=0x%X cur=0x%llX peak=0x%llX used=%zu\n",
                need, (unsigned long long)g_cur, (unsigned long long)g_peak, g_used.size());
        return {0u, 0u};
    }
    uint32_t size = it->first, addr = it->second;
    rm_free(addr, size);
    uint32_t blk;
    if (size - need >= MIN_SPLIT) { add_free(addr + need, size - need); blk = need; }
    else                         { blk = size; }
    g_cur += blk; if (g_cur > g_peak) g_peak = g_cur;
    return {addr, blk};
}

void release(uint32_t addr, uint32_t size) {
    g_cur -= size;
    /* coalesce with the following free block */
    auto nx = g_free_addr.find(addr + size);
    if (nx != g_free_addr.end()) { uint32_t ns = nx->second; rm_free(addr + size, ns); size += ns; }
    /* coalesce with the preceding free block */
    auto it = g_free_addr.lower_bound(addr);
    if (it != g_free_addr.begin()) {
        --it;
        if (it->first + it->second == addr) { uint32_t pa = it->first, ps = it->second; rm_free(pa, ps); addr = pa; size += ps; }
    }
    add_free(addr, size);
}

} // namespace

extern "C" {

/* malloc(size) -> ptr   (r3 = size) */
void ds_malloc(ppu_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto pr = carve((uint32_t)ctx->gpr[3]);
    if (pr.first) g_used[pr.first] = pr;
    ctx->gpr[3] = pr.first;
}

/* free(ptr)   (r3 = ptr) */
void ds_free(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_used.find(p);
    if (it == g_used.end()) return;          /* not ours / double-free: ignore */
    uint32_t blk = it->second.first, sz = it->second.second;
    g_used.erase(it);
    release(blk, sz);
}

/* memalign(align, size) -> ptr   (r3 = align, r4 = size) */
void ds_memalign(ppu_context* ctx) {
    uint32_t align = (uint32_t)ctx->gpr[3], size = (uint32_t)ctx->gpr[4];
    if (align < ALIGNB) align = ALIGNB;
    std::lock_guard<std::mutex> lk(g_mtx);
    auto pr = carve(size + align);
    if (!pr.first) { ctx->gpr[3] = 0; return; }
    uint32_t aligned = alnup(pr.first, align);
    g_used[aligned] = pr;                     /* user ptr = aligned; block = pr.first */
    ctx->gpr[3] = aligned;
}

/* calloc(count, size) -> zeroed ptr   (r3 = count, r4 = size) */
void ds_calloc(ppu_context* ctx) {
    uint64_t tot = (uint64_t)(uint32_t)ctx->gpr[3] * (uint32_t)ctx->gpr[4];
    if (tot > 0xFFFFFFFFull) tot = 0xFFFFFFFFull;
    std::lock_guard<std::mutex> lk(g_mtx);
    auto pr = carve((uint32_t)tot);
    if (pr.first) { g_used[pr.first] = pr; memset(vm_base + pr.first, 0, (size_t)tot); }
    ctx->gpr[3] = pr.first;
}

/* realloc(ptr, size) -> ptr   (r3 = ptr, r4 = size) */
void ds_realloc(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3], n = (uint32_t)ctx->gpr[4];
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!p) { auto pr = carve(n); if (pr.first) g_used[pr.first] = pr; ctx->gpr[3] = pr.first; return; }
    auto it = g_used.find(p);
    if (it == g_used.end()) { ctx->gpr[3] = 0; return; }
    uint32_t blk = it->second.first, bs = it->second.second;
    uint32_t usable = bs - (p - blk);
    if (n <= usable) { ctx->gpr[3] = p; return; }   /* fits in place */
    auto pr = carve(n);
    if (!pr.first) { ctx->gpr[3] = 0; return; }
    memcpy(vm_base + pr.first, vm_base + p, usable < n ? usable : n);
    g_used.erase(it); release(blk, bs);
    g_used[pr.first] = pr;
    ctx->gpr[3] = pr.first;
}

} // extern "C"
