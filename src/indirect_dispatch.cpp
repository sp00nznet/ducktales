/* indirect_dispatch.cpp — guest→host indirect-call dispatch.
 *
 * Recompiled `bctrl`/`bctr` land here with a GUEST address in CTR. We resolve
 * it to a host function via a hash table built from the lifter's
 * function_table[], with OPD ({entry,toc}) and vtable indirection fallbacks.
 *
 * Clean DuckTales build: no game-specific intercepts, no CRT-abort longjmp,
 * no forced TOC. External registrations (import sentinels, manual stubs) are
 * layered on top of the table.
 */
#include "recomp/ppu_recomp.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

/* The lifter's native function table (generated in ppu_recomp_*.cpp). */
extern "C" const func_entry function_table[];
extern "C" const uint64_t   function_table_count;

extern "C" uint8_t* vm_base;
extern "C" uint32_t vm_read32(uint64_t addr);

/* Firmware-import wiring (import_table.cpp): each import has a sentinel guest
 * address (0x3F000000+i) and a synthesized OPD (0x3E000000+i*8). Both are
 * handled with an O(1) range check rather than the hash table — the sentinels
 * are densely packed and otherwise form long probe chains. */
extern "C" const uint32_t g_duck_import_count;
extern "C" void hle_import_trampoline(void* ctx);
#define DUCK_SENTINEL_BASE 0x3F000000u
#define DUCK_OPD_BASE      0x3E000000u

/* Thread-local trampoline chain — split-fragment fallthroughs set this and
 * return; callers drain it (DRAIN_TRAMPOLINE in the lifted code, and here). */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;

/* ---------------------------------------------------------------------------
 * Open-addressing hash table: guest_addr -> host func
 * -----------------------------------------------------------------------*/
#define DISPATCH_SIZE  (1 << 17)   /* 131072 slots for ~50K funcs (<40% load) */
#define DISPATCH_MASK  (DISPATCH_SIZE - 1)

typedef struct { uint32_t guest_addr; void (*host_func)(void*); } DispatchEntry;
static DispatchEntry s_tab[DISPATCH_SIZE];
static int s_init = 0;

static inline uint32_t hash_addr(uint32_t a) {
    return (a >> 2) ^ (a >> 14) ^ (a >> 22);
}

static void dispatch_put(uint32_t addr, void (*fn)(void*)) {
    uint32_t h = hash_addr(addr) & DISPATCH_MASK;
    for (uint32_t j = 0; j < DISPATCH_SIZE; j++) {
        uint32_t i = (h + j) & DISPATCH_MASK;
        if (s_tab[i].guest_addr == addr || s_tab[i].guest_addr == 0) {
            s_tab[i].guest_addr = addr;
            s_tab[i].host_func  = fn;
            return;
        }
    }
}

static void (*dispatch_get(uint32_t addr))(void*) {
    uint32_t h = hash_addr(addr) & DISPATCH_MASK;
    for (uint32_t j = 0; j < 256; j++) {
        uint32_t i = (h + j) & DISPATCH_MASK;
        if (s_tab[i].guest_addr == addr) return s_tab[i].host_func;
        if (s_tab[i].guest_addr == 0)    break;
    }
    return nullptr;
}

static void dispatch_init(void) {
    if (s_init) return;
    memset(s_tab, 0, sizeof(s_tab));
    for (uint64_t i = 0; i < function_table_count; i++)
        dispatch_put((uint32_t)function_table[i].addr,
                     (void(*)(void*))function_table[i].func);
    s_init = 1;
    fprintf(stderr, "[dispatch] %llu functions indexed\n",
            (unsigned long long)function_table_count);
}

extern "C" void dispatch_register_external(uint32_t addr, void (*fn)(void*)) {
    if (!s_init) dispatch_init();
    dispatch_put(addr, fn);
}

/* ---------------------------------------------------------------------------
 * Resolve a guest CTR target to a host function, following OPD / vtable
 * indirection up to a few levels (covers C++ virtual calls through objects).
 * On a hit, applies the OPD's TOC into r2. Returns nullptr on miss.
 * -----------------------------------------------------------------------*/
static void (*resolve_target(ppu_context* ctx, uint32_t* io_target))(void*) {
    uint32_t target = *io_target;
    void (*fn)(void*) = dispatch_get(target);
    if (fn) return fn;

    if (target == 0 || target >= 0x40000000 || !vm_base) return nullptr;

    /* CTR may point to an OPD {entry, toc}. */
    uint32_t entry = vm_read32(target);
    uint32_t toc   = vm_read32(target + 4);
    fn = dispatch_get(entry);
    if (fn) {
        if (toc) ctx->gpr[2] = toc;
        *io_target = entry;
        return fn;
    }
    /* Deeper chain: object -> vtable -> method OPD -> entry. */
    uint32_t cur = entry;
    for (int level = 0; level < 3; level++) {
        if (cur == 0 || cur >= 0x40000000) break;
        uint32_t nxt = vm_read32(cur);
        fn = dispatch_get(nxt);
        if (fn) {
            uint32_t ntoc = vm_read32(cur + 4);
            if (ntoc) ctx->gpr[2] = ntoc;
            *io_target = nxt;
            return fn;
        }
        cur = nxt;
    }
    return nullptr;
}

static inline void drain_trampolines(ppu_context* ctx) {
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf((void*)ctx);
    }
}

extern "C" void ps3_indirect_call(ppu_context* ctx) {
    if (!s_init) dispatch_init();

    uint32_t target = (uint32_t)ctx->ctr;
    if (target == 0) { ctx->gpr[3] = 0; return; }

    /* Import sentinel called directly (via the stub thunk). */
    if (target - DUCK_SENTINEL_BASE < g_duck_import_count) {
        hle_import_trampoline(ctx);   /* reads sentinel from ctx->ctr */
        return;
    }
    /* Import OPD called directly (game loaded the OPD address and bctrl'd it):
     * the entry word is the sentinel. */
    if (target - DUCK_OPD_BASE < g_duck_import_count * 8u) {
        uint32_t entry = vm_read32(target);
        if (entry - DUCK_SENTINEL_BASE < g_duck_import_count) {
            ctx->ctr = entry;
            hle_import_trampoline(ctx);
            return;
        }
    }

    void (*fn)(void*) = resolve_target(ctx, &target);
    if (!fn) {
        static int s_miss = 0;
        if (s_miss++ < 50)
            fprintf(stderr, "[dispatch] MISS bctrl -> 0x%08X (SP=0x%08X LR=0x%08X)\n",
                    target, (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr);
        ctx->gpr[3] = 0;   /* benign CELL_OK for unresolved targets */
        return;
    }
    g_trampoline_fn = nullptr;
    fn((void*)ctx);
    drain_trampolines(ctx);
}

extern "C" void ps3_indirect_branch(ppu_context* ctx) {
    /* bctr — same resolution, no LR semantics to add here. */
    ps3_indirect_call(ctx);
}

/* Run a host function and drain its trampoline chain (entry/thread starts). */
extern "C" void ps3_trampoline_run(ppu_context* ctx, void (*fn)(void*)) {
    if (!s_init) dispatch_init();
    g_trampoline_fn = nullptr;
    fn((void*)ctx);
    drain_trampolines(ctx);
}

/* Thread entry trampoline — runtime calls this for each new PPU thread with
 * the entry address in ctx->cia. */
extern "C" void ps3_thread_entry(ppu_context* ctx) {
    if (!s_init) dispatch_init();
    uint32_t entry = (uint32_t)ctx->cia;
    void (*fn)(void*) = resolve_target(ctx, &entry);
    if (!fn) {
        fprintf(stderr, "[thread] entry 0x%08X not found\n", (uint32_t)ctx->cia);
        return;
    }
    fprintf(stderr, "[thread] %llu start @0x%08X\n",
            (unsigned long long)ctx->thread_id, entry);
    ps3_trampoline_run(ctx, fn);
    fprintf(stderr, "[thread] %llu exit (r3=0x%llX)\n",
            (unsigned long long)ctx->thread_id, (unsigned long long)ctx->gpr[3]);
}
