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
| Game init sequence | ✅ reaches `cellGameBootCheck`, module loads, SPURS, net, NP init |
| TLS + thread bridges | ✅ real `sys_initialize_tls` (from PT_TLS) + real `sys_ppu_thread_create` |
| C++ global constructors | ✅ runs through the `__do_global_ctors` table |
| CRT heap / `malloc` | 🔜 stuck in the allocator's free-list walk — heap not brought up yet |
| Graphics (RSX → D3D12/Vulkan) | 🔜 not started |
| Audio / input | 🔜 not started |

**Where it stands today:** boot enters the recompiled `_start`, sets up TLS, and runs the C++ global constructors — which fire the real game-init calls (`cellGameBootCheck`, module loads, SPURS/net/NP). It now reaches the **CRT heap allocator** and spins in a best-fit free-list search (`func_009653C0`) because the heap's free list was never initialized (no `sys_memory_allocate` yet). CRT heap bring-up is the next frontier.

A built-in **spin watchdog** periodically samples the main thread's PC and resolves it to a guest function, so a silent hang becomes a named address to chase. That's how each of the blockers below was found.

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

### v0.1.5 — "Past the Wall" (2026-06-20)
- 🧱➡️ The TOC fix got the boot **through the CRT heap phase** — it no longer
  spins in `malloc`; it reaches the message-pump / thread-coordination phase.
- 🧵 The boot now spawns the **`TmpMsgPump`** worker thread and reaches
  `cellSysutilCheckCallback` (the main-loop sysutil hook).
- 🩹 Pragmatic stub for syscalls **85** (`sys_event_flag_wait`) and **118**
  (event-flag op) — they were `ENOSYS` and the pump spun on them (5,240 calls).
  Returning success unblocks the wait. (Proper event-flag semantics later — for
  now the worker/main coordination is hand-waved.)
- 🔜 Next: a CRT coordination loop (`func_00270EA8`/`func_002716C8`/`func_00271870`)
  + two one-time `ENOSYS` syscalls (82, 872). Then: graphics (RSX).

### v0.1.4 — "TOC Talk" (2026-06-20)
- 🎯 **Landmark fix: TOC (`r2`) corruption in `nid_dispatch`.** The lifted import
  thunk already saves the caller's `r2` to `sp+0x28` before setting `r2 = OPD.toc`
  and calling us — but `nid_dispatch` then re-saved `r2` (now the synthetic OPD
  toc = **0**) to the same slot, so the thunk restored `r2 = 0`. Every HLE import
  was zeroing the caller's TOC. Removed the redundant save; the lifted code owns
  TOC management. This is a general fix affecting *all* import calls.
- With TOC preserved, the CRT heap-init now reads the correct mspace base
  (`*(0xD1B174) = 0x101D918`) and reaches `create_mspace` with valid arguments.
- 🔜 Next layer: `create_mspace` (`func_00966AF8`) still returns 0 — its internal
  `mspace_malloc` (`func_00964F88`) fails because the mspace's initial memory
  isn't acquired (the `sys_memory_allocate` path inside create isn't taken yet).

### v0.1.3 — "Oracle" (2026-06-20)
- 🔮 Booted the same EBOOT in **RPCS3** (the oracle) and mapped the real heap
  bring-up: the CRT heap is created via `sys_memory_allocate(size=0x200000)` then
  `sys_memory_allocate(0xa500000)` (the 165 MB pool) — and the real boot is
  **PRX-driven** (it `_sys_prx_load_module`s liblv2/libsysmodule/cellGcm/…).
- 🩺 Diagnosis (watchdog + memory/callchain dumps): our boot never calls
  `sys_memory_allocate` — the heap-init function (`func_0025ED40`) is never
  reached, so `malloc` runs on a NULL mspace and spins. The CRT init
  (`func_00270448`) *does* run (`cellGameBootCheck` etc.), but skips heap
  creation.
- ✅ Real `cellGameBootCheck` + `cellGameContentPermit` bridges (disc-boot
  values: type=DISC, attributes=0, content paths) — verified against the oracle.
- 🔭 Reusable diagnostics: guest-address resolver + guest stack-chain walk in the
  watchdog.
- 🔜 Next: trace why `func_00270448` skips the heap-init call (ordering/branch),
  so the mspace gets created and the pool `malloc` succeeds.

### v0.1.2 — "Shift Happens" (2026-06-20)
- 🐛 **Lifter bug fixed (`slw`/`srw`/`sraw`):** the 32-bit shift-word ops emitted
  `(uint32_t)x << (n & 0x3F)`, but PPC produces 0 when the shift ≥ 32. C `<< n`
  is UB for n≥32 and x86 masks the count to 5 bits → a wrong *nonzero* result.
  This breaks any code that shifts a mask to zero to terminate a loop. Fixed in
  `ppu_lifter.py`; existing output patched by `tools/fix_shift_word.py` (2,582
  sites). The 64-bit `sld`/`srd` were already correct — only the 32-bit forms
  were wrong.
- 🔬 Diagnosed the current wall: the CRT `malloc` (`func_009653C0`) is a
  dlmalloc tree-bin search spinning on a non-empty treebin with cyclic child
  pointers → the allocator **arena was never zero-initialized**. CRT heap
  bring-up is the active frontier.

### v0.1.1 — "Constructors & Threads" (2026-06-20)
- 🧵 Real `sys_ppu_thread_create`/`exit` (delegate to the runtime; entry OPD
  resolved by the thread trampoline) + a moving `sys_time_get_system_time`.
- 🧠 Real `sys_initialize_tls` driven from the ELF's **PT_TLS** template
  (`0xD1B224`, `0x1E8` bytes) — the real ABI passes the thread_id in `r3`, not
  the template address, so the template must come from PT_TLS. Setting a valid
  `r13` let the C++ constructors run for real and broke past the ctor-walk spin.
- 🔭 Added a **spin watchdog**: samples the main thread's RIP and maps it to the
  nearest guest function. Turns silent hangs into named PCs.
- 🐛 Import sentinels now resolve via an O(1) range check instead of the 50K-entry
  hash table (they were being lost in long probe chains).
- 🐛 4 GB-boundary guard page for guest pointers near `0xFFFFFFFF`.
- 🔜 Next: bring up the CRT heap so `malloc` stops spinning on an empty free list.

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
