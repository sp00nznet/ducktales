/*
 * DuckTales Recomp — ELF segment loader
 *
 * Parses the original EBOOT.elf and copies PT_LOAD segments into ps3recomp
 * virtual memory. This populates the guest address space with initialized
 * data (.data/.rodata), zeroes .bss, and brings in the firmware import-stub
 * thunks + OPD table (read as data by the import installer). We don't execute
 * guest PowerPC — recompiled native functions run instead — but every global
 * the lifted code touches must exist at its original guest address.
 */
#include "elf_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" uint8_t* vm_base;

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

#define ELF_MAGIC    "\x7f" "ELF"
#define ELFCLASS64   2
#define ELFDATA2MSB  2
#define PT_LOAD      1

struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

static bool parse_ehdr(const uint8_t* d, size_t size, Elf64_Ehdr* o)
{
    if (size < 64) return false;
    if (memcmp(d, ELF_MAGIC, 4) != 0) return false;
    if (d[4] != ELFCLASS64 || d[5] != ELFDATA2MSB) return false;
    memcpy(o->e_ident, d, 16);
    const uint8_t* p = d + 16;
    o->e_type = be16(p); p += 2;  o->e_machine = be16(p); p += 2;
    o->e_version = be32(p); p += 4;  o->e_entry = be64(p); p += 8;
    o->e_phoff = be64(p); p += 8;  o->e_shoff = be64(p); p += 8;
    o->e_flags = be32(p); p += 4;  o->e_ehsize = be16(p); p += 2;
    o->e_phentsize = be16(p); p += 2;  o->e_phnum = be16(p); p += 2;
    o->e_shentsize = be16(p); p += 2;  o->e_shnum = be16(p); p += 2;
    o->e_shstrndx = be16(p);
    return true;
}

static void parse_phdr(const uint8_t* d, Elf64_Phdr* o)
{
    o->p_type = be32(d); d += 4;  o->p_flags = be32(d); d += 4;
    o->p_offset = be64(d); d += 8;  o->p_vaddr = be64(d); d += 8;
    o->p_paddr = be64(d); d += 8;  o->p_filesz = be64(d); d += 8;
    o->p_memsz = be64(d); d += 8;  o->p_align = be64(d);
}

bool elf_load_segments(const char* elf_path)
{
    FILE* fp = fopen(elf_path, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0 || file_size > 64 * 1024 * 1024) {
        fprintf(stderr, "[elf_loader] invalid file size: %ld\n", file_size);
        fclose(fp); return false;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)file_size);
    if (!data) { fclose(fp); return false; }
    if (fread(data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        fprintf(stderr, "[elf_loader] read error\n");
        free(data); fclose(fp); return false;
    }
    fclose(fp);

    Elf64_Ehdr eh;
    if (!parse_ehdr(data, (size_t)file_size, &eh)) {
        fprintf(stderr, "[elf_loader] not a valid ELF64 BE file\n");
        free(data); return false;
    }
    printf("[elf_loader] ELF64 BE entry=0x%08llX, %u phdrs\n",
           (unsigned long long)eh.e_entry, eh.e_phnum);

    int loaded = 0; size_t bytes = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        uint64_t off = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        if (off + eh.e_phentsize > (uint64_t)file_size) break;

        Elf64_Phdr ph;
        parse_phdr(data + off, &ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t va = (uint32_t)(ph.p_vaddr & 0xFFFFFFFF);
        bool is_exec = (ph.p_flags & 1) != 0;
        printf("[elf_loader]   seg %d: %s vaddr=0x%08X filesz=0x%llX memsz=0x%llX\n",
               i, is_exec ? "CODE" : "DATA", va,
               (unsigned long long)ph.p_filesz, (unsigned long long)ph.p_memsz);

        if ((uint64_t)va + ph.p_memsz > 0xE0000000ULL) {
            printf("[elf_loader]     skip (out of VM range)\n");
            continue;
        }
        uint8_t* host = vm_base + va;
        memset(host, 0, (size_t)ph.p_memsz);                 /* .bss */
        if (ph.p_filesz > 0 && ph.p_offset + ph.p_filesz <= (uint64_t)file_size)
            memcpy(host, data + ph.p_offset, (size_t)ph.p_filesz);

        loaded++; bytes += (size_t)ph.p_memsz;
    }
    free(data);
    printf("[elf_loader] loaded %d segments (%zu bytes)\n", loaded, bytes);
    return loaded > 0;
}
