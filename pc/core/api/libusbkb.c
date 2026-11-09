/* The USB keyboard library (libusbkb).
 *
 * Written from the library's behaviour at its interface, as the original
 * game shows it. Offline, as in
 * the original game, the IOP's keyboard service (0x80000211) answers every request
 * with zeros: sceUsbKbInit's GetInfo finds no keyboard, so Init deletes its
 * semaphore and gives -1, and the requests after it give -1 with no
 * semaphore. The library's kernel calls (the bind, the semaphore, the
 * request's RPC without a wait and sceUsbKbSync's wait for its end) are the
 * SDK's, in its order. The per-port records are the core's own memory.
 *
 * With a keyboard connected the service (io/io_usbkb.c) answers
 * one keyboard and the keyboard's boot reports, and the library does what it does with them. That part is
 * written from black-box observation of the original's library (the SDK code, run in the
 * original game, with a keyboard model) with reports made by a script:
 * the data a Read gives (UsbKbData: the LEDs, the modifiers, a key count and the key codes) as a function of
 * the sequence of reports, and of the record's state, over about 45,000 reads of random sequences (all
 * agree, the records' words included). The rules are in ReadModeZero.
 * Read mode 1 (raw reports) is never set by the game and is not observed.
 * (include/libusbkb.h declares two of these as void; the game's
 * usb_kbd.c as s32: the definitions give the s32.) */
#include "common.h"
#include "io/io.h"
#include "io/io_kernel.h"

#define KB_SID 0x80000211u
#define KB_MAX_PORTS 0x7F

s32 AddIntcHandler(s32 cause, s32 (*handler)(s32), s32 next);
s32 EnableIntc(s32 cause);
s32 sceUsbKbInit(s32 *st);
s32 sceUsbKbGetInfo(void *info);
s32 sceUsbKbRead(s32 port, void *data);
s32 sceUsbKbSetLEDStatus(s32 port, s32 status);
s32 sceUsbKbSetLEDMode(s32 port, s32 mode);
s32 sceUsbKbSetRepeat(s32 port, s32 start, s32 interval);
s32 sceUsbKbSetCodeType(s32 port, u32 type);
s32 sceUsbKbSetArrangement(s32 port, u32 arrangement);
s32 sceUsbKbSync(s32 mode, s32 *st);
s32 sceUsbKbSetReadMode(s32 port, u32 mode);
s32 sceUsbKbClearRbuf(s32 port);

typedef struct KbRec {
    s32 ledMode, codeType, arrangement, readMode;
    s32 held;            /* 0: no key held; 1: held; 2: the repeat has begun */
    s32 repeatCount;     /* VBlanks since the key went down, or since its last repeat (the interrupt handler counts) */
    s32 repeatStart, repeatInterval;
    u16 ring[4];         /* the repeat's key events, popped one a Read */
    s32 ringWrite, ringRead, ringCount;
    u16 heldKey;         /* the key that repeats: the last of the keys that went down */
    u8 lastRaw[6];       /* the last report's keys, any report */
    u8 lastKeys[6];      /* the last report taken (not a rollover error) */
    u8 lastMods;         /* its modifiers, to see a change */
    u8 keptMods, keptLeds;   /* what the data holds: the last report taken */
    u8 kmode;            /* EMPIRICAL (see ReadModeZero): set by a key release the repeat did not hold */
} KbRec;

static struct {
    int ready;
    s32 sema;            /* -1 before sceUsbKbInit makes it */
    s32 count;           /* the keyboards found */
    SifClientData client;
    u8 send[0x10];
    u8 recv[0x90];
    s32 kind, port;      /* the request in progress, for its answer's handler */
    s32 handler;         /* the repeat handler's id */
    void *out;
    KbRec rec[KB_MAX_PORTS];
} g_kb;

static void Defaults(void)
{
    if (!g_kb.ready) {
        g_kb.ready = 1;
        g_kb.sema = -1;
    }
}

/* The RPC's end function: the semaphore signalled. */
static void End(void *arg)
{
    (void)arg;
    iSignalSema(g_kb.sema);
}

static s32 Request(u32 fn, s32 rsize)
{
    WaitSema(g_kb.sema);
    return sceSifCallRpc(&g_kb.client, (s32)fn, 1, g_kb.send, sizeof(g_kb.send), g_kb.recv, rsize, End, 0);
}

s32 sceUsbKbGetInfo(void *info);
static s32 sceUsbKbGetInfoWork(void *info)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    s32 r = Request(1, 0x90);
    if (r < 0)
        return r;
    g_kb.kind = 1;
    g_kb.out = info;
    return 0;
}

/* sceUsbKbRead's data (the game's UsbKbData, include/kbd.h): the LEDs, the modifiers, the key count and the
 * key codes. The words a read leaves alone keep what an earlier one wrote. */
typedef struct UsbKbData {
    s32 leds, modifiers, keyCount;
    u16 keys[7];
} UsbKbData;

static void RingPush(KbRec *k, u16 key)
{
    if (k->ringCount >= 4)
        return;
    k->ring[k->ringWrite] = key;
    k->ringWrite = (k->ringWrite + 1) & 3;
    k->ringCount++;
}

/* The keys of a report: its six slots, zeros left out. */
static s32 ReportKeys(const u8 *r, u8 *out)
{
    s32 n = 0;
    for (s32 i = 0; i < 6; i++)
        if (r[i])
            out[n++] = r[i];
    return n;
}

static s32 InKeys(const u8 *keys, s32 n, u8 key)
{
    for (s32 i = 0; i < n; i++)
        if (keys[i] == key)
            return 1;
    return 0;
}

/* The repeat is on: both of its values are above 0. A key that goes down while it is off is not held. */
static s32 RepeatOn(const KbRec *k)
{
    return k->repeatStart > 0 && k->repeatInterval > 0;
}

/* Read mode 0 (the game's: it sets 0 and never another): one Read answers with a report or none. The rules, as
 * observed:
 *  - a report whose first key is 1 (a keyboard's rollover error) is not taken: it only stops a key held, and
 *    is the "last report" for the change test (lastRaw). The data's key count is 0.
 *  - a report taken: the keys in it that were not in the last report taken are the new keys, in the report's
 *    order, a repeated key repeated. The data gets: with new keys, their count and codes, and the last of
 *    them is the key held (it repeats); unless the modifiers changed while the last report had keys, then
 *    the data gets one key, 0, and the key is not held (the new keys are not reported, they are down all
 *    the same); without new keys but with a change (the modifiers, or any other change of the six slots:
 *    keys released) the data gets one key, 0, and the key is not held, unless the key held is still in the
 *    report, which then gives no key. The key count of an empty answer is 0 and the key words stay.
 *    The repeat's ring is emptied by any report taken.
 *  - no report, or one that was not taken: the repeat. A held key (1) whose counter has reached the repeat's
 *    first value pushes 0, the key and (if the interval is 2 or more) 0, and becomes 2; a repeating one (2)
 *    whose counter has reached the interval pushes the key and (interval 2 or more) 0; the counter is
 *    cleared; the ring holds four and drops what doesn't fit. The repeat is looked at first, so that it
 *    also happens in a Read that gets a report not taken. A Read that took no report gives the ring's next
 *    key (key count 1) or none (key count 0).
 *  The LEDs and modifiers in the data are those of the last report taken.
 *  EMPIRICAL (the original's library keeps a state this model does not know the cause of; found with the typing
 *  test, tests/usb-keyboard/): a key goes down with the repeat off (the game sets it in the same frame) is not held,
 *  and its release, with the repeat on again, sets "kmode". In kmode a report that changes the modifiers and brings
 *  new keys from an empty last report gives one key, 0, and the new keys are not held (outside kmode they are
 *  reported, as the first rule says). Nothing was seen to clear kmode again (ClearRbuf and SetRepeat do not).
 *  The original's heldKey record word shows a second state, which this model does not use: it keeps the key
 *  after a release in kmode, and loses it at the release of the last four function keys and of the key after. */
static void ReadModeZero(UsbKbData *d, KbRec *k, const u8 *ans)
{
    u16 event[7];
    s32 nev = 0, reported = 0, skip = 0;
    u8 newKeys[6], lastKeys[6], keys[6];
    s32 nkeys, nlast, nnew = 0, i;

    if (k->held == 1 && k->repeatStart > 0 && k->repeatCount >= k->repeatStart) {
        RingPush(k, 0);
        RingPush(k, k->heldKey);
        if (k->repeatInterval >= 2)
            RingPush(k, 0);
        k->held = 2;
        k->repeatCount = 0;
    } else if (k->held == 2 && k->repeatCount >= k->repeatInterval) {
        RingPush(k, k->heldKey);
        if (k->repeatInterval >= 2)
            RingPush(k, 0);
        k->repeatCount = 0;
    }
    if (ans[1]) {
        const u8 *r = ans + 4;
        u8 mods = ans[2];
        skip = 1;
        if (r[0] == 1) {
            __builtin_memcpy(k->lastRaw, r, 6);
            k->held = 0;
        } else {
            nkeys = ReportKeys(r, keys);
            nlast = ReportKeys(k->lastKeys, lastKeys);
            for (i = 0; i < nkeys; i++)
                if (!InKeys(lastKeys, nlast, keys[i]))
                    newKeys[nnew++] = keys[i];
            reported = 1;
            s32 release = nkeys == 0 && nlast > 0;
            s32 kEffective = k->kmode;
            if (release && !k->held && RepeatOn(k))
                k->kmode = 1;
            if (mods != k->lastMods && nlast) {
                event[nev++] = 0;
                k->held = 0;
                k->heldKey = 0;
                k->repeatCount = 0;
            } else if (mods != k->lastMods && nnew && kEffective) {
                event[nev++] = 0;
                k->held = 0;
                k->heldKey = 0;
                k->repeatCount = 0;
            } else if (nnew) {
                for (i = 0; i < nnew; i++)
                    event[nev++] = newKeys[i];
                k->heldKey = newKeys[nnew - 1];
                k->held = RepeatOn(k) ? 1 : 0;
                k->repeatCount = 0;
            } else if (mods != k->lastMods) {
                event[nev++] = 0;
            } else if (__builtin_memcmp(r, k->lastRaw, 6)) {
                if (!InKeys(keys, nkeys, (u8)k->heldKey) || !k->heldKey) {
                    event[nev++] = 0;
                    k->held = 0;
                    k->heldKey = 0;
                    k->repeatCount = 0;
                }
            }
            __builtin_memcpy(k->lastKeys, r, 6);
            __builtin_memcpy(k->lastRaw, r, 6);
            k->lastMods = mods;
            k->keptMods = mods;
            k->keptLeds = ans[0];
            k->ringWrite = k->ringRead = k->ringCount = 0;
            k->repeatCount = 0;
        }
    }
    (void)reported;
    if (!nev && !skip && k->ringCount > 0) {
        event[nev++] = k->ring[k->ringRead];
        k->ringRead = (k->ringRead + 1) & 3;
        k->ringCount--;
    }
    d->leds = k->keptLeds;
    d->modifiers = k->keptMods;
    d->keyCount = nev;
    for (i = 0; i < nev; i++)
        d->keys[i] = event[i];
}

/* Read's answer handler: mode 0 above; mode 1 hands the report as the keyboard sent it (not observed: the
 * game never sets it), the key count its length. */
static void ReadAnswer(UsbKbData *d, KbRec *k)
{
    const u8 *ans = g_kb.recv;
    if (k->readMode == 1) {
        d->leds = ans[0];
        d->modifiers = ans[2];
        d->keyCount = ans[1];
        return;
    }
    ReadModeZero(d, k, ans);
}

/* The repeat's interrupt handler (INTC cause 3, the VBlank's start; added by sceUsbKbInit): for each keyboard,
 * the counter goes up by one while a key is held and the repeat's two values are above 0. */
static s32 RepeatHandler(s32 cause)
{
    (void)cause;
    for (s32 p = 0; p < g_kb.count; p++) {
        KbRec *k = &g_kb.rec[p];
        if ((k->held == 1 || k->held == 2) && k->repeatStart > 0 && k->repeatInterval > 0)
            k->repeatCount++;
    }
    return 0;
}

/* The answers' handlers, by the request's kind. */
static s32 Answer(void)
{
    s32 r = 0;
    switch (g_kb.kind) {
    case 1: {   /* GetInfo: the count and the max, then the ports' bytes */
        s32 *info = g_kb.out;
        info[0] = g_kb.recv[0];
        info[1] = g_kb.recv[1];
        for (u32 i = 0; i < g_kb.recv[0]; i++)
            ((u8 *)info)[8 + i] = g_kb.recv[2 + i];
        break;
    }
    case 2:     /* Read: the answer's report, if it has one, and the repeat */
        ReadAnswer((UsbKbData *)g_kb.out, &g_kb.rec[g_kb.port]);
        break;
    case 4:
    case 5:     /* the LEDs: -1 unless the answer echoes what was sent */
        r = g_kb.recv[1] == g_kb.send[1] ? 0 : -1;
        break;
    default:
        break;
    }
    g_kb.kind = 0;
    g_kb.out = 0;
    return r;
}

s32 sceUsbKbSync(s32 mode, s32 *st);
static s32 sceUsbKbSyncWork(s32 mode, s32 *st)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    if (mode == 0) {
        WaitSema(g_kb.sema);
        *st = Answer();
        SignalSema(g_kb.sema);
        return 0;
    }
    if (mode == 1) {
        if (sceSifCheckStatRpc(&g_kb.client))
            return 1;
        *st = Answer();
        return 0;
    }
    return -11;
}

s32 sceUsbKbInit(s32 *st);
static s32 sceUsbKbInitWork(s32 *st)
{
    Defaults();
    sceSifInitRpc(0);
    for (;;) {
        if (sceSifBindRpc(&g_kb.client, KB_SID, 0) < 0)
            return -1;
        if (g_kb.client.serve)
            break;
    }
    SemaParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 1;
    p.initCount = 1;
    g_kb.sema = CreateSema(&p);
    s32 info[0x90 / 4], sync = 0;
    __builtin_memset(info, 0, sizeof(info));
    sceUsbKbGetInfo(info);
    sceUsbKbSync(0, &sync);
    if (info[0] < 1 || info[0] > KB_MAX_PORTS) {
        DeleteSema(g_kb.sema);
        g_kb.sema = -1;
        return -1;
    }
    /* A keyboard (the model of io/io_usbkb.c): the records are set up (the LED mode, the code type and the
     * arrangement 1, the rest 0), and the repeat's interrupt handler added for the VBlank's start. */
    g_kb.count = info[0];
    __builtin_memset(g_kb.rec, 0, sizeof(g_kb.rec));
    for (s32 i = 0; i < g_kb.count; i++)
        g_kb.rec[i].ledMode = g_kb.rec[i].codeType = g_kb.rec[i].arrangement = 1;
    *st = info[0];
    g_kb.handler = AddIntcHandler(3, RepeatHandler, -1);
    if (g_kb.handler < 0) {
        DeleteSema(g_kb.sema);
        g_kb.sema = -1;
        return -1;
    }
    EnableIntc(3);
    return 0;
}

s32 sceUsbKbRead(s32 port, void *data);
static s32 sceUsbKbReadWork(s32 port, void *data)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    if ((u32)port >= (u32)g_kb.count)
        return -11;
    g_kb.send[0] = (u8)port;
    s32 r = Request(2, 0x50);
    if (r < 0)
        return r;
    g_kb.kind = 2;
    g_kb.port = port;
    g_kb.out = data;
    return 0;
}

static s32 Led(s32 port, s32 value, u32 fn)
{
    g_kb.send[0] = (u8)port;
    g_kb.send[1] = (u8)value;
    s32 r = Request(fn, 0x10);
    if (r < 0)
        return r;
    g_kb.kind = (s32)fn;
    g_kb.port = port;
    g_kb.out = 0;
    return 0;
}

s32 sceUsbKbSetLEDStatus(s32 port, s32 status);
static s32 sceUsbKbSetLEDStatusWork(s32 port, s32 status)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    if (port >= g_kb.count)
        return -11;
    return Led(port, status, 4);
}

s32 sceUsbKbSetLEDMode(s32 port, s32 mode);
static s32 sceUsbKbSetLEDModeWork(s32 port, s32 mode)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    if (port >= g_kb.count)
        return -11;
    if (mode > 2)
        return -12;
    s32 r = Led(port, mode, 5);
    if (port >= 0)
        g_kb.rec[port].ledMode = mode;
    return r;
}

s32 sceUsbKbClearRbuf(s32 port);
static s32 sceUsbKbClearRbufWork(s32 port)
{
    Defaults();
    if (g_kb.sema < 0)
        return -1;
    if ((u32)port >= (u32)g_kb.count)
        return -11;
    g_kb.send[0] = (u8)port;
    WaitSema(g_kb.sema);
    s32 r = sceSifCallRpc(&g_kb.client, 6, 1, g_kb.send, sizeof(g_kb.send), 0, 0, End, 0);
    if (r < 0)
        return r;
    KbRec *k = &g_kb.rec[port];
    k->held = k->repeatCount = 0;
    k->ring[0] = 0;
    k->ringWrite = k->ringRead = k->ringCount = 0;
    return 0;
}

/* No semaphore or port check (the SDK's): without a keyboard the records
 * are not set up, and nothing is kept. */
s32 sceUsbKbSetRepeat(s32 port, s32 start, s32 interval);
static s32 sceUsbKbSetRepeatWork(s32 port, s32 start, s32 interval)
{
    if (start < 0)
        return -11;
    if (interval < 0)
        return -12;
    if ((u32)port < (u32)g_kb.count) {
        g_kb.rec[port].repeatCount = 0;
        g_kb.rec[port].repeatStart = start;
        g_kb.rec[port].repeatInterval = interval;
    }
    return 0;
}

s32 sceUsbKbSetCodeType(s32 port, u32 type);
static s32 sceUsbKbSetCodeTypeWork(s32 port, u32 type)
{
    if (port >= g_kb.count)
        return -11;
    if (type > 1)
        return -12;
    if (port >= 0)
        g_kb.rec[port].codeType = (s32)type;
    return 0;
}

s32 sceUsbKbSetArrangement(s32 port, u32 arrangement);
static s32 sceUsbKbSetArrangementWork(s32 port, u32 arrangement)
{
    if (port >= g_kb.count)
        return -11;
    if (arrangement > 2)
        return -12;
    if (port >= 0)
        g_kb.rec[port].arrangement = (s32)arrangement;
    return 0;
}

s32 sceUsbKbSetReadMode(s32 port, u32 mode);
static s32 sceUsbKbSetReadModeWork(s32 port, u32 mode)
{
    if (port >= g_kb.count)
        return -11;
    if (mode > 1)
        return -12;
    if (port >= 0 && g_kb.rec[port].readMode != (s32)mode) {
        KbRec *k = &g_kb.rec[port];
        k->readMode = (s32)mode;
        k->repeatCount = 0;
        k->ring[0] = 0;
        k->ringWrite = k->ringRead = 0;
    }
    return 0;
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

s32 sceUsbKbGetInfo(void *info)
{
    return sceUsbKbGetInfoWork(info);
}

s32 sceUsbKbSync(s32 mode, s32 *st)
{
    return sceUsbKbSyncWork(mode, st);
}

s32 sceUsbKbInit(s32 *st)
{
    return sceUsbKbInitWork(st);
}

s32 sceUsbKbRead(s32 port, void *data)
{
    return sceUsbKbReadWork(port, data);
}

s32 sceUsbKbSetLEDStatus(s32 port, s32 status)
{
    s32 r = sceUsbKbSetLEDStatusWork(port, status);
    return r;
}

s32 sceUsbKbSetLEDMode(s32 port, s32 mode)
{
    return sceUsbKbSetLEDModeWork(port, mode);
}

s32 sceUsbKbClearRbuf(s32 port)
{
    s32 r = sceUsbKbClearRbufWork(port);
    return r;
}

s32 sceUsbKbSetRepeat(s32 port, s32 start, s32 interval)
{
    return sceUsbKbSetRepeatWork(port, start, interval);
}

s32 sceUsbKbSetCodeType(s32 port, u32 type)
{
    return sceUsbKbSetCodeTypeWork(port, type);
}

s32 sceUsbKbSetArrangement(s32 port, u32 arrangement)
{
    return sceUsbKbSetArrangementWork(port, arrangement);
}

s32 sceUsbKbSetReadMode(s32 port, u32 mode)
{
    return sceUsbKbSetReadModeWork(port, mode);
}
