# 🦆 DuckTales: Remastered — Recompiled

> *"Life is like a hurricane..."* — and so is statically recompiling a PS3 game to run natively on your PC.

A **static recompilation** of *DuckTales: Remastered* (WayForward, 2013 — PS3 disc `BLUS31368`) into native x86-64, built on the [**ps3recomp**](https://github.com/sp00nznet/ps3recomp) runtime. No emulator, no interpreter at the core — the PowerPC code is lifted **ahead of time** into C++, compiled native, and linked against an HLE runtime that stands in for the Cell OS.

This is the **second** ps3recomp title (after [flOw](https://github.com/sp00nznet/flow)), deliberately chosen as a more tractable target: a small custom 2D engine instead of flOw's gated PhyreEngine boot.

---

## 🎯 Status: **It builds. It boots. It runs real game code.**

| Milestone | State |
|---|---|
| SELF → ELF decrypt | ✅ done |
| Function discovery | ✅ 36,717 OPD funcs → **50,077** lifted (call-target analysis found ~13K more) |
| PPU → C++ lift | ✅ 49 chunks, ~2 GB of generated C++ |
| Build & link | ✅ **1.65 GB native x86-64 executable** |
| Boot / CRT startup | ✅ enters recompiled `_start`, runs the game's own C++ constructors |
| Firmware imports (NID) | ✅ all 239 wired through the PS3 import-table model |
| Game init sequence | ✅ reaches `cellGameBootCheck`, module loads, **thread creation**, SPURS, net, NP init |
| Worker threads | 🔜 `sys_ppu_thread_create` stubbed → main thread spins waiting on a worker |
| Graphics (RSX → D3D12/Vulkan) | 🔜 not started |
| Audio / input | 🔜 not started |

**Where it stands today:** the boot thread sails through the *entire* early-init sequence and then parks in a `sys_timer`/mutex wait loop — it's waiting on a worker thread that hasn't been spawned for real yet. That's the next domino.

---

## 🛠️ How it works

```
EBOOT.BIN (encrypted SELF)
   │  rpcs3 --decrypt
   ▼
EBOOT.elf  (ELF64 BE, PowerPC64, ET_EXEC)
   │  tools/find_functions.py        → 36,717 functions
   │  tools/ppu_lifter.py            → 50,077 functions lifted to C++
   ▼
src/recomp/ppu_recomp_*.cpp          (49 chunks, ~2 GB — git-ignored)
   │  + the runtime glue (this repo)
   │  + ps3recomp HLE runtime
   ▼
ducktales.exe                        (native x86-64)
```

### The runtime glue (what lives in this repo)

Clean, from-scratch glue — *none* of flOw's hard-won-but-game-specific hand-edit debt:

| File | Job |
|---|---|
| `src/main.cpp` | Boot: init VM, commit RAM, load ELF data, install imports, enter recompiled `_start`, let the game's own CRT run constructors. |
| `src/vm_bridge.cpp` | `vm_read*/vm_write*` (big-endian guest RAM) + the LV2 syscall dispatch bridge. |
| `src/indirect_dispatch.cpp` | `bctrl`/`bctr` resolution: guest address → host function, via a hash table over the lifter's native `function_table[]`, with OPD/vtable indirection. |
| `src/import_table.cpp` | **Generated.** The 239 firmware imports, wired via the authentic PS3 model (see below). |
| `src/hle_dispatch.cpp` | NID dispatch → HLE bridge or a logging `CELL_OK` stub. Iterate from the boot trace. |
| `src/elf_loader.cpp` | Maps `PT_LOAD` segments (data, rodata, import stubs) into guest memory. |
| `tools/gen_import_table.py` | Emits `import_table.cpp` from the EBOOT's import table + NID database. |
| `tools/fix_undefined_labels.py` | Post-process safety net for data-misidentified-as-code (also fixed upstream in the lifter). |

### The PS3 import trick 🪝

PS3 firmware EBOOTs don't call HLE functions directly — each import goes through a tiny **stub thunk** that reads a function pointer from a slot in the data segment. In the raw ELF those slots are *self-referential placeholders* that the real PS3's PRX loader overwrites at load time.

So we **do the loader's job**: for each of the 239 imports we synthesize an OPD whose entry is a unique *sentinel* address, repoint the data-segment slot at it, and resolve the sentinel — with an O(1) range check — straight into the HLE dispatcher. The recompiled game is none the wiser. 🎩

---

## 🚀 Building it yourself

You'll need: a dumped & decrypted `EBOOT.elf`, the [ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK as a sibling checkout, CMake 3.20+, Ninja, and MSVC (VS 2022).

```bash
# 1. Drop your decrypted EBOOT.elf into game/
# 2. Lift the PPU code (~1 hour, ~2 GB output)
python ../../ps3/tools/find_functions.py game/EBOOT.elf --output ducktales_functions.json
python ../../ps3/tools/ppu_lifter.py game/EBOOT.elf --functions ducktales_functions.json --output src/recomp

# 3. Generate the import table
python ../../ps3/tools/ppu_loader.py game/EBOOT.elf --output analysis
python tools/gen_import_table.py

# 4. Configure + build (no-opt first build = far faster on 2 GB of C++)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS_RELEASE="/Od /MD /DNDEBUG"
cmake --build build -j 6

# 5. Run (watch the boot trace on stderr)
./build/ducktales.exe game
```

> ⚠️ **Legal:** This repo contains *zero* game code or assets — only the recompiler glue. You must own DuckTales: Remastered and dump your own copy. The recompiled C++ and the EBOOT are git-ignored for a reason.

---

## 📜 Changelog

### v0.1.0 — "First Boot" (2026-06-20)
- 🎉 **It compiles and boots.** 50,077 functions lifted, 1.65 GB native exe.
- ✅ Clean runtime glue written from scratch (no flOw hand-edit debt).
- ✅ All 239 firmware imports wired via the synthesized-OPD/sentinel model.
- ✅ Boot reaches the full game-init sequence: content check, module loads,
  thread creation, SPURS / net / NP init.
- 🐛 Fixed a lifter bug: data-misidentified-as-code produced `goto` into
  undecoded regions (undefined labels). Now emits safe trailing labels —
  fixed both in the output and upstream in `ppu_lifter.py`.
- 🐛 Added a 4 GB-boundary guard page for guest pointers near `0xFFFFFFFF`.
- 🔜 Next: real `sys_ppu_thread_create` so worker threads actually run and the
  main thread stops spinning on its wait loop.

---

## 🙏 Credits

- **ps3recomp** — the runtime and toolchain that makes this possible.
- Lineage: [N64Recomp](https://github.com/N64Recomp/N64Recomp) pioneered "recompile to C, link with a runtime"; [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) proved it on the same PowerPC family.
- [RPCS3](https://rpcs3.net/) — the indispensable oracle for verifying behavior.
- DuckTales: Remastered © Disney / Capcom, developed by WayForward. This project is an independent, non-commercial preservation experiment and ships none of their code.

*Made with 🦆 and an unreasonable number of `goto`s.*
