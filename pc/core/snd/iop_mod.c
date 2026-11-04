/* The IOP's modules (the interpreter is not in the core). The game's EE code loads each sound module's
 * IRX image into IOP memory (loadfile's buffer load, the kernel's KSif_LoadModule), and this file starts it: the block it
 * takes from the IOP heap is as large as the image's header says, so the heap's layout is the original's, and its code is
 * native: CSE and the ADX driver by their descriptors (g_iop_native_cse, g_iop_native_adx, set by
 * Libsd_Setting), libsd by the function here and the native libsd (pc/core/api/libsd.c).
 *
 * Nothing of a module's code is read or run: the image is read for its name, its sizes and its start function's offset
 * (the IRX header, "IRX" in Sony's module format: an ELF with a .iopmod program header), and libsd's image is copied
 * into its block because the native libsd reads a block of zeros in it (LIBSD_ZERO). */
#include "snd/snd_iop.h"

#define MAX_MODULES 16
#define INTR_STACK_SIZE 0x4000u

u32 g_iop_libsd_lo, g_iop_libsd_size;   /* libsd's block (set by its start function), for the native libsd's own memory */

static int g_nmods;
static u32 g_intr_stack;
static u32 g_libsd_base, g_libsd_size;

static u32 rd32i(const u8 *p) { u32 v; snd_memcpy(&v, p, 4); return v; }
static u16 rd16i(const u8 *p) { u16 v; snd_memcpy(&v, p, 2); return v; }
static int is_elf(const u8 *p) { return p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F'; }

int IopKern_ModuleLoaded(void)
{
    return g_intr_stack != 0;   /* the first module's load sets it */
}

/* libsd's start function, as it was observed on the original game (the kernel calls it made and the hardware it touched): it
 * registers its library with loadcore, and sets up the SPU2 bus and the DMA controller as the native sceSdInit does after
 * it (pc/core/api/libsd.c). It starts no thread. v0 is 0. The kernel calls are made as the module made them, so each
 * return is a call point as before. The interrupt state word is a local on the start thread's stack. */
static u32 LibsdStart(u32 argc, u32 argv, u32 a2, u32 a3)
{
    u32 w = IopNative_StackTop() - 0x40;
    (void)argc;
    (void)argv;
    (void)a2;
    (void)a3;
    IopNative_Call("CpuSuspendIntr", w, 0, 0, 0);
    g_iop_libsd_lo = g_libsd_base;      /* the library libsd is registered: its calls' returns are scheduling points */
    g_iop_libsd_size = g_libsd_size;
    SND_LOGI("iopk", "library \"libsd\" registered");
    IopNative_Call("CpuResumeIntr", 1, 0, 0, 0);
    IopMem_Write32(0x1F801404u, 0xBF900000u);
    IopMem_Write32(0x1F80140Cu, 0xBF900800u);
    IopNative_Call("CpuSuspendIntr", w, 0, 0, 0);
    IopMem_Write32(0x1F8010F0u, IopMem_Read32(0x1F8010F0u) | 0x80000u);
    IopMem_Write32(0x1F801570u, IopMem_Read32(0x1F801570u) | 8u);
    IopNative_Call("CpuResumeIntr", 1, 0, 0, 0);
    IopMem_Write32(0x1F801014u, 0x200B31E1u);
    IopMem_Write32(0x1F801414u, 0x200B31E1u);
    return 0;
}

int IopKern_LoadModule(u32 image, u32 max, const char *name, const u8 *args, u32 arglen, int *start_result)
{
    if (!g_intr_stack) {   /* first module (after the game's IOP reboot reset the heap) */
        IopHw_Init();
        /* The interrupt context's stack, a block of the original's heap that nothing here uses now; it stays, so
         * that the blocks after it are at the original's addresses. */
        g_intr_stack = IopKern_HeapAlloc(INTR_STACK_SIZE);
    }
    const u8 *elf = IopMem_Ptr(image);
    if (max < 0x34 || !is_elf(elf) || rd16i(elf + 0x10) != 0xFF80) {
        SND_LOGE("iopk", "%s is not an IRX module", name);
        return -1;
    }
    u32 phoff = rd32i(elf + 0x1C);
    u16 phentsize = rd16i(elf + 0x2A), phnum = rd16i(elf + 0x2C);
    u32 entry = 0, gp = 0, load_off = 0, filesz = 0, memsz = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u8 *ph = elf + phoff + i * phentsize;
        u32 type = rd32i(ph);
        if (type == 0x70000080u) {   /* .iopmod: the start function's offset and the module's gp */
            const u8 *m = elf + rd32i(ph + 4);
            entry = rd32i(m + 4);
            gp = rd32i(m + 8);
        } else if (type == 1) {      /* the loaded segment: the block's size */
            load_off = rd32i(ph + 4);
            filesz = rd32i(ph + 16);
            memsz = rd32i(ph + 20);
        }
    }
    if (!memsz || g_nmods == MAX_MODULES)
        return -1;
    const IopNativeModule *nm = g_iop_native_adx && !snd_strcmp(name, g_iop_native_adx->name) ? g_iop_native_adx
                              : g_iop_native_cse && !snd_strcmp(name, g_iop_native_cse->name) ? g_iop_native_cse : NULL;
    int libsd = !nm && g_iop_native_libsd && !snd_strcmp(name, "Sound_Device_Library");
    if (!nm && !libsd) {
        SND_LOGE("iopk", "%s has no native code", name);
        return -1;
    }
    u32 base = IopKern_HeapAlloc(memsz + 16);
    if (!base)
        return -1;
    if (nm) {   /* the block is the module's, its code is C (g_iop_native_adx, g_iop_native_cse) */
        snd_memset(IopMem_Ptr(base), 0, memsz);
        entry = nm->setup(base, memsz);
        gp = 0;
    } else {
        snd_memcpy(IopMem_Ptr(base), elf + load_off, filesz);
        snd_memset(IopMem_Ptr(base + filesz), 0, memsz - filesz);
        g_libsd_base = base;
        g_libsd_size = memsz;
        IopNative_Map(base + entry, LibsdStart);
    }
    g_nmods++;
    SND_LOGI("iopk", "IRX \"%s\" loaded at 0x%06X (0x%X bytes), entry 0x%06X", name, base, memsz, base + entry);

    /* Run the module's start(argc, argv) on a start thread, as modload's
     * StartModule does for a module loaded from a buffer: argv[0] is
     * "LBbyEE", then each NUL-terminated string of the load arguments. */
    if (arglen > 252)
        arglen = 252;
    u32 argc = 1;
    for (u32 i = 0; i < arglen; i++)
        argc += args[i] == 0 || i + 1 == arglen;
    u32 argv = IopKern_HeapAlloc(4 * (argc + 1) + 8 + arglen + 1);
    u32 strs = argv + 4 * (argc + 1);
    static const char argv0[] = "LBbyEE";
    for (u32 i = 0; i < sizeof(argv0); i++)
        IopMem_Write8(strs + i, (u8)argv0[i]);
    IopMem_Write32(argv, strs);
    u32 n = 1, at = strs + 8;
    for (u32 i = 0; i < arglen; i++) {
        if (i == 0 || args[i - 1] == 0)
            IopMem_Write32(argv + 4 * n++, at + i);
        IopMem_Write8(at + i, args[i]);
    }
    IopMem_Write8(at + arglen, 0);
    IopMem_Write32(argv + 4 * argc, 0);
    if (arglen) {
        char shown[256];
        u32 k = 0;
        for (u32 i = 0; i < arglen && k < sizeof(shown) - 1; i++)
            shown[k++] = args[i] ? (char)args[i] : ' ';
        shown[k] = 0;
        SND_LOGI("iopk", "%s: arguments \"%s\"", name, shown);
    }
    int res = 0, returned = 0;
    if (IopKern_StartModule(base + entry, base + gp, argc, argv, &res, &returned) < 0)
        return -1;
    if (!returned)
        SND_LOGW("iopk", "%s: start() did not return", name);
    if (start_result)
        *start_result = res & 3;
    SND_LOGI("iopk", "%s: start() -> %d", name, res);
    return g_nmods;
}

/* The modules that have native code (the original's iop_misc.c, is_lle_module: the sound modules), by the name in
 * their IRX header. */
int IopKern_HasNativeModule(const char *name)
{
    return !snd_strcmp(name, "Sound_Device_Library") || !snd_strcmp(name, "Capcom Sound Engine") ||
           !snd_strcmp(name, "CRI_ADX_Driver");
}

/* The module name of an IRX image (the .iopmod program header; the
 * original's iop_misc.c, irx_name): "?" when there is none. */
void IopKern_IrxName(u32 image, u32 max, char *out, u32 cap)
{
    const u8 *img = IopMem_Ptr(image);
    snd_strcpy_n(out, "?", cap);
    if (max < 0x34 || !is_elf(img))
        return;
    u32 phoff = rd32i(img + 0x1C);
    u16 phentsize = rd16i(img + 0x2A), phnum = rd16i(img + 0x2C);
    for (u32 i = 0; i < phnum && phoff + (i + 1) * phentsize <= max; i++) {
        const u8 *ph = img + phoff + i * phentsize;
        if (rd32i(ph) == 0x70000080u) {
            u32 off = rd32i(ph + 4);
            if (off + 0x1A < max) {
                u32 n = 0;
                for (; n + 1 < cap && off + 0x1A + n < max && img[off + 0x1A + n]; n++)
                    out[n] = (char)img[off + 0x1A + n];
                out[n] = 0;
            }
            return;
        }
    }
}
