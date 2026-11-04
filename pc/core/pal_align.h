/* A block of the PS2's data in host memory: every host address of the PS2's data keeps the
 * low 16 bits of the PS2 address. A stale pointer's low half then equals the PS2's, as the game keeps it (a node
 * reused as another record keeps its old pointer's low 16 bits at +4), and a comparison with the original game sees no
 * difference. The blocks that hold a PS2 address range are aligned to 0x10000: the RAM block (Pal_EeRam, whose
 * address is the PS2's offset), the scratchpad, and the data blocks of tools/pc/data_c.py.
 *
 *   PAL_PS2_DATA(NAME, SIZE, SECTION)   defines unsigned char NAME[SIZE], 64 KiB aligned (SIZE a literal)
 *
 * ELF: the alignment attribute. COFF (Windows): clang refuses an alignment above 8192, and an object file's section
 * cannot ask for more. The block is defined in assembly in a section of its own (SECTION: at most 8 characters, a
 * different one for each block of a program, as lld truncates longer names in the image), "bw" (uninitialised, so
 * the image stays small), and the program is linked with --section-alignment=0x10000 (pc/cmake/mingw-i686.cmake),
 * which starts every output section at a multiple of 0x10000 from the image base (0x08000000). */
#ifndef MH2_PC_CORE_PAL_ALIGN_H
#define MH2_PC_CORE_PAL_ALIGN_H

#if defined(_WIN32)
#define PAL_PS2_DATA(name, size, section)                                                                          \
    extern unsigned char name[size];                                                                              \
    __asm__(".pushsection " section ",\"bw\"\n.balign 4096\n.globl _" #name "\n_" #name ":\n.zero " #size "\n.popsection")
#else
#define PAL_PS2_DATA(name, size, section) unsigned char name[size] __attribute__((aligned(0x10000)))
#endif

#endif
