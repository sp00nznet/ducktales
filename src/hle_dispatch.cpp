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
#include <cstdio>

extern "C" uint32_t    duck_import_nid_for_sentinel(uint32_t sentinel);
extern "C" const char* duck_import_name_for_sentinel(uint32_t sentinel);

/* ppu_context HLE handler type. */
typedef void (*hle_fn)(ppu_context*);

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
    /* PPC64 ELF ABI: caller restores r2 from sp+0x28 after an inter-module
     * call. The lifted stub doesn't emit the save, so do it here. */
    uint64_t saved_toc = ctx->gpr[2];
    vm_write64((uint32_t)ctx->gpr[1] + 0x28, saved_toc);

    hle_fn fn = (hle_fn)ps3_resolve_func_nid(nid);
    if (fn) fn(ctx);
    else    hle_log_stub(ctx, nid, name);

    ctx->gpr[2] = saved_toc;
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

extern "C" void duck_register_hle_modules(void)
{
    ps3_module_init(&mod_duck, "duck_hle");
    /* (bridges registered here as we iterate) */
    ps3_module_load(&mod_duck);
    ps3_register_module(&mod_duck);
    fprintf(stderr, "[HLE] module table ready (0 bridges — all imports stubbed)\n");
}
