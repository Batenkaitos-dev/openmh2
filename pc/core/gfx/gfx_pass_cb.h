#ifndef GFX_PASS_CB_H
#define GFX_PASS_CB_H
/*
 * What a copy's units call outside themselves, and the copy's one record a moment. Included by gfx_pass_run.c and gfx_pass_help.c after their copy of gfx_pass.c, so
 * that each copy (the deciding one, and each helper) has its own, under its own prefix. The second ring has one producer at a time: every put goes through PassHelp_RingPut (the ring's lock).
 */
/* ---- what the copy's units call ---- */

static u32 sCopyOdd;   /* the copy's anomalies: counted here, the game's counter is not touched */

void Gfx_Oddity(const char *tag, u32 key)
{
    (void)tag;
    (void)key;
    sCopyOdd++;
}

void hw_gif_dma_resume(void)
{
}

void gs_gif(int path, const void *data, u32 qwords)
{
    Pass_Out(path, data, qwords);
}

u32 gs_state(void *buf, int load)
{
    (void)buf;
    (void)load;
    return 3 * sizeof(u32);
}

void *Pal_ArenaAlloc(unsigned int size)
{
    return Host_Alloc(size);
}

void Pal_ArenaFree(void *p)
{
    if (p)
        Host_Free(p);
}

void Pal_Fatal(const char *what)
{
    Host_Fatal(what);
}

void *DataC_TryAt(unsigned int ps2)
{
    (void)ps2;
    return 0;
}

IPC_CALL void Ipc_StepReport(void)
{
}

IPC_CALL void Ipc_GsKeepLabels(int on)
{
    (void)on;
}

static u64 sRingDropped;   /* the pass's records that had no ring to go to */

/* One record a moment (MH2_PASS_REC1, default on; 0: a record a piece, as before). The records between a moment's begin (kind 2) and its end (kind 4) are kept in a buffer and put
 * to the ring as one record of kind 7, whose argument is the begin record's and whose data is a qword with the number of qwords that follow, then each piece as a qword header
 * (path, qwords) and its data. A record over MH2IPC_REC_MAX goes in pieces (Ipc_GsPassRing makes them kind 8 after the first). The host takes the moment apart again. Nothing
 * of the stream changes: the hash of a moment (PassSend) is of the pieces. */
static int sRec1 = 1;
static u8 *sMB;
static u32 sMBn, sMBcap, sMBarg;
static int sMBopen;
static u64 sMBrecs;

static void MBAppend(const void *data, u32 bytes)
{
    if (sMBn + bytes > sMBcap) {
        u32 nc = sMBcap ? sMBcap * 2 : (4u << 20);
        u8 *nb;
        while (nc < sMBn + bytes)
            nc *= 2;
        nb = Host_Alloc(nc);
        if (!nb)
            Host_Fatal("pass: no memory for a moment's record");
        if (sMB) {
            memcpy(nb, sMB, sMBn);
            Host_Free(sMB);
        }
        sMB = nb;
        sMBcap = nc;
    }
    memcpy(sMB + sMBn, data, bytes);
    sMBn += bytes;
}

IPC_CALL void Ipc_GsPass(unsigned int kind, unsigned int arg, const void *data, unsigned int qwords)
{
    if (sRec1) {
        if (kind == 2) {
            u32 z[4] = {0, 0, 0, 0};
            sMBn = 0;
            sMBarg = arg;
            sMBopen = 1;
            MBAppend(z, 16);
            return;
        }
        if (kind == 3 && sMBopen) {
            u32 h[4];
            h[0] = arg;
            h[1] = qwords;
            h[2] = h[3] = 0;
            MBAppend(h, 16);
            MBAppend(data, qwords * 16);
            return;
        }
        if (kind == 4 && sMBopen) {
            u64 t0 = sTime ? Ipc_TimeNs() : 0;
            u32 total = (sMBn - 16) / 16;
            sMBopen = 0;
            memcpy(sMB, &total, 4);
            sMBrecs++;
            if (!PassHelp_RingPut(7u | (sSerial << 4), sMBarg, sMB, sMBn / 16))
                sRingDropped++;
            if (sTime)
                sTAcc[T_PUT] += Ipc_TimeNs() - t0;
            return;
        }
    }
    if (!PassHelp_RingPut((kind & 15u) | (sSerial << 4), arg, data, qwords))   /* the pass thread's ring, the window's serial in the record */
        sRingDropped++;
}

/* the copy's own switches (the same environment for every copy) */
static void CbInit(void)
{
    sRec1 = !(PASS_ENV("MH2_PASS_REC1") && PASS_ENV("MH2_PASS_REC1")[0] == '0');   /* 0: a record a piece (the control of the one record a moment) */
}

#endif
