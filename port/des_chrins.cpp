/* ChrInsManThread::Thread with its done flag stored before its done event.
 *
 * Once a frame the main thread hands the character step to a worker thread
 * and waits for it (0x002A60A0; also 0x002967D8). The kick, 0x002A6E28,
 * clears the worker object's done flag (+0x1C), clears its done event
 * (+0x08, a mutex/cond event), and posts a message to the worker's queue;
 * 0x002A6660 then waits on the event. The worker, 0x002A6AA8 (the thread's
 * entry), pops a message and, if the done flag is 0, runs the step
 * (0x002986B8, under the game's big lock), SETS THE EVENT, and only then
 * stores 1 to the done flag.
 *
 * Between those two stores the main thread can wake, finish its frame and
 * kick again. Its "flag = 0" then lands before the worker's "flag = 1"; the
 * worker takes the new message with the flag at 1, skips it and goes back
 * to sleep on its queue, and the main thread waits on the event for good.
 * The game keeps presenting its last frame, so it looks like a 10 fps
 * freeze that ignores the pad. On a PS3 the window is two instructions on a
 * thread that keeps running. Here the worker can lose its core right after
 * it releases the event's mutex -- the thread it woke takes it -- and at
 * 240 fps the main thread needs only about 4 ms to come back with the next
 * kick, so it happens, more often when the CPU is busy.
 *
 * This is the game's loop with that one change: the flag is stored before
 * the event is set, so a kick can only follow it. Every other step and call
 * is the game's: 0x001D17A0 / 0x001D35D8 register and unregister the
 * thread, 0x00B5D548 pops (blocking), 0x00B5CFA0 reads the message type
 * (0 step, 1 quit), 0x00B5D2F8 releases the message, 0x00023750 returns the
 * big lock (+0x08 owner thread id, -1 when free, +0x10 count, +0x14 what
 * 0x00A8CBF0 locks and 0x00A8CA48 unlocks), 0x00B495B8 is the current
 * thread's id, and 0x009E0148 sets the event. The return addresses are the
 * game's, so guest stack dumps read the same.
 *
 * The thread is only ever entered through its function pointer, so a
 * registration replaces it without a re-lift. DES_CHRINSFIX=0 keeps the
 * game's own loop. DES_CHRINSFIX=2 runs this loop in the game's order (event,
 * then flag), for A/B runs: the two modes differ only in those two stores.
 *
 * A step request that finds the flag already 1 is skipped, as the game does,
 * and logged: in the game's order that is the lost kick, the moment the main
 * thread starts waiting for good. With the flag stored first it cannot
 * happen, so a line in the default mode means another cause.
 */
#include "ppu_recomp.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {

constexpr uint32_t kChrInsThread = 0x002A6AA8u;   // ChrInsManThread::Thread(r3 worker, r4 queue)

// The worker object.
constexpr uint32_t kDoneEvent = 0x08;
constexpr uint32_t kStep      = 0x18;   // float, stored by the kick
constexpr uint32_t kDone      = 0x1C;   // byte

// The big lock.
constexpr uint32_t kLockOwner = 0x08;
constexpr uint32_t kLockCount = 0x10;
constexpr uint32_t kLockSys   = 0x14;

constexpr uint32_t kFrame = 0xC0;       // the game's stack frame for the loop

int g_mode = 1;   // 1: flag, then event; 2: event, then flag (the game's order)

// A call made the way the lifted code makes it: the guest return address in
// lr, then any tail call the callee left on the trampoline.
void call(ppu_context* ctx, void (*fn)(ppu_context*), uint32_t ret)
{
    ctx->lr = ret;
    fn(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
}

uint64_t thread_id(ppu_context* ctx, uint32_t ret)
{
    call(ctx, func_00B495B8, ret);
    return ctx->gpr[3];
}

// The big lock, and whether this thread owns it: the owner is read before the
// thread id is asked for, as the game does.
uint32_t big_lock(ppu_context* ctx, uint32_t ret_lock, uint32_t ret_id, bool* mine)
{
    call(ctx, func_00023750, ret_lock);
    const uint32_t lock = (uint32_t)ctx->gpr[3];
    const uint64_t owner = vm_read64(lock + kLockOwner);
    ctx->gpr[31] = lock;
    ctx->gpr[29] = owner;
    *mine = owner == thread_id(ctx, ret_id);
    return lock;
}

// The step, as 0x002A6B44..0x002A6CAC run it.
void step(ppu_context* ctx, uint32_t obj, uint32_t toc)
{
    auto run = [&](uint32_t ret) {
        const uint32_t who = vm_read32(toc - 0x7FB8);
        const uint32_t bits = vm_read32(obj + kStep);
        float f;
        std::memcpy(&f, &bits, 4);
        ctx->fpr[1] = f;
        ctx->gpr[3] = vm_read32(who);
        call(ctx, func_002986B8, ret);
    };
    // Leave one level of the big lock; the last level frees it.
    auto leave = [&](uint32_t lock) {
        vm_write32(lock + kLockCount, vm_read32(lock + kLockCount) - 1);
        if (vm_read32(lock + kLockCount) != 0) return;
        vm_write64(lock + kLockOwner, ~0ull);
        ctx->gpr[3] = lock + kLockSys;
        call(ctx, func_00A8CA48, 0x002A6CA8u);
    };

    bool mine = false;
    uint32_t lock = big_lock(ctx, 0x002A6B50u, 0x002A6B60u, &mine);
    if (!mine) {
        ctx->gpr[3] = lock + kLockSys;
        ctx->gpr[4] = 0;
        call(ctx, func_00A8CBF0, 0x002A6B7Cu);
        if ((int32_t)ctx->gpr[3] == 0) {
            vm_write64(lock + kLockOwner, thread_id(ctx, 0x002A6C34u));
            mine = true;
        }
    }
    if (mine) {
        vm_write32(lock + kLockCount, vm_read32(lock + kLockCount) + 1);
        run(0x002A6C58u);
        lock = big_lock(ctx, 0x002A6C60u, 0x002A6C70u, &mine);
    } else {
        // 0x00A8CBF0 failed: the game steps anyway, without the lock.
        run(0x002A6B98u);
        lock = big_lock(ctx, 0x002A6BA0u, 0x002A6BB0u, &mine);
    }
    if (mine) leave(lock);
}

// A step request with the done flag already 1: the game skips it.
void skipped(uint32_t obj)
{
    static std::atomic<unsigned> count{0};
    const unsigned n = ++count;
    if (n > 4) return;
    std::fprintf(stderr, "[des] ChrIns: a step request found the done flag already 1 and was skipped "
                 "(worker 0x%08X, skip %u)%s\n", obj, n,
                 g_mode == 2 ? ": the lost kick, the main thread now waits on the done event for good"
                             : ", unexpected with the flag stored first");
}

void chrins_thread(ppu_context* ctx)
{
    DS_FRAME(kChrInsThread);
    uint64_t saved[9];
    for (int r = 23; r < 32; r++) saved[r - 23] = ctx->gpr[r];
    const uint64_t ret = ctx->lr;
    const uint64_t sp = ctx->gpr[1];
    ctx->gpr[1] = sp - kFrame;
    vm_write64(ctx->gpr[1], sp);                  // back chain
    vm_write64(ctx->gpr[1] + kFrame + 0x10, ret); // lr, in the caller's frame
    for (int r = 23; r < 32; r++) vm_write64(ctx->gpr[1] + 0x78 + 8 * (r - 23), saved[r - 23]);

    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const uint32_t queue = (uint32_t)ctx->gpr[4];
    const uint32_t toc = vm_read32(ctx->gpr[2] - 0x7AE0);
    const uint32_t reg = vm_read32(toc - 0x7FBC);
    ctx->gpr[23] = reg;
    ctx->gpr[24] = ctx->gpr[25] = obj;
    ctx->gpr[27] = queue;
    ctx->gpr[30] = toc;

    ctx->gpr[3] = vm_read32(reg);
    call(ctx, func_001D17A0, 0x002A6AF4u);
    uint32_t msg = 0;
    for (;;) {
        ctx->gpr[3] = queue;
        ctx->gpr[4] = 1;
        call(ctx, func_00B5D548, 0x002A6B24u);
        msg = (uint32_t)ctx->gpr[3];
        ctx->gpr[28] = ctx->gpr[3] = msg;
        call(ctx, func_00B5CFA0, 0x002A6B34u);
        const int32_t type = (int32_t)ctx->gpr[3];
        if (type == 1) break;
        if (type == 0 && vm_read8(obj + kDone) == 0) {
            step(ctx, obj, toc);
            if (g_mode != 2) vm_write8(obj + kDone, 1);   // before the event: the kick that follows sees it
            ctx->gpr[3] = obj + kDoneEvent;
            ctx->gpr[4] = 1;
            call(ctx, func_009E0148, 0x002A6BD0u);
            if (g_mode == 2) vm_write8(obj + kDone, 1);
        } else if (type == 0) {
            skipped(obj);
        }
        ctx->gpr[3] = queue;
        ctx->gpr[4] = msg;
        call(ctx, func_00B5D2F8, 0x002A6B14u);
    }
    ctx->gpr[3] = queue;
    ctx->gpr[4] = msg;
    call(ctx, func_00B5D2F8, 0x002A6BECu);
    ctx->gpr[3] = vm_read32(reg);
    call(ctx, func_001D35D8, 0x002A6BF8u);

    for (int r = 23; r < 32; r++) ctx->gpr[r] = saved[r - 23];
    ctx->gpr[1] = sp;
    ctx->lr = ret;
}

} // namespace

extern "C" void des_chrins_init(void)
{
    const char* v = std::getenv("DES_CHRINSFIX");
    const int mode = v && *v ? std::atoi(v) : 1;
    if (mode == 0) {
        std::fprintf(stderr, "[des] DES_CHRINSFIX=0: the game's own ChrIns worker loop\n");
        return;
    }
    g_mode = mode == 2 ? 2 : 1;
    if (g_mode == 2)
        std::fprintf(stderr, "[des] DES_CHRINSFIX=2: the ChrIns worker in the game's order (done event, then done flag), "
                     "skipped step requests logged\n");
    ppu_register_function(kChrInsThread, chrins_thread);
}
