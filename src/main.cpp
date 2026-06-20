/*
 * DuckTales: Remastered — ps3recomp port entry point.
 *
 * Clean boot path (no flОw-style bypass/redirect machinery):
 *   vm_init -> commit RAM -> syscalls -> HLE modules -> load ELF data ->
 *   install firmware imports -> enter recompiled _start and let the game's
 *   own CRT run constructors and call main.
 */
#include "config.h"
#include "elf_loader.h"

#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
    #include "runtime/memory/vm.h"
    #include "runtime/ppu/ppu_context.h"
    #include "runtime/syscalls/lv2_syscall_table.h"
    #include "runtime/syscalls/sys_ppu_thread.h"
}

/* Runtime globals defined exactly once by the game project. */
extern "C" uint8_t* vm_base = nullptr;
namespace vm { uint8_t* g_base = nullptr; }
lv2_syscall_table g_lv2_syscalls;
extern "C" char g_sys_fs_root[512];

/* Project glue. */
extern "C" void duck_register_hle_modules(void);
extern "C" void duck_install_imports(void);
extern "C" void ps3_trampoline_run(ppu_context* ctx, void (*fn)(void*));
extern "C" void ps3_thread_entry(ppu_context* ctx);

/* Recompiled _start (e_entry OPD -> this code address). The recomp chunks
 * compile as C++, so this matches their C++ linkage (no extern "C"). */
void func_00251F98(ppu_context* ctx);

/* ---------------------------------------------------------------------------
 * Demand-paging + diagnostic crash handler. Commits guest pages on first
 * touch (the 4 GB space is sparsely pre-committed); logs genuine host faults.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    uintptr_t base = (uintptr_t)vm_base;
    if (vm_base && addr >= base && addr < base + 0x100000000ULL) {
        uintptr_t page = addr & ~0xFFFULL;
        if (VirtualAlloc((void*)page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
            static int n = 0;
            if (++n <= 20)
                fprintf(stderr, "[vm-demand] committed guest page 0x%08X (#%d)\n",
                        (uint32_t)((addr - base) & 0xFFFF0000), n);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    /* 4 GB boundary: a guest pointer near 0xFFFFFFFF (or a -1 error code used
     * as a pointer) makes a 32-bit access spill just past the guest window.
     * Reserve+commit a guard region so the read returns zeroes instead of
     * faulting. */
    if (vm_base && addr >= base + 0x100000000ULL && addr < base + 0x100100000ULL) {
        uintptr_t page = addr & ~0xFFFULL;
        if (VirtualAlloc((void*)page, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            static int n = 0;
            if (++n <= 5)
                fprintf(stderr, "[vm-guard] committed 4GB-boundary guard page (guest ~0xFFFFFFFF)\n");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    int is_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
    bool in_vm = (vm_base && addr >= base && addr < base + 0x100000000ULL);
    if (in_vm)
        fprintf(stderr, "[crash] AV %s guest=0x%08X RIP=%p\n",
                is_write ? "WRITE" : "READ", (uint32_t)(addr - base),
                (void*)ep->ContextRecord->Rip);
    else
        fprintf(stderr, "[crash] AV %s HOST addr=%p RIP=%p\n",
                is_write ? "WRITE" : "READ", (void*)addr,
                (void*)ep->ContextRecord->Rip);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ---------------------------------------------------------------------------
 * Spin watchdog: a few seconds in, periodically suspend the main thread and
 * report the guest function its RIP sits in. Turns a silent hang into a known
 * PC so we can see exactly where the boot is stuck.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
extern "C" const char* duck_resolve_host_rip(void* rip, uint32_t* out_guest);
extern "C" uint32_t vm_read32(uint64_t addr);
static HANDLE g_main_thread = NULL;
volatile ppu_context* g_dbg_ctx = nullptr;   /* live main-thread registers */

/* When the main thread is stuck in the dlmalloc tree-bin search (func_009653C0),
 * dump the arena + the tree node chain to see whether the child pointers cycle
 * (corruption) or the arena is uninitialized garbage. */
static void dump_malloc_tree(void)
{
    if (!g_dbg_ctx) return;
    uint32_t arena = (uint32_t)g_dbg_ctx->gpr[26];
    uint32_t node  = (uint32_t)g_dbg_ctx->gpr[8];
    uint32_t req   = (uint32_t)g_dbg_ctx->gpr[30];
    fprintf(stderr, "[heap] arena=0x%08X reqsize=0x%X cur_node=0x%08X\n", arena, req, node);
    uint32_t n = node, seen[12]; int ns = 0;
    for (int i = 0; i < 12 && n; i++) {
        uint32_t head = vm_read32(n + 0x4);
        uint32_t c0   = vm_read32(n + 0x10);
        uint32_t c1   = vm_read32(n + 0x14);
        fprintf(stderr, "[heap]   node 0x%08X: head=0x%08X child[0]=0x%08X child[1]=0x%08X\n",
                n, head, c0, c1);
        for (int j = 0; j < ns; j++) if (seen[j] == n) { fprintf(stderr, "[heap]   ^^ CYCLE back to a visited node\n"); return; }
        seen[ns++] = n;
        n = c0 ? c0 : c1;   /* descend */
    }
}

static DWORD WINAPI watchdog_proc(LPVOID)
{
    HMODULE exe = GetModuleHandleA(NULL);
    int dumped = 0;
    for (int i = 0; i < 60; i++) {
        Sleep(2000);
        if (!g_main_thread) continue;
        /* Suspend only long enough to read RIP, then RESUME before doing any
         * fprintf — printing while the main thread is suspended inside the CRT
         * lock (e.g. mid-fprintf) would deadlock the watchdog. */
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(g_main_thread);
        BOOL ok = GetThreadContext(g_main_thread, &c);
        ResumeThread(g_main_thread);
        if (ok) {
            uint32_t guest = 0;
            const char* nm = duck_resolve_host_rip((void*)c.Rip, &guest);
            fprintf(stderr, "[watchdog] t=%ds main RIP=exe+0x%llX  guest=%s (0x%08X)\n",
                    (i + 1) * 2, (unsigned long long)(c.Rip - (uintptr_t)exe),
                    nm ? nm : "?", guest);
            if (guest == 0x009653C0 && dumped < 3) { dump_malloc_tree(); dumped++; }
            fflush(stderr);
        }
    }
    return 0;
}

static void start_watchdog(void)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);
}
#endif

static void commit_regions(void)
{
    /* Low + ELF region (code 0x10000.., data 0xCF0000.., BSS) */
    vm_commit(0x00000000, 0x00900000);
    vm_commit(0x00900000, 0x10000000 - 0x00900000);   /* heap / BSS */
    vm_commit(VM_RSX_BASE, VM_RSX_SIZE);               /* 0x10000000 */
    vm_commit(0x20000000, 0x10000000);                 /* extra heap */
    /* General region (holds our import OPD scratch at 0x3E000000). */
    for (uint32_t b = 0x30000000; b < 0xC0000000; b += 0x10000000)
        vm_commit(b, 0x10000000);
    /* Stack + high VRAM region. */
    vm_commit(0xC0000000, 0x10000000);
    vm_commit(0xD0000000, 0x10000000);
    vm_commit(0xE0000000, 0x10000000);
    vm_commit(0xF0000000, 0x10000000);
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
#endif

    printf("================================================\n");
    printf("  %s — ps3recomp\n", DUCK_TITLE);
    printf("  Title ID: %s\n", DUCK_TITLE_ID);
    printf("================================================\n\n");

    const char* game_dir = DUCK_GAME_DIR;
    if (argc > 1) game_dir = argv[1];
    printf("[init] game dir: %s\n", game_dir);

    if (vm_init() != CELL_OK) { fprintf(stderr, "vm_init failed\n"); return 1; }
    vm::g_base = vm_base;
    fprintf(stderr, "[init] vm_base = %p\n", (void*)vm_base);

    lv2_syscall_table_init(&g_lv2_syscalls);
    lv2_register_all_syscalls(&g_lv2_syscalls);

    duck_register_hle_modules();

    /* Real PPU threads route through our resolver. */
    {
        extern ppu_thread_entry_fn g_ppu_thread_entry_trampoline;
        g_ppu_thread_entry_trampoline = ps3_thread_entry;
    }

    snprintf(g_sys_fs_root, sizeof(g_sys_fs_root), "%s", game_dir);
    vm_stack_alloc_init(&g_vm_stack_alloc);
    commit_regions();

    char elf_path[600];
    snprintf(elf_path, sizeof(elf_path), "%s/EBOOT.elf", game_dir);
    if (!elf_load_segments(elf_path))
        fprintf(stderr, "[init] WARNING: could not load ELF segments from %s\n", elf_path);

    /* Repoint firmware import slots now that the data segment (and dispatch
     * table) exist. */
    duck_install_imports();

    /* Main-thread context: enter recompiled _start with the title's TOC. */
    ppu_context ctx;
    ppu_context_init(&ctx);
    uint32_t sp = vm_stack_allocate(&g_vm_stack_alloc, DUCK_STACK_SIZE);
    if (sp) {
        ppu_set_stack(&ctx, sp, DUCK_STACK_SIZE);
        printf("[init] stack: 0x%08X (%u KB)\n", sp, DUCK_STACK_SIZE / 1024);
    }
    ctx.cia    = DUCK_START_CODE;
    ctx.gpr[2] = DUCK_TOC;
    ctx.lr     = 0;   /* _start never returns; CRT calls sys_process_exit */

    printf("[init] entering _start @0x%08X (TOC=0x%08X)\n\n",
           DUCK_START_CODE, DUCK_TOC);

#ifdef _WIN32
    g_dbg_ctx = &ctx;
    start_watchdog();
    __try {
#endif
        ps3_trampoline_run(&ctx, (void(*)(void*))func_00251F98);
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "\n[crash] exception 0x%08lX  CIA=0x%08X SP=0x%llX TOC=0x%llX LR=0x%llX\n",
                GetExceptionCode(), (uint32_t)ctx.cia,
                (unsigned long long)ctx.gpr[1], (unsigned long long)ctx.gpr[2],
                (unsigned long long)ctx.lr);
    }
#endif

    printf("\n[exit] DuckTales recomp finished. Shutting down.\n");
    vm_shutdown();
    return 0;
}
