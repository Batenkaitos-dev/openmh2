/* The game's data blocks (tools/pc/data_c.py, generated into gen/data/data_MODULE.c): the platform layer's view.
 *
 * Each resident block lays out a span of the PS2's memory at its PS2 offsets, with the PS2 address's offset in
 * its 64 KiB kept (pal_align.h):
 *   DataC_main_Data   main's .data and .rodata, 0x195C00-0x1C8600: the libraries' .data and .rodata (the
 *                     splitter's pieces data.data.o and rodata.data.o), all zeros at start (a library's initial
 *                     bytes never come from the binary: each library sets its own fields from its interface);
 *                     the C's .data (0x1BA110-0x1BCE80) and .rodata (0x1C7B10-0x1C8600) with the matching
 *                     objects' bytes, every global of them a name of the block (the C's own definitions are
 *                     weak). The C's file-static objects are holes (their PC copies are elsewhere: report.txt).
 *   DataC_main_Bss    main's .bss, 0x1C8600-0x2E3900 (the libraries' .bss included), zeros.
 *   DataC_sub_main_Data sub_main's .data and .rodata, 0x3E7F00-0x4B1900: text_end's .data piece (zeros), the
 *                     VU1 programs (the vu1_microcode piece 0x3E7F80-0x4021C0, the matching bytes, names Vu1Prog00..)
 *                     and the C's .data and .rodata, as in DataC_main_Data (gVu1ProgInfoNN.upload: Vu1ProgNN's
 *                     host address).
 *   DataC_sub_main_Bss sub_main's .bss, 0x4B1900-0x6AF200, zeros, its C .bss objects included (0x59BF80-0x59C1B0).
 * The overlays' blocks (DataC_<m>_Data and DataC_<m>_Bss of select, game, lobby, nethttp, gm_sub, lbguild, yn,
 * plsel) are RAM-backed: a window is one memory, the RAM block's own
 * range, and every name of an overlay's .data, .rodata and .bss is Pal_EeRam + its PS2 address (the block's symbol
 * is Pal_EeRam + (lo - lo % 0x10000)). Their bytes are whichever overlay is loaded: the disc's image, which the
 * loader copies into the window, then DataC_OverlayWords' values over it, then its .bss cleared.
 */
#ifndef MH2_PC_CORE_DATA_C_H
#define MH2_PC_CORE_DATA_C_H

/* The host address of PS2 address ps2 when a resident block lays it out at its PS2 offset: host - ps2 is the
 * same for the whole range, so (host & 0xFFFF) == (ps2 & 0xFFFF). Any other address (an overlay's window, a C
 * object's hole, no module's data, a KSEG or uncached alias) stops the core with Pal_Fatal. A library that
 * hands the game a pointer into its own work area (a handle) takes it from here at the original's address,
 * so that the pointer the game keeps translates to the PS2's value (a pointer
 * into a block keeps its offset): DataC_At(0x19DF20) is ADXT's first handle. */
void *DataC_At(unsigned int ps2);

/* DataC_At's inverse: the PS2 address of host pointer p into a block (any module's, resident or an overlay's,
 * at its PS2 offsets), the RAM block (Pal_EeRam: the offset) or the scratchpad (Pal_Spr: 0x70000000 + the offset). What the game sends to the IOP holds PS2
 * addresses. A pointer in neither stops the core with Pal_Fatal
 * ("DataC_Ps2Of: host pointer 0x... is in no data block, the RAM block or the scratchpad"). Within a block,
 * DataC_Ps2Of(DataC_At(a)) == a. A pointer to a C object a block doesn't lay out (a file-static, or a C object
 * of a module whose data isn't moved yet) stops. */
unsigned int DataC_Ps2Of(const void *p);

/* An overlay's words that are addresses (its .data and .rodata, the matching build's R_MIPS_32 words): count
 * pairs (PS2 address, the PC value), sorted by address. The loader writes each value to Pal_EeRam + ps2 after
 * copying the disc's image into the window, at every load: the window then holds the PS2's bytes everywhere but
 * in these words, which hold the PC's addresses of the same targets. 0 and *count 0 for an OverlayId with none. */
typedef struct DataCWord { unsigned int ps2, value; } DataCWord;
const DataCWord *DataC_OverlayWords(int overlayId, unsigned int *count);

/* DataC_At without the stop, over all the core's memory: a resident block's host address, the RAM block's
 * (Pal_EeRam + ps2) for an address below 32 MB that no block holds, the scratchpad's (Pal_Spr + ps2 - 0x70000000)
 * for 0x70000000-0x70003FFF, and 0 for anything else (a resident block's hole, other addresses past 32 MB). An
 * overlay's window is the RAM block's (whichever overlay is loaded). DataC_Ps2Of inverts it. */
void *DataC_TryAt(unsigned int ps2);

#endif
