/* vm_bridge.cpp - memory + syscall bridge between the lifted code and the
 * ps3recomp runtime.
 *
 * The lifter emits vm_read/vm_write helpers with 64-bit guest addresses; guest
 * RAM is big-endian. We truncate to 32 bits, index vm_base, and byte-swap.
 * Clean DuckTales build - no game-specific read/write traps.
 */
#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef _MSC_VER
#include <stdlib.h>
static inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v);  }
static inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

/* Defined in main.cpp, set by vm_init(). */
extern "C" uint8_t* vm_base;

/* The single global module registry instance (declared extern in module.h). */
#include "ps3emu/module.h"
ps3_module_registry g_ps3_module_registry = {};

static inline uint8_t* gp(uint64_t addr) { return vm_base + (uint32_t)addr; }

extern "C" {

uint8_t  vm_read8 (uint64_t a) { return *gp(a); }
uint16_t vm_read16(uint64_t a) { uint16_t r; memcpy(&r, gp(a), 2); return bswap16(r); }
uint32_t vm_read32(uint64_t a) { uint32_t r; memcpy(&r, gp(a), 4); return bswap32(r); }
uint64_t vm_read64(uint64_t a) { uint64_t r; memcpy(&r, gp(a), 8); return bswap64(r); }

void vm_write8 (uint64_t a, uint8_t  v) { *gp(a) = v; }
void vm_write16(uint64_t a, uint16_t v) { uint16_t r = bswap16(v); memcpy(gp(a), &r, 2); }
void vm_write32(uint64_t a, uint32_t v) { uint32_t r = bswap32(v); memcpy(gp(a), &r, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t r = bswap64(v); memcpy(gp(a), &r, 8); }

} /* extern "C" */

/* ---------------------------------------------------------------------------
 * LV2 syscall dispatch — bridge from the recompiled `sc` instruction.
 *
 * The recompiled code uses ppu_recomp.h's ppu_context; the runtime's syscall
 * handlers use runtime/ppu/ppu_context.h. Both lead with gpr[32] at offset 0,
 * so the pointer casts safely. g_lv2_syscalls is an array of fn pointers.
 * -----------------------------------------------------------------------*/
#include "recomp/ppu_recomp.h"

struct lv2_syscall_table;
extern "C" lv2_syscall_table g_lv2_syscalls;
typedef int64_t (*lv2_syscall_fn)(void* ctx);

extern "C" void lv2_syscall(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    if (num >= 1024) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(-38); /* ENOSYS */
        return;
    }
    lv2_syscall_fn* handlers = (lv2_syscall_fn*)&g_lv2_syscalls;
    lv2_syscall_fn handler = handlers[num];
    if (handler) {
        ctx->gpr[3] = (uint64_t)handler((void*)ctx);
    } else {
        static int s_warn = 0;
        if (s_warn++ < 40)
            fprintf(stderr, "[lv2] unimplemented syscall %u (0x%X)\n", num, num);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(-38);
    }
}
