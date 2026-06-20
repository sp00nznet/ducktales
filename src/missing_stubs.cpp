/* missing_stubs.cpp — definitions for branch/call targets the lifter declared
 * as `external` (in ppu_recomp.h) but never emitted a body for.
 *
 * These arise from data-as-code regions or branch-displacement underflow
 * (e.g. a `bl` whose target computes to 0xFFFFFFFC = -4). They are never
 * legitimately executed; a no-op that returns CELL_OK keeps the link clean.
 */
#include "recomp/ppu_recomp.h"

void func_FFFFFFFC(ppu_context* ctx) { ctx->gpr[3] = 0; }
