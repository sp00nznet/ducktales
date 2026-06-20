/*
 * DuckTales Recomp — ELF segment loader
 *
 * Loads PT_LOAD segments from the original EBOOT.elf into the ps3recomp
 * virtual address space at runtime. Recompiled code references globals and
 * read-only data at their original guest addresses via vm_read/vm_write, so
 * these segments must be mapped before execution. Code segments are loaded
 * too (the import stub thunks + OPDs live there and are read as data).
 */
#ifndef DUCK_ELF_LOADER_H
#define DUCK_ELF_LOADER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load all PT_LOAD segments from the ELF file into virtual memory.
 * Returns true on success, false if the file is missing/corrupt. */
bool elf_load_segments(const char* elf_path);

#ifdef __cplusplus
}
#endif

#endif /* DUCK_ELF_LOADER_H */
