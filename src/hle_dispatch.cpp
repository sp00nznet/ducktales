/* hle_dispatch.cpp — firmware-import (NID) dispatch for DuckTales.
 *
 * Each PS3 firmware import was wired (see import_table.cpp) so that calling its
 * stub lands on a unique sentinel guest address registered to
 * hle_import_trampoline. Here we recover the NID from the sentinel and route:
 *
 *   1. a ppu_context bridge registered in a ps3_module  ->  call it, or
 *   2. nothing registered                              ->  log once + CELL_OK.
 *
 * The runtime's libs/ provide typed-C implementations but do NOT self-register,
 * so ps3_resolve_func_nid only ever returns ppu_context-typed handlers we add
 * here — safe to call directly. Bridges are added incrementally as the boot
 * trace shows which imports actually gate progress.
 */
#include "recomp/ppu_recomp.h"
#include "ps3emu/module.h"
#include "ps3emu/nid.h"
#include "config.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uint32_t    duck_import_nid_for_sentinel(uint32_t sentinel);
extern "C" const char* duck_import_name_for_sentinel(uint32_t sentinel);

extern "C" uint8_t* vm_base;
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write64(uint64_t addr, uint64_t val);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);
extern "C" void     vm_write8 (uint64_t addr, uint8_t  val);

/* ppu_context HLE handler type. */
typedef void (*hle_fn)(ppu_context*);

/* ===========================================================================
 * Critical sysPrxForUser bridges (ppu_context ABI). These must be REAL for the
 * boot to make progress: TLS setup, and especially real PPU threads so the
 * main thread's worker-wait loop is actually satisfied.
 *
 * Bridges return int64_t and set r3 themselves (matching nid_dispatch, which
 * adapts via hle_fn — see register block). Thread create/exit delegate to the
 * runtime's real implementations (sys_ppu_thread.c); the entry OPD they receive
 * is resolved by ps3_thread_entry when the host thread starts.
 * ===========================================================================*/
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);   /* runtime */
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);     /* runtime */

static void bridge_sys_initialize_tls(ppu_context* ctx)
{
    /* Real ABI: sys_initialize_tls(u64 thread_id, u32 tls_addr, u32 filesz,
     * u32 memsz) — r3 is the thread_id, NOT the template address. Rather than
     * trust the args, drive from the ELF's PT_TLS (authoritative + fixed). */
    static uint32_t s_tls_next = 0x0F000000;   /* per-thread guest TLS arena */
    const uint32_t filesz = DUCK_TLS_FILESZ;
    const uint32_t memsz  = DUCK_TLS_MEMSZ;

    uint32_t base = (s_tls_next + 0xF) & ~0xFu;
    memcpy(vm_base + base, vm_base + DUCK_TLS_VADDR, filesz);
    if (memsz > filesz) memset(vm_base + base + filesz, 0, memsz - filesz);
    s_tls_next = base + 0x8000 + memsz;        /* 0x7000 TP bias + headroom */

    ctx->gpr[13] = base + 0x7000;              /* PPC64 TLS ABI: r13 = tls + 0x7000 */
    fprintf(stderr, "[HLE] sys_initialize_tls(r3=0x%llX r4=0x%llX r5=0x%llX) -> tls=0x%08X r13=0x%llX\n",
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
            (unsigned long long)ctx->gpr[5], base, (unsigned long long)ctx->gpr[13]);
    ctx->gpr[3] = 0;
}

static void bridge_sys_ppu_thread_create(ppu_context* ctx)
{
    fprintf(stderr, "[HLE] sys_ppu_thread_create(entry=0x%llX arg=0x%llX prio=%d stack=0x%X)\n",
            (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5],
            (int32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);
    ctx->gpr[3] = (uint64_t)sys_ppu_thread_create(ctx);   /* runtime spawns host thread */
}

static void bridge_sys_ppu_thread_exit(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)sys_ppu_thread_exit(ctx);
}

static void bridge_sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t ptr = (uint32_t)ctx->gpr[3];
    if (ptr) vm_write64(ptr, ctx->thread_id ? ctx->thread_id : 0x10000);
    ctx->gpr[3] = 0;
}

static void bridge_sys_time_get_system_time(ppu_context* ctx)
{
    /* Monotonic microseconds, advanced ~1 frame per call so timeouts make
     * progress. (Real wall-clock can come later if pacing matters.) */
    static uint64_t t = 1000000;
    t += 16667;
    ctx->gpr[3] = t;
}

/* ---------------------------------------------------------------------------
 * sys_lwmutex — real mutual exclusion via a host CRITICAL_SECTION keyed by the
 * guest lwmutex address. The no-op stubs gave no real synchronization between
 * the main thread and worker threads (e.g. TmpMsgPump), so producer/consumer
 * handoffs never coordinated. CRITICAL_SECTION is recursive (matches PS3
 * recursive lwmutex) and EnterCriticalSection blocks until acquired.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
struct LwMutex { uint32_t addr; CRITICAL_SECTION cs; };
static LwMutex      g_lwm[512];
static int          g_lwm_count = 0;
static CRITICAL_SECTION g_lwm_registry_lock;
static bool         g_lwm_registry_init = false;

static CRITICAL_SECTION* lwm_get(uint32_t addr, bool create)
{
    if (!g_lwm_registry_init) { InitializeCriticalSection(&g_lwm_registry_lock); g_lwm_registry_init = true; }
    EnterCriticalSection(&g_lwm_registry_lock);
    CRITICAL_SECTION* found = nullptr;
    for (int i = 0; i < g_lwm_count; i++)
        if (g_lwm[i].addr == addr) { found = &g_lwm[i].cs; break; }
    if (!found && create && g_lwm_count < 512) {
        LwMutex* m = &g_lwm[g_lwm_count++];
        m->addr = addr;
        InitializeCriticalSection(&m->cs);
        found = &m->cs;
    }
    LeaveCriticalSection(&g_lwm_registry_lock);
    return found;
}

static void bridge_sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t addr = (uint32_t)ctx->gpr[3];
    if (addr) lwm_get(addr, true);
    ctx->gpr[3] = 0;
}
static void bridge_sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t addr = (uint32_t)ctx->gpr[3];
    CRITICAL_SECTION* cs = lwm_get(addr, true);  /* lazy-create if unseen */
    if (cs) EnterCriticalSection(cs);
    ctx->gpr[3] = 0;
}
static void bridge_sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t addr = (uint32_t)ctx->gpr[3];
    CRITICAL_SECTION* cs = lwm_get(addr, true);
    bool ok = cs && TryEnterCriticalSection(cs);
    ctx->gpr[3] = ok ? 0 : (uint64_t)(int64_t)(int32_t)0x80010005; /* EBUSY */
}
static void bridge_sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t addr = (uint32_t)ctx->gpr[3];
    CRITICAL_SECTION* cs = lwm_get(addr, false);
    if (cs) LeaveCriticalSection(cs);
    ctx->gpr[3] = 0;
}
static void bridge_sys_lwmutex_destroy(ppu_context* ctx)
{
    ctx->gpr[3] = 0;   /* leave the CS allocated; cheap and avoids races */
}
#endif

/* ---------------------------------------------------------------------------
 * cellGame — boot/content checks. cellGameBootCheck writes type/attributes/
 * size/dirName, and the CRT init (func_00270448) branches on `attributes`;
 * a CELL_OK-but-write-nothing stub leaves garbage there and the init skips
 * heap creation. Provide real disc-boot values (verified against RPCS3).
 * -----------------------------------------------------------------------*/
#define CELL_GAME_GAMETYPE_DISC   1
#define CELL_GAME_SIZEKB_NOTCALC  (-1)

static void bridge_cellGameBootCheck(ppu_context* ctx)
{
    uint32_t type_p = (uint32_t)ctx->gpr[3];
    uint32_t attr_p = (uint32_t)ctx->gpr[4];
    uint32_t size_p = (uint32_t)ctx->gpr[5];
    uint32_t dir_p  = (uint32_t)ctx->gpr[6];
    if (type_p) vm_write32(type_p, CELL_GAME_GAMETYPE_DISC);
    if (attr_p) vm_write32(attr_p, 0);                       /* no attributes */
    if (size_p) {                                            /* CellGameContentSize */
        vm_write32(size_p + 0, 0x1000000);                  /* hddFreeSizeKB (~16 GB) */
        vm_write32(size_p + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_p + 8, 0);                          /* sysSizeKB */
    }
    if (dir_p) vm_write8(dir_p, 0);                          /* disc: empty dirName */
    fprintf(stderr, "[HLE] cellGameBootCheck -> DISC (type=1, attr=0)\n");
    ctx->gpr[3] = 0;
}

static void bridge_cellGameContentPermit(ppu_context* ctx)
{
    /* contentInfoPath (r3), usrdirPath (r4) — fill with the disc paths. */
    uint32_t ci_p = (uint32_t)ctx->gpr[3];
    uint32_t ud_p = (uint32_t)ctx->gpr[4];
    const char* ci = "/dev_bdvd/PS3_GAME";
    const char* ud = "/dev_bdvd/PS3_GAME/USRDIR";
    if (ci_p) for (const char* s = ci; ; s++) { vm_write8(ci_p++, (uint8_t)*s); if (!*s) break; }
    if (ud_p) for (const char* s = ud; ; s++) { vm_write8(ud_p++, (uint8_t)*s); if (!*s) break; }
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Generic stub: log the first few calls per NID, hand back CELL_OK.
 * -----------------------------------------------------------------------*/
static void hle_log_stub(ppu_context* ctx, uint32_t nid, const char* name)
{
    /* Tiny dedup ring so a hot import doesn't flood the log. */
    static uint32_t seen[256];
    static int seen_n = 0;
    bool first = true;
    for (int i = 0; i < seen_n; i++) if (seen[i] == nid) { first = false; break; }
    if (first && seen_n < 256) seen[seen_n++] = nid;
    if (first)
        fprintf(stderr, "[HLE-STUB] %s (nid=0x%08X) -> CELL_OK\n", name, nid);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * NID dispatch. Saves the caller TOC to the ABI slot (sp+0x28) like a real
 * PLT stub would, resolves a registered bridge, else falls to the log stub.
 * -----------------------------------------------------------------------*/
extern "C" void vm_write64(uint64_t addr, uint64_t val);

static void nid_dispatch(ppu_context* ctx, uint32_t nid, const char* name)
{
    /* TOC (r2) is fully managed by the lifted import thunk: it saves the
     * caller's r2 to sp+0x28 BEFORE setting r2 = OPD.toc and invoking us, then
     * restores r2 from sp+0x28 afterwards. We must NOT touch r2 or sp+0x28 — by
     * the time we run, r2 is already the (synthetic, 0) OPD toc, so saving it
     * would clobber the thunk's correct save with 0 and zero out the caller's
     * TOC. The HLE handler is host code and never needs r2. */
    hle_fn fn = (hle_fn)ps3_resolve_func_nid(nid);
    if (fn) fn(ctx);
    else    hle_log_stub(ctx, nid, name);
}

/* Entry point all import sentinels resolve to (registered in import_table). */
extern "C" void hle_import_trampoline(void* vctx)
{
    ppu_context* ctx = (ppu_context*)vctx;
    uint32_t sentinel = (uint32_t)ctx->ctr;
    uint32_t nid = duck_import_nid_for_sentinel(sentinel);
    const char* name = duck_import_name_for_sentinel(sentinel);
    if (!nid) {
        fprintf(stderr, "[HLE] unknown sentinel 0x%08X\n", sentinel);
        ctx->gpr[3] = 0;
        return;
    }
    nid_dispatch(ctx, nid, name);
}

/* ---------------------------------------------------------------------------
 * Module registration. Empty for the first build — every import flows through
 * the log stub so the boot trace reveals what to implement first. Real
 * ppu_context bridges (sysPrxForUser TLS/thread/mutex, etc.) get added to this
 * module as the trace demands them.
 * -----------------------------------------------------------------------*/
static ps3_module mod_duck;

static void reg(const char* name, void* fn)
{
    ps3_nid_table_add(&mod_duck.func_table, ps3_compute_nid(name), name, fn);
}

extern "C" void duck_register_hle_modules(void)
{
    ps3_module_init(&mod_duck, "duck_hle");

    /* Minimum for the boot to advance past its worker-wait spin: TLS + real
     * PPU threads + a moving clock. Everything else still hits the log stub. */
    reg("sys_initialize_tls",       (void*)bridge_sys_initialize_tls);
    reg("sys_ppu_thread_create",    (void*)bridge_sys_ppu_thread_create);
    reg("sys_ppu_thread_exit",      (void*)bridge_sys_ppu_thread_exit);
    reg("sys_ppu_thread_get_id",    (void*)bridge_sys_ppu_thread_get_id);
    reg("sys_time_get_system_time", (void*)bridge_sys_time_get_system_time);

    /* cellGame — real boot-check values so the CRT init reaches heap creation. */
    reg("cellGameBootCheck",        (void*)bridge_cellGameBootCheck);
    reg("cellGameContentPermit",    (void*)bridge_cellGameContentPermit);

#ifdef _WIN32
    /* Real lwmutex (host CRITICAL_SECTION) so the main thread and worker
     * threads actually synchronize. */
    reg("sys_lwmutex_create",       (void*)bridge_sys_lwmutex_create);
    reg("sys_lwmutex_lock",         (void*)bridge_sys_lwmutex_lock);
    reg("sys_lwmutex_trylock",      (void*)bridge_sys_lwmutex_trylock);
    reg("sys_lwmutex_unlock",       (void*)bridge_sys_lwmutex_unlock);
    reg("sys_lwmutex_destroy",      (void*)bridge_sys_lwmutex_destroy);
#endif

    ps3_module_load(&mod_duck);
    ps3_register_module(&mod_duck);
    fprintf(stderr, "[HLE] module table ready (5 real bridges)\n");
}
