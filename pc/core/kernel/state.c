/*
 * Save states: the core's side (state.h).
 *
 * The file is a header and sections, each deflated (level 1):
 *   "MEM"    the pages of the writable memory that are not zero, as records {address, pages, 4096 bytes a page}
 *            in rising address order: the image's .data and .bss, the low pool's fibers and its arena;
 *   owners'  one section a registered owner (a card image, a handle table);
 *   "HOST"   the host's part, if the host side gives one (the renderer's memory).
 * The memory leaves out the process's own data (the host seam's sections, the words that were declared) and
 * zero pages; a load makes those pages zero again, so it replaces the memory, it does not add to it.
 *
 * A save runs at a VBlank's start, in the kernel's event, on whichever fiber the event runs on. That fiber is
 * suspended with the same switch the kernel uses (its registers go on its own stack, which is in the copy), and
 * the copy is made on a stack outside the state (fiber.c, KFiber_RunOutside). A load puts the memory back from
 * the same outside stack, then switches into the recorded context: the save call returns there a second time,
 * as setjmp does, and the kernel goes on as after any event.
 *
 * Everything the state needs to resume that is not in the memory is in the sections: the host's own handles
 * (the disc, the card file, the sound device, the connection to the host) are the process's and stay.
 */
#include "ipc/core_ipc.h"
#include "kernel/kern_int.h"
#include "kernel/khost.h"
#include "kernel/khost_state.h"
#include "kernel/klog.h"
#include "kernel/state.h"
#include "pal_arena.h"
#include "pal_camera.h"
#include "pal_hub.h"
#include "pal.h"
#include "pal_core.h"
#include "pal_fpmodel.h"
#include "pal_freecam.h"
#include "io/io.h"

#define PAGE 4096u
#define ST_VERSION 1u
#define MAX_OWNERS 16
#define MAX_WORDS 64
#define MAX_SECTIONS 24
#define MAX_SETTINGS 24
#define MAX_RAW (1u << 30)         /* a section larger than this is not ours */
#define MAX_SPANS 24

typedef struct StSection {
    char tag[8];
    u32 rawSize, storedSize, adler, offset;
} StSection;

typedef struct StSetting {
    char tag[8];
    u32 value;
} StSetting;

typedef struct StHeader {
    char magic[8];
    u32 version, headerSize;
    u64 codeHash;
    u64 vblank, now, calendar;
    u32 sp;                    /* the suspended context */
    u32 lowLo[2], lowHi[2];    /* the low pool's regions in use */
    char hostId[32];
    u32 nSettings, nSections;
    StSetting settings[MAX_SETTINGS];
    StSection sections[MAX_SECTIONS];
} StHeader;

struct KStateOut {
    HostBuf b;
};

typedef struct Span {
    u32 lo, hi;
} Span;

/* The module's own data belongs to the process: a load must not change it. */
static const KStateOwner *sOwners[MAX_OWNERS] KERN_HOSTBSS;
static int sNumOwners KERN_HOSTBSS;
static Span sWords[MAX_WORDS] KERN_HOSTBSS;
static int sNumWords KERN_HOSTBSS;
static const KStateHost *sHost KERN_HOSTBSS;
static int sReq KERN_HOSTBSS;               /* 0 none, 1 save, 2 load */
static char sReqPath[1024] KERN_HOSTBSS;
static const char *sResult KERN_HOSTBSS;
static u64 sCodeHash KERN_HOSTBSS;
static const char *sFail KERN_HOSTBSS;      /* the loader's reason */
static StHeader sHdr KERN_HOSTBSS;
static HostBuf sFileIn KERN_HOSTBSS;
int g_kern_state_card KERN_HOSTBSS;
/* The host's request in flight (core_ipc.h, Ipc_StateRequest): answered when the VBlank start that took it is over. Words
 * of the host side, which a load leaves as they are: a load returns to KState_VBlankStart from the context of the save. */
static int sHostReq KERN_HOSTBSS;
static int sHeld KERN_HOSTBSS;        /* a held request has been logged */
static u64 sDoneVblank KERN_HOSTBSS;     /* the VBlank the answer names: the save's, or the loaded state's */

/* The C library's string functions are cdecl; these are the game-facing convention's own. */
static int StrLen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int StrNCmp(const char *a, const char *b, int n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b) {
            return (unsigned char)*a - (unsigned char)*b;
        }
        if (!*a) {
            return 0;
        }
    }
    return 0;
}

static void StrNCopy(char *dst, const char *src, int cap)    /* at most cap - 1 characters, then 0 */
{
    int i;
    for (i = 0; i < cap - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

/* -------------------------------------------------------- registration */

void Kern_StateRegister(const KStateOwner *o)
{
    int i;
    for (i = 0; i < sNumOwners; i++) {
        if (sOwners[i] == o) {
            return;
        }
    }
    if (sNumOwners == MAX_OWNERS) {
        Pal_Fatal("state: more than 16 owners");
    }
    sOwners[sNumOwners++] = o;
}

void Kern_StateHostWord(const void *p, u32 size)
{
    int i;
    u32 lo = (u32)(unsigned long)p;
    for (i = 0; i < sNumWords; i++) {
        if (sWords[i].lo == lo && sWords[i].hi == lo + size) {
            return;
        }
    }
    if (sNumWords == MAX_WORDS) {
        Pal_Fatal("state: more than 64 host words");
    }
    sWords[sNumWords].lo = lo;
    sWords[sNumWords++].hi = lo + size;
}

void Kern_StateSetHost(const KStateHost *h)
{
    sHost = h;
}

__attribute__((weak)) int Kern_StateBlocked(void)
{
    return 0;
}

void Kern_StatePut(KStateOut *out, const void *data, u32 size)
{
    if (HostBuf_Append(&out->b, data, size) != 0) {
        Pal_Fatal("state: out of memory");
    }
}

/* ------------------------------------------------------------ the ranges */

static HostStateLayout sLay KERN_HOSTBSS;
static Span sEx[MAX_WORDS + HOSTSTATE_RANGES] KERN_HOSTBSS;     /* what a state skips, sorted, joined */
static int sNumEx KERN_HOSTBSS;

static void SortSpans(Span *s, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        Span t = s[i];
        for (j = i; j > 0 && s[j - 1].lo > t.lo; j--) {
            s[j] = s[j - 1];
        }
        s[j] = t;
    }
}

static void BuildExclusions(void)
{
    Span all[MAX_WORDS + HOSTSTATE_RANGES];
    int n = 0, i;
    HostState_Layout(&sLay);
    for (i = 0; i < sLay.nHost; i++) {
        all[n].lo = sLay.hostLo[i];
        all[n++].hi = sLay.hostHi[i];
    }
    for (i = 0; i < sNumWords; i++) {
        all[n++] = sWords[i];
    }
    SortSpans(all, n);
    sNumEx = 0;
    for (i = 0; i < n; i++) {
        if (all[i].lo >= all[i].hi) {
            continue;
        }
        if (sNumEx && all[i].lo <= sEx[sNumEx - 1].hi) {
            if (all[i].hi > sEx[sNumEx - 1].hi) {
                sEx[sNumEx - 1].hi = all[i].hi;
            }
        } else {
            sEx[sNumEx++] = all[i];
        }
    }
}

/* The spans a state covers, page-aligned: the image's data and the low pool's regions (extended to lowHi). */
static int BuildSpans(Span *out, const u32 lowLo[2], const u32 lowHi[2])
{
    int n = 0, i;
    for (i = 0; i < sLay.nData; i++) {
        out[n].lo = sLay.dataLo[i] & ~(PAGE - 1);
        out[n++].hi = (sLay.dataHi[i] + PAGE - 1) & ~(PAGE - 1);
    }
    for (i = 0; i < 2; i++) {
        if (lowHi[i] > lowLo[i]) {
            out[n].lo = lowLo[i] & ~(PAGE - 1);
            out[n++].hi = (lowHi[i] + PAGE - 1) & ~(PAGE - 1);
        }
    }
    SortSpans(out, n);
    return n;
}

/* How a page meets the skipped ranges: 0 not at all, 1 partly, 2 wholly. sEx is walked by *cursor, for pages
 * in rising order. */
static int PageClass(u32 a, int *cursor)
{
    int k = *cursor;
    while (k < sNumEx && sEx[k].hi <= a) {
        k++;
    }
    *cursor = k;
    if (k < sNumEx && sEx[k].lo < a + PAGE) {
        return sEx[k].lo <= a && sEx[k].hi >= a + PAGE ? 2 : 1;
    }
    return 0;
}

/* The page's bytes that are not skipped, into dst (the skipped ones as 0 or as keep gives them). */
static void MaskPage(u8 *dst, const u8 *src, u32 a, int cursor, const u8 *keep)
{
    int k;
    __builtin_memcpy(dst, src, PAGE);
    for (k = cursor; k < sNumEx && sEx[k].lo < a + PAGE; k++) {
        u32 lo = sEx[k].lo > a ? sEx[k].lo : a;
        u32 hi = sEx[k].hi < a + PAGE ? sEx[k].hi : a + PAGE;
        if (hi > lo) {
            if (keep) {
                __builtin_memcpy(dst + (lo - a), keep + (lo - a), hi - lo);
            } else {
                __builtin_memset(dst + (lo - a), 0, hi - lo);
            }
        }
    }
}

/* Zero except the skipped bytes. */
static void ZeroPage(u8 *dst, u32 a, int cursor)
{
    u32 at = a;
    int k;
    for (k = cursor; k < sNumEx && sEx[k].lo < a + PAGE; k++) {
        if (sEx[k].lo > at) {
            __builtin_memset(dst + (at - a), 0, sEx[k].lo - at);
        }
        if (sEx[k].hi > at) {
            at = sEx[k].hi < a + PAGE ? sEx[k].hi : a + PAGE;
        }
    }
    if (at < a + PAGE) {
        __builtin_memset(dst + (at - a), 0, a + PAGE - at);
    }
}

static int IsZero(const u8 *p)
{
    const u64 *w = (const u64 *)p;
    u32 i;
    u64 acc = 0;
    for (i = 0; i < PAGE / 8; i += 8) {
        acc |= w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7];
    }
    return acc == 0;
}

static u64 CodeHash(void)
{
    if (!sCodeHash) {
        const u32 *p = (const u32 *)(unsigned long)sLay.codeLo;
        const u32 *e = (const u32 *)(unsigned long)(sLay.codeHi & ~3u);
        u64 h = 0xcbf29ce484222325ull;
        for (; p < e; p++) {
            h = (h ^ *p) * 0x100000001b3ull;
            h ^= h >> 29;
        }
        sCodeHash = h ? h : 1;
    }
    return sCodeHash;
}

/* ------------------------------------------------------------------ save */

static void AddSetting(StHeader *h, const char *tag, u32 value)
{
    StSetting *s;
    if (h->nSettings == MAX_SETTINGS) {
        Pal_Fatal("state: too many settings");
    }
    s = &h->settings[h->nSettings++];
    __builtin_memset(s, 0, sizeof(*s));
    StrNCopy(s->tag, tag, 8);
    s->value = value;
}

extern const void *g_iop_native_cse;   /* the audio engine (snd) */

int Kern_StateCardEmbedded(void)
{
    return g_kern_state_card;
}

/* The settings a state is good for: the same at the load, or it is refused. */
static void Settings(StHeader *h)
{
    int i;
    h->nSettings = 0;
    AddSetting(h, "audio", g_iop_native_cse ? 1u : 0u);
    AddSetting(h, "keyboard", (u32)Io_KbConnected());
    AddSetting(h, "wide", (u32)Pal_WidescreenLevel());
    AddSetting(h, "fpu", (u32)Pal_FpuModelId());   /* the float model: a state is a point of one model's play */
    if (Pal_OfflineHub()) {   /* only when on, so a state made without it is as it was */
        AddSetting(h, "hub", 1u);
    }
    {   /* a text pack's hash, as eight words (none: no setting, so a state without a pack is as it was) */
        char hex[65];
        if (Io_DiscPackHash(hex) == 0) {
            static const char tags[8][8] = {"tpack0", "tpack1", "tpack2", "tpack3", "tpack4", "tpack5", "tpack6", "tpack7"};
            for (i = 0; i < 8; i++) {
                u32 v = 0;
                int k;
                for (k = 0; k < 8; k++) {
                    char c = hex[i * 8 + k];
                    v = v << 4 | (u32)(c <= '9' ? c - '0' : c - 'a' + 10);
                }
                AddSetting(h, tags[i], v);
            }
        }
    }
    for (i = 0; i < sNumOwners; i++) {
        if (sOwners[i]->setting) {
            AddSetting(h, sOwners[i]->tag, sOwners[i]->setting());
        }
    }
}

static int AddSection(StHeader *h, HostBuf *file, const char *tag, const void *raw, u32 n)
{
    StSection *s;
    int stored;
    if (h->nSections == MAX_SECTIONS) {
        return -1;
    }
    s = &h->sections[h->nSections];
    __builtin_memset(s, 0, sizeof(*s));
    StrNCopy(s->tag, tag, 8);
    s->rawSize = n;
    s->adler = HostState_Adler(raw, n);
    s->offset = file->n;
    stored = HostState_Deflate(raw, n, file, 1);
    if (stored < 0) {
        return -1;
    }
    s->storedSize = (u32)stored;
    h->nSections++;
    return 0;
}

typedef struct SaveStats {
    u32 rawBytes, pages, storedBytes, ms;
} SaveStats;
static SaveStats sStats KERN_HOSTBSS;

/* Appends the memory's records to raw. */
static int CopyMemory(HostBuf *raw, const Span *spans, int nSpans, u32 *pagesOut)
{
    int si, cursor = 0;
    u32 pages = 0, runAt = 0, runPages = 0;
    HostBuf run = {0, 0, 0};          /* the pages of the run in progress */
    u8 tmp[PAGE];
    for (si = 0; si < nSpans; si++) {
        u32 a;
        for (a = spans[si].lo; a < spans[si].hi; a += PAGE) {
            const u8 *src = (const u8 *)(unsigned long)a;
            int cls = PageClass(a, &cursor);
            if (cls == 2) {
                src = 0;
            } else if (cls == 1) {
                MaskPage(tmp, src, a, cursor, 0);
                src = tmp;
            }
            if (src && !IsZero(src)) {
                if (runPages && runAt + runPages * PAGE != a) {
                    u32 hdr[2];
                    hdr[0] = runAt;
                    hdr[1] = runPages;
                    if (HostBuf_Append(raw, hdr, 8) || HostBuf_Append(raw, run.p, run.n)) {
                        HostBuf_Free(&run);
                        return -1;
                    }
                    run.n = 0;
                    runPages = 0;
                }
                if (!runPages) {
                    runAt = a;
                }
                if (HostBuf_Append(&run, src, PAGE) != 0) {
                    HostBuf_Free(&run);
                    return -1;
                }
                runPages++;
                pages++;
            }
        }
    }
    if (runPages) {
        u32 hdr[2];
        hdr[0] = runAt;
        hdr[1] = runPages;
        if (HostBuf_Append(raw, hdr, 8) || HostBuf_Append(raw, run.p, run.n)) {
            HostBuf_Free(&run);
            return -1;
        }
    }
    HostBuf_Free(&run);
    *pagesOut = pages;
    return 0;
}

static void SaveMain(void *arg)
{
    const char *path = (const char *)arg;
    StHeader *h = &sHdr;
    HostBuf file = {0, 0, 0}, raw = {0, 0, 0};
    u32 t0 = HostState_Ms(), pages = 0, lowLo[2], lowHi[2], i;
    Span spans[MAX_SPANS];
    int nSpans;
    sFail = 0;
    __builtin_memset(h, 0, sizeof(*h));
    __builtin_memcpy(h->magic, "MH2STATE", 8);
    h->version = ST_VERSION;
    h->headerSize = (u32)sizeof(*h);
    BuildExclusions();
    h->codeHash = CodeHash();
    h->vblank = Kern_VblankCount() + 1;      /* the VBlank about to start */
    h->now = Kern_Now();
    h->calendar = Kern_CalendarSeconds();
    h->sp = (u32)(unsigned long)KFiber_OutsideSp();
    Settings(h);
    if (sHost && sHost->identity) {
        StrNCopy(h->hostId, sHost->identity(), 32);
    }
    if (Pal_LowRegions(lowLo, lowHi) != 2) {
        sFail = "the low pool is not mapped";
        return;
    }
    for (i = 0; i < 2; i++) {
        h->lowLo[i] = lowLo[i];
        h->lowHi[i] = lowHi[i];
    }
    nSpans = BuildSpans(spans, lowLo, lowHi);
    if (HostBuf_Append(&file, h, (u32)sizeof(*h)) != 0) {
        sFail = "out of memory";
        return;
    }
    file.n = (u32)sizeof(*h);
    if (CopyMemory(&raw, spans, nSpans, &pages) != 0 || AddSection(h, &file, "MEM", raw.p, raw.n) != 0) {
        sFail = "out of memory";
        HostBuf_Free(&raw);
        HostBuf_Free(&file);
        return;
    }
    sStats.rawBytes = raw.n;
    sStats.pages = pages;
    HostBuf_Free(&raw);
    for (i = 0; i < (u32)sNumOwners; i++) {
        KStateOut out;
        int rc;
        out.b.p = 0;
        out.b.n = out.b.cap = 0;
        if (sOwners[i]->save) {
            sOwners[i]->save(&out);
        }
        rc = AddSection(h, &file, sOwners[i]->tag, out.b.p, out.b.n);
        sStats.rawBytes += out.b.n;
        HostBuf_Free(&out.b);
        if (rc != 0) {
            sFail = "too many sections";
            HostBuf_Free(&file);
            return;
        }
    }
    if (sHost && sHost->get) {
        u32 n = 0;
        const void *d = sHost->get(&n);
        if (!d && sHost->error && sHost->error()) {
            sFail = sHost->error();
            HostBuf_Free(&file);
            return;
        }
        if (d && AddSection(h, &file, "HOST", d, n) != 0) {
            sFail = "the host's part could not be stored";
            HostBuf_Free(&file);
            return;
        }
        sStats.rawBytes += n;
    }
    __builtin_memcpy(file.p, h, sizeof(*h));
    if (HostState_WriteFile(path, file.p, file.n) != 0) {
        sFail = "the file could not be written";
    }
    sStats.storedBytes = file.n;
    sStats.ms = HostState_Ms() - t0;
    HostBuf_Free(&file);
}

/* ------------------------------------------------------------------ load */

static const StSection *FindSection(const StHeader *h, const char *tag)
{
    u32 i;
    for (i = 0; i < h->nSections; i++) {
        if (!StrNCmp(h->sections[i].tag, tag, 8)) {
            return &h->sections[i];
        }
    }
    return 0;
}

/* Checks a header against this process: 0 when the state is good for it. */
static const char *CheckHeader(const StHeader *h, u32 fileSize)
{
    StHeader cur;
    u32 i, j;
    if (fileSize < sizeof(*h) || __builtin_memcmp(h->magic, "MH2STATE", 8) != 0) {
        return "not a save state";
    }
    if (h->version != ST_VERSION || h->headerSize != sizeof(*h)) {
        return "a save state of another version";
    }
    if (h->codeHash != CodeHash()) {
        return "made by another build of the game";
    }
    if (h->nSections > MAX_SECTIONS || h->nSettings > MAX_SETTINGS) {
        return "damaged";
    }
    __builtin_memset(&cur, 0, sizeof(cur));
    Settings(&cur);
    for (i = 0; i < cur.nSettings; i++) {
        for (j = 0; j < h->nSettings && __builtin_memcmp(h->settings[j].tag, cur.settings[i].tag, 8) != 0; j++) {
        }
        if (j == h->nSettings || h->settings[j].value != cur.settings[i].value) {
            if (!StrNCmp(cur.settings[i].tag, "audio", 8)) {
                return "made with another audio engine";
            }
            if (!StrNCmp(cur.settings[i].tag, "keyboard", 8)) {
                return "made with the USB keyboard on or off the other way";
            }
            if (!StrNCmp(cur.settings[i].tag, "wide", 8)) {
                return "made with another widescreen setting";
            }
            if (!StrNCmp(cur.settings[i].tag, "tpack", 5)) {
                return "made with another translation pack";
            }
            if (!StrNCmp(cur.settings[i].tag, "fpu", 8)) {
                return "made with the other float model (the console's arithmetic, or the older one)";
            }
            if (!StrNCmp(cur.settings[i].tag, "hub", 8)) {
                return "made without the offline hub";
            }
            return "made with other settings";
        }
    }
    if (h->nSettings != cur.nSettings) {
        for (j = 0; j < h->nSettings; j++) {
            if (!StrNCmp(h->settings[j].tag, "tpack", 5)) {
                return "made with a translation pack";
            }
            if (!StrNCmp(h->settings[j].tag, "hub", 8)) {
                return "made with the offline hub";
            }
        }
        return "made with other settings";
    }
    for (i = 0; i < (u32)sNumOwners; i++) {
        if (!FindSection(h, sOwners[i]->tag)) {
            return "lacks a part this game needs";
        }
    }
    for (i = 0; i < h->nSections; i++) {
        const StSection *s = &h->sections[i];
        if (s->offset < sizeof(*h) || s->offset > fileSize || s->storedSize > fileSize - s->offset ||
            s->rawSize > MAX_RAW) {
            return "damaged";
        }
    }
    if (!FindSection(h, "MEM")) {
        return "damaged";
    }
    if (sHost && sHost->get) {
        if (!FindSection(h, "HOST")) {
            return "made without the picture";
        }
        if (StrNCmp(h->hostId, sHost->identity ? sHost->identity() : "", 31) != 0) {
            return "made by another build of the renderer";
        }
    }
    {
        u32 lowLo[2], lowHi[2];
        if (Pal_LowRegions(lowLo, lowHi) != 2 || lowLo[0] != h->lowLo[0] || lowLo[1] != h->lowLo[1]) {
            return "made with the low pool elsewhere";
        }
    }
    return 0;
}

/* The section's bytes, inflated and checked; 0 on failure. */
static u8 *OpenSection(const StSection *s)
{
    u8 *raw = (u8 *)Host_Alloc(s->rawSize ? s->rawSize : 1);
    if (!raw) {
        return 0;
    }
    if (HostState_Inflate(sFileIn.p + s->offset, s->storedSize, raw, s->rawSize) != 0 ||
        HostState_Adler(raw, s->rawSize) != s->adler) {
        Host_Free(raw);
        return 0;
    }
    return raw;
}

/* Puts the memory back: the records over the covered pages, zero over the rest, the skipped bytes untouched. */
static const char *ApplyMemory(const u8 *raw, u32 rawSize, const Span *spans, int nSpans)
{
    u32 at = 0, recAt = 0, recPages = 0, recEnd = 0;
    const u8 *recData = 0;
    int si, cursor = 0;
    /* the records are in rising order and inside the spans: checked before any page is written */
    {
        u32 last = 0, p = 0;
        while (p < rawSize) {
            u32 a, n, e;
            int ok = 0;
            if (rawSize - p < 8) {
                return "damaged";
            }
            __builtin_memcpy(&a, raw + p, 4);
            __builtin_memcpy(&n, raw + p + 4, 4);
            if (!n || n > (rawSize - p - 8) / PAGE || (a & (PAGE - 1))) {
                return "damaged";
            }
            e = a + n * PAGE;
            if (a < last || e < a) {
                return "damaged";
            }
            for (si = 0; si < nSpans; si++) {
                ok |= a >= spans[si].lo && e <= spans[si].hi;
            }
            if (!ok) {
                return "damaged";
            }
            last = e;
            p += 8 + n * PAGE;
        }
    }
    for (si = 0; si < nSpans; si++) {
        u32 a;
        for (a = spans[si].lo; a < spans[si].hi; a += PAGE) {
            u8 *dst = (u8 *)(unsigned long)a;
            const u8 *src = 0;
            int cls = PageClass(a, &cursor);
            while (a >= recEnd && at < rawSize) {      /* the next record */
                __builtin_memcpy(&recAt, raw + at, 4);
                __builtin_memcpy(&recPages, raw + at + 4, 4);
                recData = raw + at + 8;
                recEnd = recAt + recPages * PAGE;
                at += 8 + recPages * PAGE;
            }
            if (recData && a >= recAt && a < recEnd) {
                src = recData + (a - recAt);
            }
            if (cls == 2) {
                continue;
            }
            if (src) {
                if (cls == 1) {
                    u8 tmp[PAGE];
                    MaskPage(tmp, src, a, cursor, dst);
                    __builtin_memcpy(dst, tmp, PAGE);
                } else {
                    __builtin_memcpy(dst, src, PAGE);
                }
            } else if (!IsZero(dst)) {
                if (cls == 1) {
                    ZeroPage(dst, a, cursor);
                } else {
                    __builtin_memset(dst, 0, PAGE);
                }
            }
        }
    }
    return 0;
}

/* On the outside stack. Returns only when the state was refused; a good one ends in the resumed context. */
static void LoadMain(void *arg)
{
    const char *path = (const char *)arg;
    StHeader *h = &sHdr;
    u32 t0 = HostState_Ms(), i, lowLo[2], lowHi[2];
    Span spans[MAX_SPANS];
    int nSpans;
    u8 *mem;
    const StSection *ms;
    const char *why;
    sFail = 0;
    HostBuf_Free(&sFileIn);
    BuildExclusions();
    if (HostState_ReadFile(path, &sFileIn) != 0) {
        sFail = "the file could not be read";
        return;
    }
    if (sFileIn.n >= sizeof(*h)) {
        __builtin_memcpy(h, sFileIn.p, sizeof(*h));
    }
    why = CheckHeader(h, sFileIn.n);
    if (why) {
        sFail = why;
        HostBuf_Free(&sFileIn);
        return;
    }
    ms = FindSection(h, "MEM");
    mem = OpenSection(ms);
    if (!mem) {
        sFail = "damaged";
        HostBuf_Free(&sFileIn);
        return;
    }
    if (Pal_LowRegions(lowLo, lowHi) != 2) {
        sFail = "the low pool is not mapped";
        Host_Free(mem);
        HostBuf_Free(&sFileIn);
        return;
    }
    for (i = 0; i < 2; i++) {      /* the pages of either process */
        lowHi[i] = lowHi[i] > h->lowHi[i] ? lowHi[i] : h->lowHi[i];
    }
    nSpans = BuildSpans(spans, lowLo, lowHi);
    for (i = 0; i < (u32)sNumOwners; i++) {     /* the owners that can refuse, while nothing has changed */
        const StSection *os = FindSection(h, sOwners[i]->tag);
        u8 *d;
        if (!sOwners[i]->accept) {
            continue;
        }
        d = OpenSection(os);
        if (!d) {
            sFail = "damaged";
        } else {
            sFail = sOwners[i]->accept(d, os->rawSize);
            Host_Free(d);
        }
        if (sFail) {
            Host_Free(mem);
            HostBuf_Free(&sFileIn);
            return;
        }
    }
    for (i = 0; i < (u32)sNumOwners; i++) {
        if (sOwners[i]->prepare) {
            sOwners[i]->prepare();
        }
    }
    why = ApplyMemory(mem, ms->rawSize, spans, nSpans);
    Host_Free(mem);
    if (why) {
        Pal_Fatal("state: the memory was damaged after the load began");
    }
    for (i = 0; i < (u32)sNumOwners; i++) {
        const StSection *os = FindSection(h, sOwners[i]->tag);
        u8 *d = OpenSection(os);
        if (!d || (sOwners[i]->load && sOwners[i]->load(d, os->rawSize) != 0)) {
            Pal_Fatal("state: a part of the state could not be put back");
        }
        Host_Free(d);
    }
    if (sHost && sHost->set) {
        const StSection *hs = FindSection(h, "HOST");
        u8 *d = hs ? OpenSection(hs) : 0;
        if (!d || sHost->set(d, hs->rawSize) != 0) {
            Pal_Fatal("state: the host's part could not be put back");
        }
        Host_Free(d);
    }
    sStats.ms = HostState_Ms() - t0;
    sStats.storedBytes = sFileIn.n;
    sStats.rawBytes = ms->rawSize;
    KFiber_ResumeAt((void *)(unsigned long)h->sp);
}

/* In the restored context, after the save call returned a second time. */
static void AfterLoad(void)
{
    int i;
    for (i = 0; i < sNumOwners; i++) {
        if (sOwners[i]->resume) {
            sOwners[i]->resume();
        }
    }
    HostBuf_Free(&sFileIn);
    Pal_FreezeCancel();   /* a pause's snapshot belongs to the game this state replaced */
    sDoneVblank = sHdr.vblank;
    KLog("state: loaded VBlank %llu (%u bytes, %u ms)", (u64)sHdr.vblank, sStats.storedBytes, sStats.ms);
    sResult = 0;
}

/* --------------------------------------------------- the pause's snapshot */

/* An in-memory state for the freecam's pause (pal_freecam.c: the state loop): the pages of the
 * file state's memory section kept on the C heap, and the renderer's block, with the context to return to. It is made
 * and put back in the game's own thread at a step's start, not at a VBlank's start, and holds no owner's section (the
 * card is outside the image: the pause does not begin with a card request running). Its words are the process's. */
static HostBuf sMem KERN_HOSTBSS;          /* the records of CopyMemory */
static HostBuf sMemGs KERN_HOSTBSS;        /* the renderer's block, when the host side gives one */
static u32 sMemSp KERN_HOSTBSS;
static int sMemHave KERN_HOSTBSS;
static u32 sMemLowHi[2] KERN_HOSTBSS;
static u32 sMemLowLo[2] KERN_HOSTBSS;
static const char *sMemFail KERN_HOSTBSS;
static int sMemWithGs KERN_HOSTBSS;        /* the restore in progress puts the renderer back too */

static void MemSaveMain(void *arg)
{
    Span spans[MAX_SPANS];
    int nSpans;
    u32 pages = 0;
    (void)arg;
    sMemFail = 0;
    sMemHave = 0;
    HostBuf_Free(&sMem);
    HostBuf_Free(&sMemGs);
    BuildExclusions();
    if (Pal_LowRegions(sMemLowLo, sMemLowHi) != 2) {
        sMemFail = "the low pool is not mapped";
        return;
    }
    nSpans = BuildSpans(spans, sMemLowLo, sMemLowHi);
    if (CopyMemory(&sMem, spans, nSpans, &pages) != 0) {
        sMemFail = "out of memory";
        HostBuf_Free(&sMem);
        return;
    }
    if (sHost && sHost->get) {
        u32 n = 0;
        const void *d = sHost->get(&n);
        if (!d && sHost->error && sHost->error()) {
            sMemFail = sHost->error();
            HostBuf_Free(&sMem);
            return;
        }
        if (d && HostBuf_Append(&sMemGs, d, n) != 0) {
            sMemFail = "out of memory";
            HostBuf_Free(&sMem);
            return;
        }
    }
    sMemSp = (u32)(unsigned long)KFiber_OutsideSp();
    sMemHave = 1;
}

static void MemLoadMain(void *arg)
{
    Span spans[MAX_SPANS];
    int nSpans, i;
    u32 lowLo[2], lowHi[2];
    const char *why;
    (void)arg;
    BuildExclusions();
    if (!sMemHave || Pal_LowRegions(lowLo, lowHi) != 2) {
        return;
    }
    for (i = 0; i < 2; i++) {      /* the pages of either moment */
        lowHi[i] = lowHi[i] > sMemLowHi[i] ? lowHi[i] : sMemLowHi[i];
    }
    nSpans = BuildSpans(spans, lowLo, lowHi);
    why = ApplyMemory(sMem.p, sMem.n, spans, nSpans);
    if (why) {
        Pal_Fatal("state: the pause's snapshot was damaged");
    }
    if (sMemWithGs && sHost && sHost->set && sMemGs.n) {
        if (sHost->set(sMemGs.p, sMemGs.n) != 0) {
            Pal_Fatal("state: the renderer's part could not be put back");
        }
    }
    KFiber_ResumeAt((void *)(unsigned long)sMemSp);
}

/* Takes the snapshot, in the running fiber, as a state is taken at a VBlank's start: the call returns 0 when it is
 * taken, -1 when it is not (Kern_StateMemWhy says why), and returns 1 a second time, in the restored context, after
 * Kern_StateMemRestore. */
int Kern_StateMemSave(void)
{
    if (KFiber_Current() == KFiber_Main()) {
        sMemFail = "the kernel is not running yet";
        return -1;
    }
    if (KFiber_RunOutside(MemSaveMain, 0) != 0) {
        return 1;
    }
    return sMemFail ? -1 : 0;
}

const char *Kern_StateMemWhy(void)
{
    return sMemFail;
}

/* Puts the snapshot back and goes on in the context that took it (its Kern_StateMemSave returns 1). With gs nonzero the
 * renderer's block is put back too (the pause ends). Returns only when there is no snapshot. */
void Kern_StateMemRestore(int gs)
{
    sMemWithGs = gs;
    KFiber_RunOutside(MemLoadMain, 0);
}

void Kern_StateMemDrop(void)
{
    sMemHave = 0;
    HostBuf_Free(&sMem);
    HostBuf_Free(&sMemGs);
}

int Kern_StateMemHave(void)
{
    return sMemHave;
}

__attribute__((weak)) void Pal_FreezeCancel(void)   /* pal_freecam.c's; the state's own tests run without it */
{
}

/* ------------------------------------------------------------- requests */

const char *Kern_StateRequestSave(const char *path)
{
    if (!path || StrLen(path) >= (int)sizeof(sReqPath)) {
        return "no such file name";
    }
    StrNCopy(sReqPath, path, (int)sizeof(sReqPath));
    sReq = 1;
    return 0;
}

const char *Kern_StateRequestLoad(const char *path)
{
    if (!path || StrLen(path) >= (int)sizeof(sReqPath)) {
        return "no such file name";
    }
    StrNCopy(sReqPath, path, (int)sizeof(sReqPath));
    sReq = 2;
    return 0;
}

int Kern_StateRequestPending(void)
{
    return sReq != 0;
}

const char *Kern_StateResult(void)
{
    return sResult;
}

/* Why an owner cannot be saved or loaded at this VBlank (a memory card file is open), or NULL. */
static const char *OwnersBusy(void)
{
    int i;
    for (i = 0; i < sNumOwners; i++) {
        const char *why = sOwners[i]->busy ? sOwners[i]->busy() : 0;
        if (why) {
            return why;
        }
    }
    return 0;
}

static void VBlankStartInner(u64 n)
{
    int kind = sReq;
    int blocked;
    const char *busy = OwnersBusy();
    if (!kind) {
        return;
    }
    blocked = Kern_StateBlocked();
    if (busy && !blocked && !sHostReq) {
        if (!sHeld) {
            sHeld = 1;
            KLog("state: a request waits at VBlank %llu for a VBlank with no card file open (%s)", n, busy);
        }
        return;                /* a script's request waits (sReq stays) for a VBlank with no card file open */
    }
    sHeld = 0;
    sReq = 0;
    if (blocked) {
        sResult = "not while a movie is open";
    } else if (kind == 1 && Kern_StateMemHave()) {
        sResult = "not while the pause is on";   /* the file would hold the loop's renderer, not the snapshot's */
    } else if (busy) {
        sResult = busy;
    } else if (KFiber_Current() == KFiber_Main()) {
        sResult = "the kernel is not running yet";
    } else if (kind == 1) {
        if (KFiber_RunOutside(SaveMain, sReqPath) != 0) {
            AfterLoad();       /* a load put this context back */
            return;
        }
        sResult = sFail;
        if (sFail) {
            KLog("state: not saved: %s", sFail);
        } else {
            KLog("state: saved VBlank %llu to %s (%u pages, %u bytes raw, %u bytes stored, %u ms)", n, sReqPath,
                 sStats.pages, sStats.rawBytes, sStats.storedBytes, sStats.ms);
        }
        return;
    } else {
        KFiber_RunOutside(LoadMain, sReqPath);
        sResult = sFail;
        KLog("state: not loaded: %s", sFail ? sFail : "?");
        return;
    }
    KLog("state: not %s: %s", kind == 1 ? "saved" : "loaded", sResult);
}

/* Called at the start of every VBlank, first (vblank.c), with its number. The host's request (a key of its window) is
 * taken here, in the same call, and answered when the save or load is over. */
void KState_VBlankStart(u64 n)
{
    if (!sReq) {
        int k = Ipc_StateRequest(sReqPath, (u32)sizeof(sReqPath));
        if (k) {
            sReq = k;
            sHostReq = 1;
        }
    }
    sDoneVblank = n;
    VBlankStartInner(n);
    if (sHostReq) {
        sHostReq = 0;
        Ipc_StateAnswer(sDoneVblank, Kern_StateResult());
    }
}

