/*
 * DuckTales: Remastered — ps3recomp port config
 *
 * Title:    DuckTales: Remastered (WayForward)
 * Title ID: BLUS31368 (USA disc)
 * Engine:   WayForward custom 2D engine (NOT PhyreEngine)
 *
 * 36,717 functions, ~13.5MB code, minimal SPU (cellSpurs x1). Chosen as the
 * 2nd ps3recomp port for tractability after flОw stalled on PhyreEngine boot.
 */
#ifndef DUCKTALES_CONFIG_H
#define DUCKTALES_CONFIG_H

#define DUCK_TITLE        "DuckTales: Remastered"
#define DUCK_TITLE_ID     "BLUS31368"

#ifndef DUCK_GAME_DIR
#define DUCK_GAME_DIR     "game"            /* holds EBOOT.elf + extracted USRDIR */
#endif

/* Entry: e_entry is an OPD -> _start. (from EBOOT.elf analysis) */
#define DUCK_ENTRY_OPD    0x00CFC1B8u       /* e_entry (function descriptor)      */
#define DUCK_START_CODE   0x00251F98u       /* *(_start OPD) = _start code addr   */
#define DUCK_TOC          0x00D20038u       /* *(_start OPD + 4) = initial TOC/r2 */

/* PT_LOAD layout (from EBOOT.elf): */
#define DUCK_CODE_BASE    0x00010000u       /* code+rodata, filesz 0xCDB9E8       */
#define DUCK_DATA_BASE    0x00CF0000u       /* data+BSS, filesz 0xB4580 memsz 0x355808 */

/* PS3 main memory */
#define DUCK_MAIN_MEM_SIZE  (256ULL * 1024 * 1024)

#define DUCK_STACK_SIZE     (1024 * 1024)   /* main-thread guest stack (1 MB) */

#define DUCK_WINDOW_WIDTH   1280
#define DUCK_WINDOW_HEIGHT  720
#define DUCK_WINDOW_TITLE   "DuckTales: Remastered (ps3recomp)"

#endif /* DUCKTALES_CONFIG_H */
