/*
 * Calls into an overlay by its PS2 address (pal_overlay.c; the stubs of tools/pc/gen_stubs.py).
 *
 * The game calls some functions of an overlay by their PS2 address: on the PS2 a call to 0x6B6700
 * reaches whatever the window holds there (select's Option_SetDefaults while select is loaded). On
 * the PC every overlay is part of the image, so such a call (an undefined func_<ADDR>) goes through
 * a table per window: the generated entry point reads which overlay Pal_OverlayActivate made active
 * in the address's window (Pal_OverlayWindowActive, an OverlayId; 0 when none is), and jumps to that
 * overlay's PC function at the address. An entry with no function (another overlay is loaded, none
 * is, or the one loaded has no PC function there) is a function that stops with a fatal error that
 * names the call and the window.
 *
 * The entry point changes no register the call passes or keeps (it saves EAX, and the flags are
 * never kept across a call) and leaves the stack as it found it, so the target is entered as if
 * the caller had called it: any prototype, any calling convention.
 */
#ifndef PAL_OVCALL_H
#define PAL_OVCALL_H

/* The overlay active in each window (an OverlayId; 0: none), indexed by pal_overlay.c's group. */
#define PAL_OVCALL_WINDOWS 8
#define PAL_OVCALL_IDS 16
extern int Pal_OverlayWindowActive[PAL_OVCALL_WINDOWS];

/* A call by address that reached no function: stops with a fatal error (pal_overlay.c). stub is the
 * called name, addr its PS2 address, window the group, known the overlays' names for the address. */
void Pal_OverlayCallMissing(const char *stub, unsigned int addr, int window, const char *known);

#define PAL_OVCALL_STR2(x) #x
#define PAL_OVCALL_STR(x) PAL_OVCALL_STR2(x)
#define PAL_OVCALL_SYM(name) PAL_OVCALL_STR(__USER_LABEL_PREFIX__) name
#if defined(__ELF__)
#define PAL_OVCALL_TYPE(name) ".type " PAL_OVCALL_SYM(name) ", @function\n"
#else
#define PAL_OVCALL_TYPE(name) ""
#endif

/*
 * The entry point: symbol name (the callers' spelling, "__regcall3__func_6B6700"), table the symbol
 * of its void (*const [PAL_OVCALL_IDS])(void), window a literal group number.
 */
#define PAL_OVCALL(name, table, window)                                         \
    __asm__(".text\n"                                                           \
            ".globl " PAL_OVCALL_SYM(name) "\n"                                 \
            PAL_OVCALL_TYPE(name)                                               \
            ".p2align 4\n"                                                      \
            PAL_OVCALL_SYM(name) ":\n"                                          \
            "\tpushl %eax\n"                                                    \
            "\tmovl " PAL_OVCALL_SYM("Pal_OverlayWindowActive") "+4*" #window ", %eax\n" \
            "\tandl $15, %eax\n"                                                \
            "\tmovl " PAL_OVCALL_SYM(table) "(,%eax,4), %eax\n"                 \
            "\txchgl %eax, (%esp)\n"                                            \
            "\tret\n")

/* A resident function's address (main, sub_main): one target, a jump. */
#define PAL_OVJUMP(name, target)                                                \
    __asm__(".text\n"                                                           \
            ".globl " PAL_OVCALL_SYM(name) "\n"                                 \
            PAL_OVCALL_TYPE(name)                                               \
            ".p2align 4\n"                                                      \
            PAL_OVCALL_SYM(name) ":\n"                                          \
            "\tjmp " PAL_OVCALL_SYM(target) "\n")

#endif
