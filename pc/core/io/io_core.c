/* The io start, its log and the disc's reads (io.h). */
#include "host.h"
#include "io/io.h"
#include "io/io_host.h"
#include "io/io_pad.h"
#include "kernel/kern.h"

/* ------------------------------------------------------------ logs */

static u32 PutNum(char *out, u32 n, u32 cap, u32 v, u32 base, int neg, u32 width, char pad, int upper)
{
    char tmp[12];
    u32 k = 0;
    do {
        u32 d = v % base;
        tmp[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    if (neg)
        tmp[k++] = '-';
    while (k < width && k < sizeof(tmp))
        tmp[k++] = pad;
    while (k && n + 1 < cap)
        out[n++] = tmp[--k];
    return n;
}

u32 Io_Format(char *out, u32 cap, const char *fmt, u32 a, u32 b)
{
    u32 n = 0, arg = 0;
    if (!cap)
        return 0;
    for (const char *p = fmt; *p && n + 1 < cap; p++) {
        if (*p != '%') {
            out[n++] = *p;
            continue;
        }
        p++;
        char pad = ' ';
        u32 width = 0;
        if (*p == '0')
            pad = '0';
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (u32)(*p++ - '0');
        u32 v = arg == 0 ? a : b;
        switch (*p) {
        case 'd':
            arg++;
            n = (s32)v < 0 ? PutNum(out, n, cap, 0u - v, 10, 1, width, pad, 0) : PutNum(out, n, cap, v, 10, 0, width, pad, 0);
            break;
        case 'u':
            arg++;
            n = PutNum(out, n, cap, v, 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            arg++;
            n = PutNum(out, n, cap, v, 16, 0, width, pad, *p == 'X');
            break;
        case 's':
            arg++;
            for (const char *s = v ? (const char *)v : "(null)"; *s && n + 1 < cap; s++)
                out[n++] = *s;
            break;
        case '%':
            out[n++] = '%';
            break;
        default:
            if (!*p)
                p--;
            break;
        }
    }
    out[n] = 0;
    return n;
}

void Io_Log(const char *fmt, u32 a, u32 b)
{
    char line[400] = "io: ";
    Io_Format(line + 4, sizeof(line) - 4, fmt, a, b);
    Host_Log(line);
}

/* ------------------------------------------------------------ the disc */

int Io_DiscFind(const char *path, u32 *lba, u32 *size)
{
    return Host_DiscFind(path, lba, size);
}

int Io_DiscPackOpen(const char *dir)
{
    return Host_DiscPackOpen(dir);
}

const char *Io_DiscPackName(void)
{
    return Host_DiscPackName();
}

int Io_DiscPackHash(char hex[65])
{
    return Host_DiscPackHash(hex);
}

int Io_DiscPackFind(const char *key, u32 *lba, u32 *size)
{
    unsigned int l = 0, n = 0;
    if (Host_DiscPackFind(key, &l, &n) != 0)
        return -1;
    *lba = l;
    *size = n;
    return 0;
}

int Io_DiscRead(u32 lba, u32 count, void *dst)
{
    return Host_DiscRead(lba, count, dst);
}

int Io_DiscLocation(u32 lba, const char **path, u64 *offset)
{
    unsigned long long off = 0;
    const char *p = 0;
    if (Host_DiscLocation(lba, &p, &off) != 0)
        return -1;
    *path = p;
    *offset = (u64)off;
    return 0;
}

int Io_DiscSideFile(const char *name, void **data, u32 *size)
{
    unsigned int n = 0;
    void *p = 0;
    if (Host_DiscSideFile(name, &p, &n) != 0)
        return -1;
    *data = p;
    *size = n;
    return 0;
}

int Io_DiscReadSectors(u32 lba, u32 count, void *dst)
{
    int r = Host_DiscRead(lba, count, dst);
    if (r == 0)
        ;
    return r;
}

int Io_DiscReadBytes(u32 lba, u32 pos, u32 size, void *dst)
{
    static u8 sec[IO_SECTOR];
    u8 *out = dst;
    while (size) {
        u32 s = lba + pos / IO_SECTOR, off = pos % IO_SECTOR;
        if (off == 0 && size >= IO_SECTOR) {   /* whole sectors straight into dst */
            u32 n = size / IO_SECTOR;
            if (Io_DiscRead(s, n, out) != 0)
                return -1;
            out += n * IO_SECTOR;
            pos += n * IO_SECTOR;
            size -= n * IO_SECTOR;
            continue;
        }
        u32 n = IO_SECTOR - off < size ? IO_SECTOR - off : size;
        if (Io_DiscRead(s, 1, sec) != 0)
            return -1;
        __builtin_memcpy(out, sec + off, n);
        out += n;
        pos += n;
        size -= n;
    }
    return 0;
}

/* ------------------------------------------------------------ the start */

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void Io_RegisterServices(void)
{
    IoFio_Register();
    IoCdvd_Register();
    IoMc_Register();
    IoPad_Register();
    IoKb_Register();
}

/* Pointers into the process's arguments: the process's, not a save state's (kernel/kern.h). */
static const char *g_optDisc KERN_HOSTBSS, *g_optCard[2] KERN_HOSTBSS, *g_optPack KERN_HOSTBSS;

s32 Io_Option(s32 argc, char **argv, s32 i)
{
    if (Io_KbOption(argc, argv, i))   /* --usb-keyboard */
        return 1;
    if (i + 1 >= argc)
        return 0;
    if (StrEq(argv[i], "--disc"))
        g_optDisc = argv[i + 1];
    else if (StrEq(argv[i], "--pack"))
        g_optPack = argv[i + 1];
    else if (StrEq(argv[i], "--memcard1"))
        g_optCard[0] = argv[i + 1];
    else if (StrEq(argv[i], "--memcard2"))
        g_optCard[1] = argv[i + 1];
    else
        return 0;
    return 2;
}

int Io_Start(void)
{
    const char *disc = g_optDisc;
    for (int p = 0; p < 2; p++)
        IoMc_Load(p, g_optCard[p]);   /* NULL: empty */
    if (!disc)
        disc = Host_Getenv("MH2_DISC");
    int r = -1;
    if (disc) {
        r = Host_DiscOpen(disc);
    } else {
        static const char *const tries[] = {"mh2-disc.arc", "build/port/mh2-disc.arc"};
        for (u32 i = 0; i < 2 && r == -1; i++)
            r = Host_DiscOpen(disc = tries[i]);
    }
    if (r != 0)
        Io_Log("no game archive (%s): give --disc FILE or MH2_DISC, or make it in the launcher (the Game archive page: Make the archive from a disc image)",
               (u32)disc, 0);
    if (r == 0 && g_optPack && Host_DiscPackOpen(g_optPack) != 0)
        Host_DiscPackRefuse();   /* a pack that was asked for is never silently left out: the run ends, and says why */
    Io_RegisterServices();
    return r == 0 ? 0 : -1;
}

int Io_Init(int argc, char **argv)
{
    for (s32 i = 1; i < argc; i++) {
        s32 n = Io_Option(argc, argv, i);
        if (n)
            i += n - 1;
    }
    return Io_Start();
}
