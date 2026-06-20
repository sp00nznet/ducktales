#!/usr/bin/env python3
"""
fix_shift_word.py — patch the slw/srw/sraw >= 32 bug in already-lifted chunks.

PPC slw/srw produce 0 (and sraw replicates the sign bit) when the shift amount
is >= 32 (bit 0x20 set). The old lifter emitted `(uint32_t)x << (n & 0x3F)`,
which is UB in C for n>=32 and on x86 masks the count to 5 bits — yielding a
wrong nonzero value. Real-world impact: a binned allocator that walks bins by
shifting a mask to zero never terminates (infinite malloc spin).

The lifter is fixed going forward (ppu_lifter.py); this rewrites existing output
so we don't need a full re-lift to test. Deterministic / re-runnable.
"""
import re, sys, glob, os

# slw:  (uint32_t)ctx->gpr[A] << (ctx->gpr[B] & 0x3F)
SLW = re.compile(r'\(uint32_t\)ctx->gpr\[(\d+)\] << \(ctx->gpr\[(\d+)\] & 0x3F\)')
# srw:  (uint32_t)ctx->gpr[A] >> (ctx->gpr[B] & 0x3F)
SRW = re.compile(r'\(uint32_t\)ctx->gpr\[(\d+)\] >> \(ctx->gpr\[(\d+)\] & 0x3F\)')
# sraw: (int32_t)ctx->gpr[A] >> (ctx->gpr[B] & 0x3F)
SRAW = re.compile(r'\(int32_t\)ctx->gpr\[(\d+)\] >> \(ctx->gpr\[(\d+)\] & 0x3F\)')

def slw_sub(m):
    a, b = m.group(1), m.group(2)
    return f'((ctx->gpr[{b}] & 0x20) ? 0u : ((uint32_t)ctx->gpr[{a}] << (ctx->gpr[{b}] & 0x1F)))'

def srw_sub(m):
    a, b = m.group(1), m.group(2)
    return f'((ctx->gpr[{b}] & 0x20) ? 0u : ((uint32_t)ctx->gpr[{a}] >> (ctx->gpr[{b}] & 0x1F)))'

def sraw_sub(m):
    a, b = m.group(1), m.group(2)
    return f'(int32_t)ctx->gpr[{a}] >> ((ctx->gpr[{b}] & 0x20) ? 31 : (int)(ctx->gpr[{b}] & 0x1F))'

def fix(path):
    t = open(path, encoding="utf-8", errors="ignore").read()
    t, n_sraw = SRAW.subn(sraw_sub, t)   # sraw first (int32_t) so srw's uint32_t doesn't shadow
    t, n_slw  = SLW.subn(slw_sub, t)
    t, n_srw  = SRW.subn(srw_sub, t)
    total = n_slw + n_srw + n_sraw
    if total:
        open(path, "w", encoding="utf-8").write(t)
    return n_slw, n_srw, n_sraw

def main():
    chunks = sorted(glob.glob(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "src", "recomp", "ppu_recomp_*.cpp")))
    if len(sys.argv) > 1:
        chunks = sys.argv[1:]
    g = [0, 0, 0]
    for c in chunks:
        s = fix(c)
        for i in range(3): g[i] += s[i]
    print(f"patched slw={g[0]} srw={g[1]} sraw={g[2]} (total {sum(g)}) across {len(chunks)} chunks")

if __name__ == "__main__":
    main()
