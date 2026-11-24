/*
 * A helper of the second pass (a helper for each moment; MH2_PASS_HELPERS=n, 0 to 5, in a development build): gfx_pass.c again, with its own VIF1, VU1, GIF and compiled microprograms (gfx_pass_h1_vif.c and the other
 * files of its instance), under the names of gfx_pass_prefix.h with this instance's number (pp1_ to pp5_: gfx_pass_h1_help.c to gfx_pass_h5_help.c include this file after defining
 * GFX_INST). Compiled alone, with no instance, this file is empty.
 *
 * A helper runs one moment's Resend from its package (MomPkg, gfx_pass_job.h): the copy after the moment's Models, the arrays the Models made, the alpha. What else a Resend reads is the
 * window's, read only: the units' snapshot 0, the output stage's registers at the window's start, the labels, the pages table, the MXCSR, the serial (all in the window's slot, which is
 * the helper's until the window's countdown reaches zero). The helper copies the snapshot into its own buffer (SendFlip writes the scratch snapshot beside it), loads it, sends the copy
 * through its own units and puts the moment's record to the second ring (one record, under the ring's lock). It reads no set: the sets are the deciding copy's, and Cur and Past are
 * fatal here. The helper that ends the last moment of a window sends "all sent" (kind 6) and gives the slot back.
 *
 * MH2_PASS_HMODELS=1: the package of a moment after the first names the moment only, and the helper makes that moment's Models itself before the Resend, from what the window's slot
 * holds for it (gfx_pass.c's WinShare and ShareModels: the kept copy, the finding, the first moment's decisions, the deciding copy's sets read only).
 *
 * Nothing of the game's is touched; the data is the process's (hostsec.h).
 */
#include "common.h"
#if defined(GFX_INST) && GFX_INST >= 1
#define MH2_PASS_HELPER 1
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "gfx_pass_prefix.h"
#include "gfx_pass.c"
#include "gfx_pass_cb.h"

static PassBuf *sHBuf;          /* this helper's own snapshots (0 loaded from the window's, 2 the scratch) */
static u32 sHEvCap;
static u64 sHMoments, sHResendNs;
static u32 sHSpinUs, sHCtl;     /* MH2_PASS_HSPIN=ms: a pseudo-random wait before each Resend, to shake the order the moments run in; MH2_PASS_HELPCTL=1: the control, every 50th package skips Load(0); 2: the control, the helper reads the window's live copy and not its package's copy */

int PPN(HelpInit)(void)
{
    const char *e;
    Init();   /* this copy's: the same switches, its own buffers and counters */
    if (sMode < 3)
        return 0;
    CbInit();
    vif_init();
    sHBuf = Host_Alloc(sizeof(PassBuf));
    if (!sHBuf)
        return 0;
    sBuf = sHBuf;
    sHEvCap = 4u << 20;
    sEv = Host_Alloc(sHEvCap);
    if (!sEv)
        return 0;
    sEvCap = sHEvCap;
    sNoSets = 1;
    e = PASS_ENV("MH2_PASS_HSPIN");
    sHSpinUs = 0;
    while (e && *e >= '0' && *e <= '9')
        sHSpinUs = sHSpinUs * 10 + (u32)(*e++ - '0');
    sHSpinUs *= 1000;   /* ms */
    sHCtl = PASS_ENV("MH2_PASS_HELPCTL") ? (u32)(PASS_ENV("MH2_PASS_HELPCTL")[0] - '0') : 0;
    return 1;
}

void PPN(HelpRun)(MomPkg *k)
{
    PassJob *j = k->job;
    u32 mo = k->mo;
    u64 t0, cz0 = sCensus ? __builtin_ia32_rdtsc() : 0, cz1 = 0, czm = 0;
    if (sHSpinUs) {
        u64 w0 = Ipc_TimeNs(), w = ((u64)(sHMoments + mo) * 2654435761ull >> 8) % sHSpinUs;
        while (Ipc_TimeNs() - w0 < w * 1000ull)
            ;
    }
    _mm_setcsr(j->csr);
    if (k->evLen > sHEvCap) {   /* grown as a window needs */
        u32 nc = k->evLen + (k->evLen >> 2) + 4096u;
        u8 *nb = Host_Alloc(nc);
        if (!nb)
            Host_Fatal("pass helper: no memory for a moment's copy");
        Host_Free(sEv);
        sEv = nb;
        sHEvCap = sEvCap = nc;
    }
    memcpy(sVu0, j->buf.vu0, sizeof(sHBuf->vu0));
    memcpy(sVif0, j->buf.vif0, sizeof(sHBuf->vif0));
    memcpy(sVus0, j->buf.vus0, sizeof(sHBuf->vus0));
    memcpy(sGif0, j->buf.gif0, sizeof(sHBuf->gif0));
    memcpy(sGs0, j->buf.gs0, sizeof(sHBuf->gs0));
    memcpy(&sWS, j->ws, sizeof(sWS));
    sDraws = j->draws;
    sDrawCap = j->drawCap;
    if (j->pages)
        PPN(vif_set_pages)(j->pages);
    sEvLen = k->evLen;
    sNDraws = k->nDraws;
    if (k->models) {   /* MH2_PASS_HMODELS: this moment's Models is made here */
        u64 m0 = sCensus ? __builtin_ia32_rdtsc() : 0;
        ShareModels(k);
        PassHelp_ModelsDone();
        if (sCensus) {
            czm = __builtin_ia32_rdtsc() - m0;
            sCzMod += czm;
            sCzModN++;
            CzMark(sEv, ((WinShare *)j->share)->keep);
        }
    } else
        PkgApply(k, mo);
    if (sHCtl == 2)   /* the control: the window's live copy (the deciding copy's, as its Models leave it) instead of the package's */
        memcpy(sEv, j->ev, sEvLen);
    sNMom = k->nm;
    sSerial = j->serial;
    sWinEnd = j->tEnd;
    sN.windows = k->win;
    PassSend(2, (u32)(sAlpha * 256.0 + 0.5) | (sSkipMom ? 0x8000u : 0u) | (mo << 16) | (sNMom > 1 ? (sNMom << 24) : 0u), 0, 0);
    sBirthW = sAlpha >= 1.0 ? 256u : (u32)(sAlpha * 256.0 + 0.5);
    t0 = Ipc_TimeNs();
    if (sCensus) {   /* MH2_PASS_CENSUS: the window's snapshot and the package copied in, up to here */
        cz1 = __builtin_ia32_rdtsc();
        sCzPre += cz1 - cz0 - czm;
    }
    if (!(sHCtl == 1 && ((sHMoments + 1) % 50u) == 0))   /* (the control skips the load: the units are as the last package left them) */
        Load(0);
    gif_sandbox(1, sSb);
    sRealCur = 0;
    sDiff = 0;
    if (sCensus) {   /* the units put back */
        cz0 = __builtin_ia32_rdtsc();
        sCzLoad += cz0 - cz1;
    }
    gGfxPassRun = 1;
    Resend();
    gGfxPassRun = 0;
    if (sCensus)
        cz1 = __builtin_ia32_rdtsc();
    PassSend(4, 0, 0, 0);
    gif_sandbox(0, 0);
    gif_sandbox_clear();
    sHMoments++;
    if (sCensus)   /* the moment's record put to the ring, the wait for the ring's lock in it */
        sCzPut += __builtin_ia32_rdtsc() - cz1;
    sHResendNs += Ipc_TimeNs() - t0;
    if (sWinEnd)
        MPut(MV_MOM0 + mo, Ipc_TimeNs() - sWinEnd);
    PassHelp_Done(k);
    if (__atomic_sub_fetch(&j->left, 1, __ATOMIC_ACQ_REL) == 0) {
        PassSend(6, 0, 0, 0);   /* all of the window's records are sent */
        PassHelp_Release(j);
    }
}

void PPN(HelpFinish)(void)
{
    char line[160];
    u32 n = 0;
    Str(line, &n, "mh2-core: pass helper ");
    Num(line, &n, GFX_INST);
    Str(line, &n, ": moments ");
    Num(line, &n, sHMoments);
    Str(line, &n, " resend-ms ");
    Num(line, &n, sHResendNs / 1000000ull);
    Str(line, &n, " ring-dropped ");
    Num(line, &n, sRingDropped);
    if (sCensus) {   /* (for the census's totals: the same sum in microseconds) */
        Str(line, &n, " resend-us ");
        Num(line, &n, sHResendNs / 1000ull);
    }
    line[n] = 0;
    Host_Log(line);
    Gfx_PassFinish();
}
#endif
