#!/usr/bin/env python3
"""
fix_undefined_labels.py — make the lifted chunks compile.

find_functions occasionally hands the lifter a bogus, oversized function
boundary (data misidentified as code at the tail of the code segment). Inside
such a "function", in-range branch targets are emitted as `goto loc_XXXX;` but
the byte at that address was never decoded into a labeled instruction, so the
label is undefined -> C2094 and the whole build fails.

These functions are data and are never legitimately executed. For each
function we collect the loc labels referenced by `goto` and the labels actually
defined, then inject `loc_XXXX: ;` for every undefined one immediately before
the function's closing brace. A stray goto therefore lands at function end and
returns — harmless, and the TU compiles.

Operates per chunk, in place. Deterministic (re-runnable; re-liftable).
"""
import re, sys, glob, os

GOTO   = re.compile(r'goto (loc_[0-9A-Fa-f]+)\s*;')
LABEL  = re.compile(r'^\s*(loc_[0-9A-Fa-f]+):')
FUNC   = re.compile(r'^void (?:[A-Za-z0-9_]*func_[0-9A-Fa-f]+)\(ppu_context\* ctx\) \{\s*$')

def fix_chunk(path):
    lines = open(path, encoding="utf-8", errors="ignore").read().split("\n")
    out = []
    i = 0
    n = len(lines)
    total_injected = 0
    funcs_fixed = 0
    while i < n:
        line = lines[i]
        if FUNC.match(line):
            # gather the whole function body up to the closing '}' at col 0
            body = [line]
            j = i + 1
            while j < n and not (lines[j] == "}" or lines[j].rstrip() == "}"):
                body.append(lines[j]); j += 1
            # j is the closing brace (or EOF)
            text = "\n".join(body)
            referenced = set(GOTO.findall(text))
            defined = set(m.group(1) for m in (LABEL.match(b) for b in body) if m)
            undefined = sorted(referenced - defined)
            out.extend(body)
            if undefined:
                for lab in undefined:
                    out.append(f"        {lab}: ;")
                total_injected += len(undefined)
                funcs_fixed += 1
            if j < n:
                out.append(lines[j])   # the closing brace
            i = j + 1
        else:
            out.append(line); i += 1
    if total_injected:
        open(path, "w", encoding="utf-8").write("\n".join(out))
    return funcs_fixed, total_injected

def main():
    chunks = sorted(glob.glob(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "src", "recomp", "ppu_recomp_*.cpp")))
    if len(sys.argv) > 1:
        chunks = sys.argv[1:]
    grand_f = grand_l = 0
    for c in chunks:
        f, l = fix_chunk(c)
        if l:
            print(f"  {os.path.basename(c)}: {l} labels in {f} funcs")
        grand_f += f; grand_l += l
    print(f"injected {grand_l} labels across {grand_f} functions")

if __name__ == "__main__":
    main()
