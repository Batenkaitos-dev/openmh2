/*
 * The second pass: the platform layer runs a chain again on a copy, to make a picture that the game did not draw.
 *
 * A window is one transfer to VIF1 (the DMA emulation's DmaRun: a chain, or a block). While it is open, gfx_dma.c hands this file every piece of data
 * it sends to VIF1 and to the GIF (the DMA channel 2 that a MSKPATH3 lets through in the middle of it), the tags it moves with its TTE bit, and the
 * label of each model draw whose chain it walks (Ipc_GsModelTag): the copy. gfx_gs.c hands it the stream the window made (the real one). When the
 * window closes, the pass runs: the units it runs on (VU1's memory and registers, VIF1, PATH3's mask, the path buffers, the anomaly counters) are
 * saved, put back to what they were when the window opened, the copy is sent through them again with the GS stream going here and nowhere
 * else (the game's hash, the host's ring and the GS's events never see it), and what the window left is put back. The game never sees the pass:
 * it reads no guest memory (the copy holds the bytes), writes none, and every unit's state is the real one's again when it ends.
 *
 * MH2_PASS=1 is the identity pass: the copy is sent as it is, and its stream, the units it leaves and the PATH buffers must equal the real
 * window's, byte for byte (counted: the windows compared, the ones that differ). MH2_PASS_CTL=1 is its control: one entry of one draw's
 * model-to-screen matrix is changed in the copy, and the streams must then differ.
 *
 * This file's data is the process's, not the game's (the build gives it hostsec.h): a save state does not hold it.
 */
#include "gfx_rt.h"
#include "host.h"
#include "ipc/core_ipc.h"
#include "gfx_pass_job.h"


#define EV_CAP (24u << 20)       /* the copy of a window: bytes */
#define REAL_CAP (48u << 20)     /* the real stream of a window */
#define MAX_DRAWS 4096
_Static_assert(MAX_DRAWS == PJ_MAXDRAWS, "gfx_pass_job.h: the package holds as many draws as the pass");

typedef struct PEv {
    u32 kind, n, draw, pad;      /* kind 1 VIF1 data of n qwords, 2 a tag's upper half (16 bytes follow), 3 GIF data (PATH3); draw: the label, or -1 */
} PEv;

int gGfxPassOpen, gGfxPassRun;
static int sMode = -1;           /* 0 off, 1 the identity pass, 2 and the stream to the renderer, 3 and the matrices of the in-between moment, 4 the skip test's form (3 with the step before the step before) */
static double sAlpha = 0.5;      /* mode 3: where the in-between moment lies between the step before and this one */
static int sCtl;                 /* the control: change a matrix entry in the copy */
static u8 *sEv;                  /* Host_Alloc'ed when the pass is on: 24 MB */
static u32 sEvLen;
static u8 *sReal;                /* 48 MB */
static u32 sRealLen, sRealCur;
static IpcLabel *sDraws;         /* Host_Alloc'ed with the pass (an IpcLabel holds the matrix table: 2.4 KB) */
static u32 sNDraws;
static int sCur;
static int sLog;                 /* MH2_PASS_LOG=1: a line for each draw of a window; 4: a line for each draw with its class (development) */
static u64 sStepNo;
static u32 sOutDraw = 0xFFFFFFFFu;   /* the draw whose piece of the copy the pass is sending */
static u64 sStepQ[4][2];
static u32 sDropCls;             /* MH2_PASS_DROPCLS=mask (development): the pass sends no piece of a labelled draw whose class (sCls) is in the mask: 1 matrices blended, 2 still with vertices the game moved, 4 still, 8 no matrix found, 16 birth, 32 cut, 64 skinned and its bones not found, 128 no model */
static u8 sCls[MAX_DRAWS];
static u32 sDropUnl, sDropE0, sDropE1, sEvIdx;   /* MH2_PASS_DROPUNL=mask (development): the pass sends no piece of the draws that have no label, of the paths in the mask (1, 2, 4: path 1, 2, 3);
                                                    MH2_PASS_DROPE=a,b: no piece of the unlabelled events of the window numbered a up to b (counted from 1 in the window's list) */
static double sHoldPx = 64.0;    /* MH2_PASS_HOLD=px (development; 0 turns it off): a model or a bone whose place moved more than this many pixels between the steps is not blended: the step's own, as the renderer's gate does a kick */
static int sHold2 = 1;           /* MH2_PASS_HOLD2=0 (development; plan 2's step 2): below one half (alpha under 128/256) a model, a bone, a header row or a 2D packet that is cut or jumps is held as the step before
                                  * had it (from one half on it is the step's own, as before); a camera cut gives no pass picture below one half */
static int sSkipMom;             /* Models: the moment is below one half and the camera cut: its in-between picture is not made from the pass */
static int sHeldNow;             /* the draw in hand has its matrices held (HdrBlend holds its jumping rows with them) */
static int sP2 = 1;              /* MH2_PASS_P2=0 (development): the 2D packets that slide (a message's banner) are not blended (Direct2D) */
static int sFade = 1;            /* MH2_PASS_FADE=0 (development): the colours of the 2D packets (a fade's sprite) are not blended */
static int sFlipMode = 1;       /* MH2_PASS_FLIP=0 (development): a flipbook's offset is not crossfaded; 2 the control: the draw is sent twice, the first copy dropped at the output */
static int sBD = 2;              /* a 2D packet that is born in the span: 2 (the default) it is not drawn in an in-between picture, it appears at its own step as in the original; MH2_PASS_BD=1 (development control) it fades in; MH2_PASS_BD=0 it is drawn whole */
/* The pass's own cost (development): the time of each part of a step's windows, from Ipc_TimeNs, read for the log at the end (Gfx_PassFinish). MH2_PASS_REPEAT=N runs the second half of
 * a window's work (the copy put back, Resend) N - 1 more times with its stream thrown away; MH2_PASS_SPIN=ms is the control: a busy wait of that many ms once a step, before the pass runs. */
/* T_HDR and T_2D (the time of HdrBlend and of Direct2D) are inside the first moment's Models (T_MODELS), T_LHDR and T_L2D inside a later moment's (T_LMODELS); T_LMODELS, T_LLOAD and T_LRESEND
 * are the parts of the moments after the first, whose sum is the moments' share of T_REPEAT; T_WALL is a window's whole pass timed from outside (WindowRun), so that the parts' sum has something to be checked against. */
enum { T_COPY, T_FIND, T_MODELS, T_SAVELOAD, T_RESEND, T_PUT, T_REPEAT, T_SPIN, T_HDR, T_2D, T_LMODELS, T_LLOAD, T_LRESEND, T_LHDR, T_L2D, T_WALL, T_TOTAL, T_N };
#define TM_MAX 65536u
static int sTime;                /* the pass is on: the parts are timed */
/* MH2_PASS_CENSUS=1 (development; the modes 3 and 4): the time of the moments' Resends by draw kind, counted by the copy that runs them (the pass thread's, or each helper's). Every piece of
 * the copy that Resend sends is timed with the processor's tick counter and entered in the row of its kind (CZ_*) and, when it is a labelled draw's, in the row of the draw's call site and
 * microprogram; the part of that time spent in the pass's own handling of the stream the piece made (Pass_Out) is entered beside it. A helper also times the window's snapshot copied, the
 * units put back and the moment's record put; the deciding copy times each moment's Models and package. The rows are logged at the end, in ticks, with the span of the clock and of the
 * tick counter over the run (a census tool reads them). Off, nothing is timed. 2 is the control of the totals: the row of the blended draws is counted and not timed.
 * With helpers every row is kept twice: for the draws the moment's Models left as the window had them, and for the draws any of whose bytes it wrote (the package's maker compares each
 * piece with the window's kept copy and marks the draw's pieces in the event's spare word, which nothing else reads). */
enum { CZ_TAG, CZ_PATH3, CZ_UNL, CZ_MOVED, CZ_VANIM, CZ_STILL, CZ_NOMTX, CZ_NOPART, CZ_CUT, CZ_NOBONE, CZ_NOHANDLE, CZ_OTHER, CZ_FLIP, CZ_N };
#define CZ_SITES 128u
typedef struct CzAcc {
    u64 ticks, out_ticks, pieces, qw, runs, out_qw;
} CzAcc;
static int sCensus;
static CzAcc sCz[CZ_N][2], sCzSite[CZ_SITES + 1], *sCzCur, *sCzCurSite;   /* (the last site row: the draws that found no row) */
static u32 sCzRow;
static u8 sCzWr[MAX_DRAWS];   /* (the package's maker: the draws this moment's Models wrote) */
static u32 sCzSiteKey[CZ_SITES][2], sCzSiteN;
static u8 sCzSiteUsed[CZ_SITES];
static u64 sCzTsc0, sCzNs0, sCzMoments, sCzPre, sCzLoad, sCzPut;
static u64 sCzDec[6][4], sCzDecWin, sCzDecHeadNs;   /* the deciding copy with helpers, by moment: Models, the wait for a free package, the package filled and queued, the count */
static int sRepeat = 1, sSpinMs, sSpinDone, sQuiet;
/* Several moments a step (development, for the proofs; the runs are --fast): MH2_PASS_ALPHAS=a,b,c (in 256ths, up to 6) makes each window's pass run once for each alpha in a row, from the copy as the
 * window left it (a kept image of it is put back before each moment after the first), the records of the step made by the first moment only. MH2_PASS_CTL=2 is the control: the copy is not put back. */
#define MOM_MAX 6u
static u32 sMomA[MOM_MAX], sNMom = 1;
static int sMomCtl, sRecord = 1;
static int sDefer, sNoSets;   /* MH2_PASS_DEFER (development): 1 runs every moment's Models first and each Resend from its package (MomPkg), with the sets made fatal during the Resends; 2 and 3 are its controls */
static int sHoldSafe = 1;   /* MH2_PASS_HOLDRAW=1: the hold puts any word of the step before in (the control of HoldSafe; development) */
static u8 *sKeep;                /* Host_Alloc'ed when there are several moments: the copy as the window left it (MH2_PASS_HMODELS with helpers: the slot's own, in its WinShare) */
static int sHMod __attribute__((unused)), sHModCtl __attribute__((unused));   /* MH2_PASS_HMODELS (development; on unless =0): with helpers, a moment after the first has its Models made by the helper that runs its Resend, from what the window's
                                  * slot holds for it (WinShare), and the step's end waits until every such Models is made. MH2_PASS_HMODCTL: its controls (1: every 50th such Models pairs with
                                  * the set of another step; 2: the step's end does not wait) */
static int sWalkTag, sWalkCtl;   /* MH2_PASS_WALKTAG (development; on unless =0): the stream is followed by tag: a packet whose register list writes no register that decides a colour's class, and none of
                                  * whose colours is scaled, is not visited datum by datum; its counters follow from its tag (WalkTag). 2: each packet so taken is followed the old way too,
                                  * and the registers, the counters and the bytes are compared. MH2_PASS_WALKCTL, its controls: 1 a packet with A+D in its list is taken too (its register
                                  * writes are lost); 2 a packet with a colour to scale is taken too (the colour is not scaled) */
static u64 sWt[6];               /* MH2_PASS_WALKTAG: the packets with a register list followed and those taken by tag, their quadwords, the packets compared and those that differed */
static int sDIx, sDHo, sDCtl;    /* MH2_PASS_DRAWIDX (development; on unless =0): the walks for one draw's data (FindMatrix, FindBones, HdrBlend) visit the span of the copy that holds the draw's pieces,
                                  * from an index made in one walk a window (IndexDraws; a helper's from the window's share). 2: each walk is made over the whole copy too and compared; the
                                  * whole copy's answer is used. MH2_PASS_HASHONCE=1: a moment after the first takes the draws' hashes of the window's first moment (a helper: from the
                                  * window's share) instead of hashing the copy again. 2: it hashes the copy too and compares; the new hashes are used. MH2_PASS_DRAWCTL, their controls: 1
                                  * every fourth draw's span begins past its first piece; 2 a moment after the first takes the hashes of the window before */
static u32 sDLo[MAX_DRAWS], sDHi[MAX_DRAWS];   /* MH2_PASS_DRAWIDX: the offset in the copy of the header of the draw's first piece, and the offset past its last piece (0 and 0: no piece) */
static u64 sDH1[MAX_DRAWS], sDHOld[MAX_DRAWS]; /* MH2_PASS_HASHONCE: the hashes the window's first moment made; the window before's (the control) */
static u64 sDx[6];               /* the walks made by the index, those compared and those that differed; the hashes taken, compared and differing */
static int sD2x;                 /* MH2_PASS_D2IDX=1 (development): the 2D walk (Direct2D) of a moment after the first does not follow the VIF codes of the whole copy again: it visits the
                                  * places of the unlabelled GIF data that the window's first moment's walk noted (a helper's from the window's share). 2: the walk is made over the whole
                                  * copy too, the places it comes to are compared with the noted ones, and it is the one used. MH2_PASS_DRAWCTL=3, its control: every eighth place is
                                  * left out of the list */
static u64 sD2s[7];              /* the walks made from the list, the places they visited; the walks compared, their places, the walks that differed; the windows whose list was full; the
                                  * most places in a window */
static int sShared;            /* a helper is in such a Models: Cur and Past answer with the window's share, and nothing of a set is written */
static u64 sCzMod, sCzModN;      /* MH2_PASS_CENSUS: a helper's own Models of the later moments (with the kept copy taken), in ticks, and their count */
static u64 sCzSt[3][2];          /* MH2_PASS_CENSUS, the stream's copies inside a Resend, ticks and quadwords: 0 followed for the registers (Walk), 1 copied aside first (a weight, PATH2 or PATH3), 2 sent
                                  * on (appended to the moment's record; with MH2_PASS_HASH the hash of its words too) */
static u64 sTAcc[T_N], sTCalls;
static int sLaterMo;             /* a moment after the first is in its Models (the timers of HdrBlend and Direct2D go to their own buckets) */
static u32 sTWin, sTN, sTWinAll;
/* A moment's time from its window's end, by moment number, and the windows whose copy Models left unchanged (development; read at the end, with the rows above):
 * MV_MOM0.. rows hold, for each window that ran, the microseconds from the window's end (the game's thread: the job's tEnd on the pass thread) to the moment's last record put; MH2_PASS_STILL=1
 * keeps a copy of the stream before Models and counts the windows it leaves as they were (still), with the Resend time of the first moment and of the later ones, still and changed apart.
 * MH2_PASS_ALPHAS=sweep makes the moments the alphas a 165 Hz display asks for, from the step's number (SweepSet). */
enum { MV_MOM0 = 0, MV_RS_STILL = 6, MV_RS_CHG, MV_LRS_STILL, MV_LRS_CHG, MV_N };
static u32 (*sMV)[TM_MAX];
static u32 sMVn[MV_N];
static u64 sWinEnd;
static int sStill, sSweep;
static u32 sStillWins, sChgWins;
static u64 sSweepStep;   /* the steps that have ended since the first window ran (the sweep's key: the same in a run with the pass on a thread, which may drop a window) */
static int sSweepOn;
/* The live alphas (MH2_PASS_LIVE=1, development; plan 11's step 5a): the moments of a step are the host's plan's (Ipc_GsLivePlan), read at the step's first window on the game's thread (LiveDecide), the same for every window
 * of the step; the plan is used only when its VBlank number is the one whose VSync record follows the chain (sLFor, from the VBlank the kernel is in); else the plan's fallback (60: one moment at 128; above 60: none, the
 * window gets no pass and its step is B's). The pass thread's job carries them (nMom, momA). */
static int sLive;
static u32 sLNMom, sLA[MOM_MAX];                 /* the game's thread: this step's moments */
static int sLSkip;                               /* this window has no moment (the plan is not in time and its fallback is none) */
#ifndef MH2_PASS_COPY
static u32 sLVb, sLFor;                          /* the plan's VBlank they came from (0: the fallback), the VSync number the decision is for */
static u64 sLvCur;
static int sLvIn;                                /* the kernel is between a VBlank's start and its end */
static u64 sLv[8];                               /* counts: steps decided at a plan in time, at an old plan, at another plan, with none; (unused); steps whose stamp named the plan */
#endif
static u32 (*sTV)[TM_MAX];       /* microseconds a step: Host_Alloc'ed with the pass, T_N rows and one for TReport's sort */
static int sNoHdr;               /* MH2_PASS_NOHDR=1 (development): the parameters of a draw's header that move from step to step (a texture scroll) are not blended */
static int sDataMats = 1;       /* MH2_PASS_DATAMATS=0 (development): do not move the matrices of a skinned draw's data that are not copies of the label's slots */
static u32 sDropH[16], sNDropH, sOnlyH[16], sNOnlyH;   /* MH2_PASS_DROPH / MH2_PASS_ONLYH=h,h,...: the pass sends no piece of a labelled draw of these handles / of any other handle (development) */
static int sLerp;                /* MH2_PASS_LERP=1 (development): a matrix is blended entry by entry, as a vertex is (the renderer's own rule for the vertices), not by the quaternions */
static double sGate = 1.0;       /* MH2_PASS_GATE=2 (development): the gates of a cut for a span of two steps: twice the distance, any rotation */
static int sDrop;                /* MH2_PASS_DROP=1: the pass sends no PATH1 stream of a labelled draw; 2: none of PATH2's unlabelled data (to see which picture content is which) */
static u32 sHdrH;                /* MH2_PASS_HDR=h (development, with MH2_PASS_LOG=4): the first 40 qwords of each draw of handle h, as words, a line a qword */
static int sOverflow, sDirty, sDiff;
static u32 sDiffAt;
/* The snapshots of the units (PassBuf, gfx_pass_job.h) are Host_Alloc'ed with the pass, and the sandbox's path buffers too (2 MB): the core's image holds none of them. With the pass on a thread
 * of its own (MH2_PASS_THREAD=1) a window's snapshots are its slot's: the game's thread fills them, the pass thread (this file compiled again, under the names of gfx_pass_prefix.h) reads them. */
#ifdef MH2_PASS_COPY
#define PASS_COPY 1
#else
#define PASS_COPY 0
#endif
#ifdef MH2_PASS_HELPER   /* a helper's copy (gfx_pass_help.c): it runs a moment's Resend from a package; it has no sets and never runs the window's own pass, and it makes a Models only
                          * under MH2_PASS_HMODELS, a later moment's, reading the deciding copy's sets through the window's share */
#define PASS_HELPER 1
#else
#define PASS_HELPER 0
#endif
static PassBuf *sBuf;
#define sVu0 (sBuf->vu0)
#define sVu1 (sBuf->vu1)
#define sVu2 (sBuf->vu2)
#define sVif0 (sBuf->vif0)
#define sVif1 (sBuf->vif1)
#define sVif2 (sBuf->vif2)
#define sVus0 (sBuf->vus0)
#define sVus1 (sBuf->vus1)
#define sVus2 (sBuf->vus2)
#define sVut (sBuf->vut)
#define sVift (sBuf->vift)
#define sGif0 (sBuf->gif0)
#define sGif1 (sBuf->gif1)
#define sGif2 (sBuf->gif2)
#define sGs0 (sBuf->gs0)
#define sGs1 (sBuf->gs1)
#define sGs2 (sBuf->gs2)
static u8 *sSb;                  /* 4 * GFX_PASS_SB_QW * 16 bytes, 16-aligned (Host_Alloc gives pages) */
static u32 sEvCap, sRealCap;     /* the capacities of sEv and sReal: EV_CAP and REAL_CAP, or what the slot holds (grown as a window needs, up to them) */
static u32 sDrawCap;             /* the labels sDraws holds */
static int sThread;              /* MH2_PASS_THREAD=1: this file's pass runs on a thread of its own (the copy of this file in gfx_pass_run.c); this one makes the copies and queues the jobs */
static PassJob *sJob;            /* the slot of the open window */
static u32 sWinSerial;           /* the game's thread: the last window's serial number (MH2_PASS_THREAD=1) */
static u32 sSerial __attribute__((unused));   /* the pass thread: the serial of the window it runs */

/* the output stage's registers (see "the output stage" below) */
#define GS_PRIM 0
#define GS_PMC 1
#define GS_PRMODE 2
#define GS_ALPHA 3
#define GS_TEST 5
#define GS_ZBUF 7
#define GS_TEX0 9
#define GS_PABE 11
#define GS_NREG 12

typedef struct GsSt {
    u64 r[GS_NREG];
    u32 known;                   /* a bit for each r[] written since the start */
} GsSt;

static GsSt sRS, sWS, sOS;       /* the real stream's registers now; at the window's start; the pass's now */

static struct {
    u64 cam_done, cam_other, cam_cut, cam_nomatrix;
    u64 kicks_lab, kicks_unl;
    u64 out_q[4][2];   /* the pass's stream in qwords: by path (1 to 3), and from a labelled draw's data (1) or not (0) */
    u64 hold_px, p2_packets, p2_blended, p2_faded, hdr_draws, hdr_qwords, vanim, vanim_draws_seen, unl_found, unl_rewritten;
    u64 data_mats, mod_rigid, mod_still, mod_cut, mod_birth, skin_blended, skin_still, skin_cut, skin_birth, skin_nobones, bones_blended, bone_nopartner, deaths, set_full, bones_over;
    u64 held_mod, held_skin, held_hdr, held_p2, skip_mom;
    u64 bd_born, bd_changed, mark_seen, mark_leak, bd_dead, bd_dead_changed;
    u64 flip_draws, flip_held[12], flip_p3, flip_cand;
    u64 windows, run, differ, skipped_dirty, skipped_over, nested, draws, found, ctl_changed, ctl_differ, end_differ, bytes, max_ev, max_real;
} sN;

/* The stream hash (development; MH2_PASS_HASH=1 or 2): what the pass sends for each moment of each window, hashed (FNV-1a over the kind, the argument and the data of each record from the
 * moment's begin record to its end record), in one table kept for the log at the end: with 2 each (window, moment, hash) is logged, with 1 the chain of them, one line. The records that
 * only mark the window (kinds 0, 1 and 6) are left out: they differ with the way the pass is run (a thread's serial), not with what it sends. It is the proof of step 4's work: the same
 * hash for every window and moment, whatever runs the moments. */
typedef struct HashRec {
    u32 window, moment;
    u64 h;
} HashRec;
#define HASH_MAX 262144u
static int sHashOn;
static u64 sHAcc;
static u32 sHMom, sHN;
static HashRec *sHTab;
static u64 sHChain;

/* The matrix blends' list (MatPrep, MatBlendT, below): its switches and counts. */
static u32 sTapeN, sTapeI, sTapeOver;
static int sTapeOn, sTapeMode;           /* sTapeMode: 0 off, 1 the first moment records, 2 a later moment takes */
static u64 sTapeHit, sTapeMiss, sTapeRec, sTapeBlend, sTapeCtl, sTapeCtlDone;
static void TapeInit(void);

static u64 HashStep(u64 h, u32 w)
{
    h ^= w;
    return h * 0x100000001B3ull;
}

static void PassSend(unsigned int kind, unsigned int arg, const void *data, unsigned int qwords)
{
    u32 k = kind & 15u;
    if (sHashOn && k >= 2 && k != 6) {
        u32 i, nw = qwords * 4;
        const u32 *w = (const u32 *)data;
        if (k == 2) {
            sHAcc = 0xCBF29CE484222325ull;
            sHMom = (arg >> 16) & 255u;
        }
        sHAcc = HashStep(HashStep(sHAcc, k), arg);
        for (i = 0; i < nw; i++)
            sHAcc = HashStep(sHAcc, w[i]);
        if ((k == 4 || k == 5) && sHN < HASH_MAX) {
            sHTab[sHN].window = sN.windows;
            sHTab[sHN].moment = k == 5 ? 255u : sHMom;
            sHTab[sHN].h = sHAcc;
            sHN++;
            sHChain = HashStep(HashStep(HashStep(sHChain, sN.windows), sHTab[sHN - 1].moment), (u32)sHAcc);
            sHChain = HashStep(sHChain, (u32)(sHAcc >> 32));
            if (k == 4)
                sHMom = 0;
        }
    }
    Ipc_GsPass(kind, arg, data, qwords);
}

/* ---- the sets: what each step drew, kept for the pairing of the steps after it ---- */

#define SET_DRAWS 8192u
#define SET_BONES 65536u
#define SET_HASH 16384u
#define OCC_SLOTS 4096u
#define P2_MAX 4096u
#define P2V_MAX (1u << 17)
#define HQW 24u                  /* the header of a rigid draw: the qwords before the vertices (what PDraw.dh leaves out) */
#define VQW 176u                 /* the qwords of a draw's data the bones are looked for in */

typedef struct PDraw {
    u32 handle, occ, nb, bfirst;
    u32 used;                    /* a later step paired with this draw (what is left is a death) */
    u64 dh;                      /* a hash of the draw's data after its matrices (a rigid draw's from qword 24, a skinned one's from 160): the vertices */
    float world[16];
    u32 hqn;                     /* the first HQW qwords of a rigid draw's data (its header: the matrices and the parameters), kept for the next steps' pairing */
    float hq[HQW * 4];
} PDraw;

typedef struct PBone {
    u32 slot;
    float m[16];
} PBone;

typedef struct P2 {
    u64 key;                     /* a hash of a 2D packet's words except the places of its vertices */
    u32 occ, nv, off, nc;        /* the packet's number among those with the same key, its vertices, and where their places start in PSet.p2v (its nc colours follow them) */
    u32 form;                    /* a hash of its tag (without the loop count) and its register list: what kind of packet it is */
    u32 prim;                    /* the PRIM its tag carries (PRE set), or 0xFFFF */
    u32 tex;                     /* its register list names ST or UV (1), or A+D (2), or neither (0) */
} P2;

typedef struct PSet {
    P2 *p2;
    u32 *p2v;                    /* x | y << 16 of the vertices of the 2D packets, in GS units (12.4), then their colours (r | g << 8 | b << 16 | a << 24) */
    u32 np2, nv2;
    PDraw *d;
    PBone *b;
    u16 *hash;
    u32 nd, nb;
    int have;                    /* a main view */
    float view[16], proj[16], vp[16];   /* ... and the projection and the viewport matrix it was drawn with */
} PSet;

static PSet sSet[3];
static int sCurSet;
static struct {
    u32 handle, count;
} sOcc[OCC_SLOTS];
static int sBack = 1;            /* the step the pairing looks at: 1 the step before (mode 3), 2 the one before that (mode 4, the skip test) */
static int sDIdx[MAX_DRAWS];     /* the draw's entry in the current set, or -1 */
static float *sF[MAX_DRAWS];     /* the draw's model-to-screen matrix in the copy */
static int sForm[MAX_DRAWS];
static u64 sDH[MAX_DRAWS];       /* the draw's data hash (PDraw.dh) */

static PSet sShCur, *sShPast, *sShPast1;   /* a helper in a later moment's Models (sShared): the current set's header as the window's first moment left it, with the helper's own room for the 2D
                                            * packets, and the deciding copy's Past(sBack) and Past(1) of that time (read only: the step's end waits for the helper) */

static PSet *Cur(void)
{
    if (sShared)
        return &sShCur;
    if (sNoSets) {
        Host_Log(PASS_DEVTEXT("mh2-core: MH2_PASS_DEFER FATAL: Cur() inside a Resend", "mh2-core: pass FATAL: Cur() inside a Resend"));
        __builtin_trap();
    }
    return &sSet[sCurSet];
}

static PSet *Past(int back)
{
    if (sShared) {
        if (back == sBack)
            return sShPast;
        if (back == 1)
            return sShPast1;
        Host_Log(PASS_DEVTEXT("mh2-core: MH2_PASS_HMODELS FATAL: a set the window's share does not have", "mh2-core: pass FATAL: a set the window's share does not have"));
        __builtin_trap();
    }
    if (sNoSets) {
        Host_Log(PASS_DEVTEXT("mh2-core: MH2_PASS_DEFER FATAL: Past() inside a Resend", "mh2-core: pass FATAL: Past() inside a Resend"));
        __builtin_trap();
    }
    return &sSet[(sCurSet + 3 - back) % 3];
}

static u32 HashOf(u32 handle, u32 occ)
{
    return (handle * 2654435761u ^ occ * 40503u) & (SET_HASH - 1);
}

static void HashAdd(PSet *s, u32 i)
{
    u32 h = HashOf(s->d[i].handle, s->d[i].occ), k;
    for (k = 0; k < SET_HASH; k++, h = (h + 1) & (SET_HASH - 1))
        if (!s->hash[h]) {
            s->hash[h] = (u16)(i + 1);
            return;
        }
}

static PDraw *Lookup(PSet *s, u32 handle, u32 occ)
{
    u32 h = HashOf(handle, occ), k;
    if (!s->hash)
        return 0;
    for (k = 0; k < SET_HASH; k++, h = (h + 1) & (SET_HASH - 1)) {
        u16 x = s->hash[h];
        if (!x)
            return 0;
        if (s->d[x - 1].handle == handle && s->d[x - 1].occ == occ)
            return &s->d[x - 1];
    }
    return 0;
}

static u32 Occurrence(u32 handle)
{
    u32 h = (handle * 2654435761u) & (OCC_SLOTS - 1), k;
    for (k = 0; k < OCC_SLOTS; k++, h = (h + 1) & (OCC_SLOTS - 1)) {
        if (!sOcc[h].count || sOcc[h].handle == handle) {
            sOcc[h].handle = handle;
            return sOcc[h].count++;
        }
    }
    return 0xFFFFFFFFu;
}

static void SetsInit(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        sSet[i].d = Host_Alloc(SET_DRAWS * sizeof(PDraw));
        sSet[i].b = Host_Alloc(SET_BONES * sizeof(PBone));
        sSet[i].hash = Host_Alloc(SET_HASH * sizeof(u16));
        sSet[i].p2 = Host_Alloc(P2_MAX * sizeof(P2));
        sSet[i].p2v = Host_Alloc(P2V_MAX * sizeof(u32));
        sSet[i].np2 = sSet[i].nv2 = 0;
        if (!sSet[i].p2 || !sSet[i].p2v)
            sSet[i].p2 = 0;
        if (!sSet[i].d || !sSet[i].b || !sSet[i].hash) {
            Host_Log("mh2-core: pass: no memory for the sets: the models are not blended");
            sSet[i].d = 0;
            sSet[i].hash = 0;
        } else
            memset(sSet[i].hash, 0, SET_HASH * sizeof(u16));
    }
}


static void Num(char *line, u32 *n, u64 v)
{
    char t[24];
    u32 k = 0, i;
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (i = 0; i < k; i++)
        line[(*n)++] = t[k - 1 - i];
}

static void Str(char *line, u32 *n, const char *s)
{
    while (*s)
        line[(*n)++] = *s++;
}

static void Init(void)
{
    const char *m = PASS_ENV("MH2_PASS");
    const char *c = PASS_ENV("MH2_PASS_CTL");
    const char *a = PASS_ENV("MH2_PASS_ALPHA");
    (void)m;
    sMode = Pal_GsLabels ? 3 : 0;   /* a release: the pass on (C) exactly when the host made its ring */
    sBack = sMode == 4 ? 2 : 1;
    sDrop = PASS_ENV("MH2_PASS_DROP") ? PASS_ENV("MH2_PASS_DROP")[0] - '0' : 0;
    sLog = PASS_ENV("MH2_PASS_LOG") ? (PASS_ENV("MH2_PASS_LOG")[0] == '6' ? 6 : PASS_ENV("MH2_PASS_LOG")[0] == '4' ? 4 : PASS_ENV("MH2_PASS_LOG")[0] == '5' ? 5 : PASS_ENV("MH2_PASS_LOG")[0] == '3' ? 3 : PASS_ENV("MH2_PASS_LOG")[0] == '2' ? 2 : 1) : 0;
    sCtl = c && c[0] == '1';
    sBD = PASS_ENV("MH2_PASS_BD") && PASS_ENV("MH2_PASS_BD")[0] == '0' ? 0 : PASS_ENV("MH2_PASS_BD") && PASS_ENV("MH2_PASS_BD")[0] == '1' ? 1 : 2;
    if (PASS_ENV("MH2_PASS_REPEAT")) {
        u32 v = 0;
        for (const char *q = PASS_ENV("MH2_PASS_REPEAT"); *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (u32)(*q - '0');
        sRepeat = v < 1 ? 1 : v > 16 ? 16 : (int)v;
    }
    if (PASS_ENV("MH2_PASS_SPIN"))
        for (const char *q = PASS_ENV("MH2_PASS_SPIN"); *q >= '0' && *q <= '9'; q++)
            sSpinMs = sSpinMs * 10 + (*q - '0');
    sFlipMode = PASS_ENV("MH2_PASS_FLIP") && PASS_ENV("MH2_PASS_FLIP")[0] >= '0' && PASS_ENV("MH2_PASS_FLIP")[0] <= '2' ? PASS_ENV("MH2_PASS_FLIP")[0] - '0' : 1;
    sDataMats = !(PASS_ENV("MH2_PASS_DATAMATS") && PASS_ENV("MH2_PASS_DATAMATS")[0] == '0');
    sLerp = PASS_ENV("MH2_PASS_LERP") && PASS_ENV("MH2_PASS_LERP")[0] == '1';
    if (PASS_ENV("MH2_PASS_GATE") && PASS_ENV("MH2_PASS_GATE")[0] == '2')
        sGate = 2.0;
    for (int w = 0; w < 2; w++) {
        const char *q = PASS_ENV(w ? "MH2_PASS_ONLYH" : "MH2_PASS_DROPH");
        u32 *list = w ? sOnlyH : sDropH, *cnt = w ? &sNOnlyH : &sNDropH, v = 0;
        int have = 0;
        for (; q && *cnt < 16; q++) {
            if (*q >= '0' && *q <= '9') {
                v = v * 10 + (u32)(*q - '0');
                have = 1;
            } else {
                if (have)
                    list[(*cnt)++] = v;
                v = 0;
                have = 0;
                if (!*q)
                    break;
            }
        }
    }
    sNoHdr = PASS_ENV("MH2_PASS_NOHDR") != 0;
    sHoldSafe = PASS_ENV("MH2_PASS_HOLDRAW") == 0;
    sDefer = PASS_ENV("MH2_PASS_DEFER") ? PASS_ENV("MH2_PASS_DEFER")[0] - '0' : 0;
    if (sDefer < 0 || sDefer > 3)
        sDefer = 0;
    if (PASS_ENV("MH2_PASS_HOLD")) {
        u32 hv = 0;
        for (const char *q = PASS_ENV("MH2_PASS_HOLD"); *q >= '0' && *q <= '9'; q++)
            hv = hv * 10 + (u32)(*q - '0');
        sHoldPx = (double)hv;
    }
    if (PASS_ENV("MH2_PASS_HDR"))
        for (const char *q = PASS_ENV("MH2_PASS_HDR"); *q >= '0' && *q <= '9'; q++)
            sHdrH = sHdrH * 10 + (u32)(*q - '0');
    if (PASS_ENV("MH2_PASS_DROPCLS"))
        for (const char *q = PASS_ENV("MH2_PASS_DROPCLS"); *q >= '0' && *q <= '9'; q++)
            sDropCls = sDropCls * 10 + (u32)(*q - '0');
    if (PASS_ENV("MH2_PASS_P2") && PASS_ENV("MH2_PASS_P2")[0] == '0')
        sP2 = 0;
    if (PASS_ENV("MH2_PASS_HOLD2") && PASS_ENV("MH2_PASS_HOLD2")[0] == '0')
        sHold2 = 0;
    if (PASS_ENV("MH2_PASS_FADE") && PASS_ENV("MH2_PASS_FADE")[0] == '0')
        sFade = 0;
    if (PASS_ENV("MH2_PASS_DROPUNL"))
        for (const char *q = PASS_ENV("MH2_PASS_DROPUNL"); *q >= '0' && *q <= '9'; q++)
            sDropUnl = sDropUnl * 10 + (u32)(*q - '0');
    if (PASS_ENV("MH2_PASS_DROPE")) {
        const char *q = PASS_ENV("MH2_PASS_DROPE");
        for (; *q >= '0' && *q <= '9'; q++)
            sDropE0 = sDropE0 * 10 + (u32)(*q - '0');
        for (q = *q == ',' ? q + 1 : q; *q >= '0' && *q <= '9'; q++)
            sDropE1 = sDropE1 * 10 + (u32)(*q - '0');
    }
    sMomCtl = c && c[0] == '2';
    sStill = PASS_ENV("MH2_PASS_STILL") && PASS_ENV("MH2_PASS_STILL")[0] == '1';
    sCensus = sMode >= 3 && PASS_ENV("MH2_PASS_CENSUS") && PASS_ENV("MH2_PASS_CENSUS")[0] >= '1' && PASS_ENV("MH2_PASS_CENSUS")[0] <= '2' ? PASS_ENV("MH2_PASS_CENSUS")[0] - '0' : 0;
    if (sCensus) {
        sCzTsc0 = __builtin_ia32_rdtsc();
        sCzNs0 = Ipc_TimeNs();
    }
    /* The switches that are on unless their variable begins with 0 (the old way); 2, where a switch has one, is its compare. */
#define PASS_SW(name, two) (!PASS_ENV(name) ? 1 : PASS_ENV(name)[0] == '0' ? 0 : ((two) && PASS_ENV(name)[0] == '2') ? 2 : 1)
    sHMod = sMode >= 3 && PASS_SW("MH2_PASS_HMODELS", 0);
    sHModCtl = sHMod && PASS_ENV("MH2_PASS_HMODCTL") ? PASS_ENV("MH2_PASS_HMODCTL")[0] - '0' : 0;
    sWalkTag = sMode >= 3 ? PASS_SW("MH2_PASS_WALKTAG", 1) : 0;
    sWalkCtl = sWalkTag && PASS_ENV("MH2_PASS_WALKCTL") ? PASS_ENV("MH2_PASS_WALKCTL")[0] - '0' : 0;
    sDIx = sMode >= 3 ? PASS_SW("MH2_PASS_DRAWIDX", 1) : 0;
    sDHo = sMode >= 3 ? PASS_SW("MH2_PASS_HASHONCE", 1) : 0;
#undef PASS_SW
    sD2x = sMode >= 3 && PASS_ENV("MH2_PASS_D2IDX") && (PASS_ENV("MH2_PASS_D2IDX")[0] == '1' || PASS_ENV("MH2_PASS_D2IDX")[0] == '2') ? PASS_ENV("MH2_PASS_D2IDX")[0] - '0' : 0;
    sDCtl = (sDIx || sDHo || sD2x) && PASS_ENV("MH2_PASS_DRAWCTL") ? PASS_ENV("MH2_PASS_DRAWCTL")[0] - '0' : 0;
    if (PASS_ENV("MH2_PASS_ALPHAS") && sMode >= 3 && PASS_ENV("MH2_PASS_ALPHAS")[0] == 's') {
        sSweep = 1;   /* the moments of each step are set when its window runs (SweepSet) */
        sNMom = MOM_MAX;
        for (u32 i = 0; i < MOM_MAX; i++)
            sMomA[i] = 128;
    } else if (PASS_ENV("MH2_PASS_ALPHAS") && sMode >= 3) {
        const char *q = PASS_ENV("MH2_PASS_ALPHAS");
        sNMom = 0;
        while (*q && sNMom < MOM_MAX) {
            u32 v = 0;
            for (; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (u32)(*q - '0');
            sMomA[sNMom++] = v > 256 ? 256 : v;
            while (*q && *q != ',')
                q++;
            q += *q == ',';
        }
        if (!sNMom)
            sNMom = 1, sMomA[0] = 128;
    }
    if (sMode >= 3 && (PASS_FIXED || (PASS_ENV("MH2_PASS_LIVE") && PASS_ENV("MH2_PASS_LIVE")[0] == '1'))) {
        sLive = 1;   /* the moments of each step are the host's plan's (LiveDecide); the room for MOM_MAX of them, as the sweep's */
        sSweep = 0;
        sNMom = MOM_MAX;
        for (u32 i = 0; i < MOM_MAX; i++)
            sMomA[i] = 128;
    }
    if (a && a[0] == '1' && !a[1])
        sAlpha = 1.0;   /* MH2_PASS_ALPHA=1: the step itself */
    else if (a && a[0] == '0' && a[1] == '.') {   /* MH2_PASS_ALPHA=0.xxx */
        double v = 0, d = 0.1;
        for (const char *q = a + 2; *q >= '0' && *q <= '9'; q++, d /= 10)
            v += (*q - '0') * d;
        sAlpha = v;
    }
    sThread = !PASS_COPY && sMode && (PASS_FIXED || (PASS_ENV("MH2_PASS_THREAD") && PASS_ENV("MH2_PASS_THREAD")[0] == '1'));
    if (sMode && (vif_state(0, 0) > sizeof(sVif0) || vu1_state(0, 0) > sizeof(sVus0))) {
        Host_Log("mh2-core: pass: a unit's state is larger than the pass's buffer: the pass is off");
        sMode = 0;
    }
    if (sMode) {
        int ok = 1;
        if (!sThread && !PASS_COPY) {   /* the buffers of the window being copied (with a pass thread they are the slots') */
            sBuf = Host_Alloc(sizeof(PassBuf));
            sEv = Host_Alloc(EV_CAP);
            sEvCap = EV_CAP;
            sReal = Host_Alloc(REAL_CAP);
            sRealCap = REAL_CAP;
            sDraws = Host_Alloc(MAX_DRAWS * sizeof(IpcLabel));
            sDrawCap = MAX_DRAWS;
            ok = sBuf && sEv && sReal && sDraws;
        }
        if (!sThread) {   /* what the pass itself needs */
            sSb = Host_Alloc(4 * GFX_PASS_SB_QW * 16);
            sTV = Host_Alloc((T_N + 1) * sizeof *sTV);
            sMV = Host_Alloc((MV_N + 1) * sizeof *sMV);
            ok = ok && sSb && sTV && sMV;
        }
        if (!ok) {
            Host_Log("mh2-core: pass: no memory: the pass is off");
            sMode = 0;
            sThread = 0;
        }
    }
    if (sMode >= 3 && (sNMom > 1 || sStill || sCensus) && !sThread && !PASS_HELPER) {
        sKeep = Host_Alloc(EV_CAP);
        if (!sKeep) {
            Host_Log("mh2-core: pass: no memory for the moments' image: one moment");
            sNMom = 1;
            sStill = 0;
            sSweep = 0;
        }
    }
    if (!PASS_HELPER)
        TapeInit();
    if (sMode >= 3 && PASS_ENV("MH2_PASS_ALPHAS"))
        sAlpha = (double)sMomA[0] / 256.0;
    if (sMode >= 3 && !sThread && !PASS_HELPER)
        SetsInit();
    sTime = sMode > 0 && !sThread;
    sHashOn = sMode >= 2 && !sThread && PASS_ENV("MH2_PASS_HASH") && PASS_ENV("MH2_PASS_HASH")[0] != '0';
    if (sHashOn) {
        sHTab = Host_Alloc(HASH_MAX * sizeof *sHTab);
        if (!sHTab)
            sHashOn = 0;
        sHChain = 0xCBF29CE484222325ull;
    }
    if (sMode)
        Ipc_GsKeepLabels(1);
    if (sThread && !PassJob_Start()) {
        Host_Log("mh2-core: pass: the pass thread did not start: the pass is off");
        sMode = 0;
        sThread = 0;
        sTime = 0;
    }
}

static void TSort(u32 *a, u32 n)
{
    u32 gap, i, j, x;
    for (gap = n / 2; gap; gap /= 2)
        for (i = gap; i < n; i++) {
            x = a[i];
            for (j = i; j >= gap && a[j - gap] > x; j -= gap)
                a[j] = a[j - gap];
            a[j] = x;
        }
}

static void TReport(void)
{
    static const char *const names[T_N] = {"copy", "finding", "Models", "saves and loads", "Resend", "(of Resend: the put to the ring)", "each repeat of Resend", "spin", "(of Models: HdrBlend)", "(of Models: Direct2D)", "(of the later moments: Models)", "(of the later moments: the units put back)", "(of the later moments: Resend)", "(of the later moments' Models: HdrBlend)", "(of the later moments' Models: Direct2D)", "(a window's whole pass, timed from outside)", "total of a step"};
    u32 *tmp;
    char line[300];
    u32 n = 0, k, i;
    Str(line, &n, "mh2-core: pass time: steps ");
    Num(line, &n, sTN);
    Str(line, &n, " windows ");
    Num(line, &n, sTWinAll);
    Str(line, &n, " repeat ");
    Num(line, &n, (u64)sRepeat);
    Str(line, &n, " spin-ms ");
    Num(line, &n, (u64)sSpinMs);
    Str(line, &n, " timed-calls ");
    Num(line, &n, sTCalls);
    line[n] = 0;
    Host_Log(line);
    if (!sTN)
        return;
    tmp = sTV[T_N];   /* sTN counts only the steps written into sTV */
    for (k = 0; k < T_N; k++) {
        u64 sum = 0;
        for (i = 0; i < sTN; i++) {
            tmp[i] = sTV[k][i];
            sum += tmp[i];
        }
        TSort(tmp, sTN);
        n = 0;
        Str(line, &n, "mh2-core: pass time (us a step): ");
        Str(line, &n, names[k]);
        Str(line, &n, " mean ");
        Num(line, &n, sum / sTN);
        Str(line, &n, " median ");
        Num(line, &n, tmp[sTN / 2]);
        Str(line, &n, " p95 ");
        Num(line, &n, tmp[(u64)sTN * 95ull / 100ull]);
        Str(line, &n, " worst ");
        Num(line, &n, tmp[sTN - 1]);
        line[n] = 0;
        Host_Log(line);
    }
}

/* One row of the moment and still tables: the mean, the median, the 95th percentile and the worst of a column, in microseconds. */
static void MRow(const char *head, const char *name, u32 k)
{
    char line[300];
    u32 n = 0, i, cnt = sMVn[k];
    u32 *tmp = sMV[MV_N];
    u64 sum = 0;
    Str(line, &n, head);
    Str(line, &n, name);
    if (!cnt) {
        Str(line, &n, " none");
        line[n] = 0;
        Host_Log(line);
        return;
    }
    for (i = 0; i < cnt; i++) {
        tmp[i] = sMV[k][i];
        sum += tmp[i];
    }
    TSort(tmp, cnt);
    Str(line, &n, " mean ");
    Num(line, &n, sum / cnt);
    Str(line, &n, " median ");
    Num(line, &n, tmp[cnt / 2]);
    Str(line, &n, " p95 ");
    Num(line, &n, tmp[(u64)cnt * 95ull / 100ull]);
    Str(line, &n, " worst ");
    Num(line, &n, tmp[cnt - 1]);
    Str(line, &n, " windows ");
    Num(line, &n, cnt);
    line[n] = 0;
    Host_Log(line);
}

static void MReport(void)
{
    static const char *const mom[MOM_MAX] = {"moment 1 done, from its window's end", "moment 2 done, from its window's end", "moment 3 done, from its window's end", "moment 4 done, from its window's end", "moment 5 done, from its window's end", "moment 6 done, from its window's end"};
    char line[200];
    u32 n = 0, k;
    if (!sMV)
        return;
    for (k = 0; k < MOM_MAX; k++)
        if (sMVn[k])
            MRow("mh2-core: step order (us): ", mom[k], k);
    if (!sStill)
        return;
    Str(line, &n, "mh2-core: pass still: windows ");
    Num(line, &n, sStillWins + sChgWins);
    Str(line, &n, " still ");
    Num(line, &n, sStillWins);
    Str(line, &n, " changed ");
    Num(line, &n, sChgWins);
    line[n] = 0;
    Host_Log(line);
    MRow("mh2-core: pass still (us): ", "Resend of a still window", MV_RS_STILL);
    MRow("mh2-core: pass still (us): ", "Resend of a changed window", MV_RS_CHG);
    MRow("mh2-core: pass still (us): ", "Resend of the later moments of a still window", MV_LRS_STILL);
    MRow("mh2-core: pass still (us): ", "Resend of the later moments of a changed window", MV_LRS_CHG);
}

#ifdef GFX_INST
#define CZ_INST GFX_INST
#else
#define CZ_INST 0
#endif

static void CzLine(char *line, u32 *n, const char *what)
{
    *n = 0;
    Str(line, n, "mh2-core: pass census ");
    Num(line, n, CZ_INST);
    Str(line, n, ": ");
    Str(line, n, what);
}

static void CzAcc_(char *line, u32 *n, const CzAcc *a)
{
    Str(line, n, " pieces ");
    Num(line, n, a->pieces);
    Str(line, n, " copy-qw ");
    Num(line, n, a->qw);
    Str(line, n, " runs ");
    Num(line, n, a->runs);
    Str(line, n, " out-qw ");
    Num(line, n, a->out_qw);
    Str(line, n, " ticks ");
    Num(line, n, a->ticks);
    Str(line, n, " out-ticks ");
    Num(line, n, a->out_ticks);
    line[*n] = 0;
    Host_Log(line);
}

/* MH2_PASS_CENSUS: this copy's rows, in ticks; the first line has the clock's span and the tick counter's over the same time */
static void CzReport(void)
{
    static const char *const names[CZ_N] = {"tag", "path3", "unlabelled", "moved", "vertex-animated", "still", "no-matrix", "no-partner", "cut", "no-bone", "no-handle", "other", "flipbook"};
    char line[400];
    u32 n, k, c;
    if (!sCensus || (!sCzMoments && !sCzDecWin))
        return;
    CzLine(line, &n, "clock tsc-span ");
    Num(line, &n, __builtin_ia32_rdtsc() - sCzTsc0);
    Str(line, &n, " ns-span ");
    Num(line, &n, Ipc_TimeNs() - sCzNs0);
    Str(line, &n, " control ");
    Num(line, &n, (u64)(sCensus == 2));
    line[n] = 0;
    Host_Log(line);
    if (sCzDecWin) {
        CzLine(line, &n, "deciding windows ");
        Num(line, &n, sCzDecWin);
        Str(line, &n, " head-ns ");
        Num(line, &n, sCzDecHeadNs);
        Str(line, &n, " marked ");
        Num(line, &n, (u64)(sKeep != 0));
        line[n] = 0;
        Host_Log(line);
        for (k = 0; k < 6; k++)
            if (sCzDec[k][3]) {
                CzLine(line, &n, "deciding moment ");
                Num(line, &n, k);
                Str(line, &n, " count ");
                Num(line, &n, sCzDec[k][3]);
                Str(line, &n, " models-ticks ");
                Num(line, &n, sCzDec[k][0]);
                Str(line, &n, " wait-ticks ");
                Num(line, &n, sCzDec[k][1]);
                Str(line, &n, " fill-ticks ");
                Num(line, &n, sCzDec[k][2]);
                line[n] = 0;
                Host_Log(line);
            }
    }
    if (!sCzMoments)
        return;
    CzLine(line, &n, "moments ");
    Num(line, &n, sCzMoments);
    Str(line, &n, " pre-ticks ");
    Num(line, &n, sCzPre);
    Str(line, &n, " load-ticks ");
    Num(line, &n, sCzLoad);
    Str(line, &n, " put-ticks ");
    Num(line, &n, sCzPut);
    Str(line, &n, " sites ");
    Num(line, &n, sCzSiteN);
    Str(line, &n, " own-models ");
    Num(line, &n, sCzModN);
    Str(line, &n, " own-models-ticks ");
    Num(line, &n, sCzMod);
    line[n] = 0;
    Host_Log(line);
    CzLine(line, &n, "stream walk-ticks ");
    Num(line, &n, sCzSt[0][0]);
    Str(line, &n, " walk-qw ");
    Num(line, &n, sCzSt[0][1]);
    Str(line, &n, " aside-ticks ");
    Num(line, &n, sCzSt[1][0]);
    Str(line, &n, " aside-qw ");
    Num(line, &n, sCzSt[1][1]);
    Str(line, &n, " send-ticks ");
    Num(line, &n, sCzSt[2][0]);
    Str(line, &n, " send-qw ");
    Num(line, &n, sCzSt[2][1]);
    line[n] = 0;
    Host_Log(line);
    for (k = 0; k < CZ_N; k++)
        for (c = 0; c < 2; c++)
            if (sCz[k][c].pieces) {
                CzLine(line, &n, "row ");
                Str(line, &n, names[k]);
                Str(line, &n, c ? " written 1" : " written 0");
                CzAcc_(line, &n, &sCz[k][c]);
            }
    for (k = 0; k <= CZ_SITES; k++)
        if (sCzSite[k].pieces) {
            CzLine(line, &n, "site ");
            Num(line, &n, k < CZ_SITES ? sCzSiteKey[k][0] : 0);
            Str(line, &n, " prog ");
            Num(line, &n, k < CZ_SITES ? sCzSiteKey[k][1] : 0);
            Str(line, &n, k < CZ_SITES ? " kept 1" : " kept 0");
            CzAcc_(line, &n, &sCzSite[k]);
        }
}

static void WtReport(void);
static void DxReport(void);

void Gfx_PassFinish(void)
{
    char line[1300];
    u32 n = 0;
    if (sThread)   /* a release: the pass thread's queue drained; the counts are a development log */
        PassJob_Finish();
    (void)line;
    (void)n;
    return;
    CzReport();
    WtReport();
    DxReport();
#ifndef MH2_PASS_COPY
    if (sLive) {
        Str(line, &n, "mh2-core: pass live: steps with the plan in time ");
        Num(line, &n, sLv[0]);
        Str(line, &n, " old plan ");
        Num(line, &n, sLv[1]);
        Str(line, &n, " other plan ");
        Num(line, &n, sLv[2]);
        Str(line, &n, " no plan ");
        Num(line, &n, sLv[3]);
        Str(line, &n, " plans named at the VSync ");
        Num(line, &n, sLv[5]);
        line[n] = 0;
        Host_Log(line);
        n = 0;
    }
#endif
    if (sThread) {   /* the queue is drained and the pass thread's counts are logged by it */
        PassJob_Finish();
        return;
    }
    if (sHashOn) {
        u32 i;
        char hl[200];
        for (i = 0; i < sHN; i++) {
            u32 m = 0;
            if (PASS_ENV("MH2_PASS_HASH")[0] != '2')
                break;
            Str(hl, &m, "mh2-core: pass hash ");
            Num(hl, &m, sHTab[i].window);
            Str(hl, &m, " ");
            Num(hl, &m, sHTab[i].moment);
            Str(hl, &m, " ");
            Num(hl, &m, sHTab[i].h >> 32);
            Str(hl, &m, " ");
            Num(hl, &m, sHTab[i].h & 0xFFFFFFFFull);
            hl[m] = 0;
            Host_Log(hl);
        }
        {
            u32 m = 0;
#ifdef MH2_PASS_HELPER
            Str(hl, &m, "mh2-core: pass helper ");
            Num(hl, &m, GFX_INST);
            Str(hl, &m, " hash total: moments ");
#else
            Str(hl, &m, "mh2-core: pass hash total: moments ");
#endif
            Num(hl, &m, sHN);
            Str(hl, &m, " chain ");
            Num(hl, &m, sHChain >> 32);
            Str(hl, &m, " ");
            Num(hl, &m, sHChain & 0xFFFFFFFFull);
            hl[m] = 0;
            Host_Log(hl);
        }
    }
#ifdef MH2_PASS_HELPER
    return;   /* a helper logs its hashes only (its counts are the deciding copy's report's work) */
#endif
    if (sTapeOn) {
        char tl[200];
        u32 m = 0;
        Str(tl, &m, "mh2-core: pass tape: recorded ");
        Num(tl, &m, sTapeRec);
        Str(tl, &m, " blending ");
        Num(tl, &m, sTapeBlend);
        Str(tl, &m, " taken ");
        Num(tl, &m, sTapeHit);
        Str(tl, &m, " not-taken ");
        Num(tl, &m, sTapeMiss);
        Str(tl, &m, " over-capacity ");
        Num(tl, &m, sTapeOver);
        Str(tl, &m, " control-entry ");
        Num(tl, &m, sTapeCtlDone);
        tl[m] = 0;
        Host_Log(tl);
    }
    TReport();
    MReport();
    if (sMode <= 0)
        return;
    Str(line, &n, "mh2-core: pass: windows ");
    Num(line, &n, sN.windows);
    Str(line, &n, " compared ");
    Num(line, &n, sN.run);
    Str(line, &n, " differ ");
    Num(line, &n, sN.differ);
    Str(line, &n, " end-state-differ ");
    Num(line, &n, sN.end_differ);
    Str(line, &n, " skipped ");
    Num(line, &n, sN.skipped_dirty);
    Str(line, &n, "+");
    Num(line, &n, sN.skipped_over);
    Str(line, &n, " draws ");
    Num(line, &n, sN.draws);
    Str(line, &n, " matrices-found ");
    Num(line, &n, sN.found);
    Str(line, &n, " control-changed ");
    Num(line, &n, sN.ctl_changed);
    Str(line, &n, " control-differ ");
    Num(line, &n, sN.ctl_differ);
    Str(line, &n, " camera-rewritten ");
    Num(line, &n, sN.cam_done);
    Str(line, &n, " other-view ");
    Num(line, &n, sN.cam_other);
    Str(line, &n, " cut ");
    Num(line, &n, sN.cam_cut);
    Str(line, &n, " no-matrix ");
    Num(line, &n, sN.cam_nomatrix);
    Str(line, &n, " rigid-blended ");
    Num(line, &n, sN.mod_rigid);
    Str(line, &n, " rigid-still ");
    Num(line, &n, sN.mod_still);
    Str(line, &n, " rigid-cut ");
    Num(line, &n, sN.mod_cut);
    Str(line, &n, " rigid-birth ");
    Num(line, &n, sN.mod_birth);
    Str(line, &n, " skin-blended ");
    Num(line, &n, sN.skin_blended);
    Str(line, &n, " skin-still ");
    Num(line, &n, sN.skin_still);
    Str(line, &n, " skin-cut ");
    Num(line, &n, sN.skin_cut);
    Str(line, &n, " skin-birth ");
    Num(line, &n, sN.skin_birth);
    Str(line, &n, " px-held ");
    Num(line, &n, sN.hold_px);
    Str(line, &n, " header-draws ");
    Num(line, &n, sN.hdr_draws);
    Str(line, &n, " header-qwords ");
    Num(line, &n, sN.hdr_qwords);
    Str(line, &n, " packets-2d ");
    Num(line, &n, sN.p2_packets);
    Str(line, &n, " blended ");
    Num(line, &n, sN.p2_blended);
    Str(line, &n, " faded ");
    Num(line, &n, sN.p2_faded);
    Str(line, &n, " skin-nobones ");
    Num(line, &n, sN.skin_nobones);
    Str(line, &n, " data-matrices ");
    Num(line, &n, sN.data_mats);
    Str(line, &n, " bones-blended ");
    Num(line, &n, sN.bones_blended);
    Str(line, &n, " bone-nopartner ");
    Num(line, &n, sN.bone_nopartner);
    Str(line, &n, " deaths ");
    Num(line, &n, sN.deaths);
    Str(line, &n, " set-full ");
    Num(line, &n, sN.set_full);
    Str(line, &n, " held-models ");
    Num(line, &n, sN.held_mod);
    Str(line, &n, " held-skins ");
    Num(line, &n, sN.held_skin);
    Str(line, &n, " held-rows ");
    Num(line, &n, sN.held_hdr);
    Str(line, &n, " held-2d ");
    Num(line, &n, sN.held_p2);
    Str(line, &n, " cut-moments-skipped ");
    Num(line, &n, sN.skip_mom);
    Str(line, &n, " vu1-kicks-labelled ");
    Num(line, &n, sN.kicks_lab);
    Str(line, &n, " vu1-kicks-unlabelled ");
    Num(line, &n, sN.kicks_unl);
    Str(line, &n, " out-qwords-p1 ");
    Num(line, &n, sN.out_q[1][0]);
    Str(line, &n, "+");
    Num(line, &n, sN.out_q[1][1]);
    Str(line, &n, " p2 ");
    Num(line, &n, sN.out_q[2][0]);
    Str(line, &n, "+");
    Num(line, &n, sN.out_q[2][1]);
    Str(line, &n, " p3 ");
    Num(line, &n, sN.out_q[3][0] + sN.out_q[3][1]);
    Str(line, &n, " vertex-moved-by-the-game ");
    Num(line, &n, sN.vanim);
    Str(line, &n, " unlabelled-matrices-found ");
    Num(line, &n, sN.unl_found);
    Str(line, &n, " rewritten ");
    Num(line, &n, sN.unl_rewritten);
    Str(line, &n, " path3-in-window ");
    Num(line, &n, sN.nested);
    Str(line, &n, " bytes ");
    Num(line, &n, sN.bytes);
    Str(line, &n, " max-copy ");
    Num(line, &n, sN.max_ev);
    Str(line, &n, " max-real ");
    Num(line, &n, sN.max_real);
    line[n] = 0;
    Host_Log(line);
    if (sMode >= 3 && sFlipMode) {
        u32 k;
        n = 0;
        Str(line, &n, "mh2-core: pass flip: candidates ");
        Num(line, &n, sN.flip_cand);
        Str(line, &n, " sent-twice ");
        Num(line, &n, sN.flip_draws);
        Str(line, &n, " path3 ");
        Num(line, &n, sN.flip_p3);
        Str(line, &n, " births ");
        Num(line, &n, sN.bd_born);
        Str(line, &n, " changed ");
        Num(line, &n, sN.bd_changed);
        Str(line, &n, " dead ");
        Num(line, &n, sN.bd_dead);
        Str(line, &n, " dead-changed ");
        Num(line, &n, sN.bd_dead_changed);
        Str(line, &n, " marks-seen ");
        Num(line, &n, sN.mark_seen);
        Str(line, &n, " marks-left ");
        Num(line, &n, sN.mark_leak);
        Str(line, &n, " held-by-why");
        for (k = 1; k < 10; k++) {
            line[n++] = ' ';
            Num(line, &n, sN.flip_held[k]);
        }
        line[n] = 0;
        Host_Log(line);
    }
}

/* ---- the window: the copy and the real stream ---- */


static void Save(int which)
{
    if (which == 2) {
        memcpy(sVu2, g_vu_mem, sizeof(g_vu_mem));
        vif_state(sVif2, 0);
        vu1_state(sVus2, 0);
        gif_state(&sGif2, 0);
        gs_state(sGs2, 0);
        return;
    }
    memcpy(which ? sVu1 : sVu0, g_vu_mem, sizeof(g_vu_mem));
    vif_state(which ? sVif1 : sVif0, 0);
    vu1_state(which ? sVus1 : sVus0, 0);
    gif_state(which ? &sGif1 : &sGif0, 0);
    gs_state(which ? sGs1 : sGs0, 0);
}

static void Load(int which)
{
    if (which == 2) {
        memcpy(g_vu_mem, sVu2, sizeof(g_vu_mem));
        vif_state(sVif2, 1);
        vu1_state(sVus2, 1);
        gif_state(&sGif2, 1);
        gs_state(sGs2, 1);
        return;
    }
    memcpy(g_vu_mem, which ? sVu1 : sVu0, sizeof(g_vu_mem));
    vif_state(which ? sVif1 : sVif0, 1);
    vu1_state(which ? sVus1 : sVus0, 1);
    gif_state(which ? &sGif1 : &sGif0, 1);
    gs_state(which ? sGs1 : sGs0, 1);
}

#ifndef MH2_PASS_COPY
u64 Pal_FreezeBias(void);
/* The first window of a step: the moments (above). The VSync record that follows the chain is the one of the VBlank the kernel is in (between a VBlank's start and its end), or of the next. */
static void LiveDecide(void)
{
    const u32 want = (u32)(sLvCur + (sLvIn ? 0 : 1) + Pal_FreezeBias());
    unsigned int vb = want, n = 0, fb = 1, al[6] = {0, 0, 0, 0, 0, 0}, k;
    if (sLFor == want)
        return;
    sLFor = want;
    sLVb = 0;
    sLNMom = 0;
    if (Ipc_GsLivePlan(&vb, &n, al, &fb)) {
        if (vb == want && n) {
            sLVb = vb;
            sLNMom = n;
            for (k = 0; k < n && k < MOM_MAX; k++)
                sLA[k] = al[k];
            sLv[0]++;
            return;
        }
        sLv[vb < want ? 1 : 2]++;
    } else
        sLv[3]++;
    sLNMom = fb > 1 ? 1 : fb;   /* the plan is not in time: its fallback (60: one moment at 128; above 60: none) */
    sLA[0] = 128;
}
#endif

static void Pass_WindowBegin_(void)
{
    if (sMode < 0)
        Init();
    if (!sMode || gGfxPassOpen || gGfxPassRun)
        return;
#ifndef MH2_PASS_COPY
    if (sLive)
        LiveDecide();
#endif
    if (sThread) {   /* the window is copied into a free slot's buffers; with none free it gets no pass (counted) */
        sJob = PassJob_Acquire();
        if (!sJob) {
            PassJob_Dropped();
            if (sMode >= 2)
                PassSend(5, 0, 0, 0);   /* no pass for this window: the step is not made from the pass */
            return;
        }
        sJob->serial = ++sWinSerial;
        sBuf = &sJob->buf;
        sEv = sJob->ev;
        sEvCap = sJob->evCap;
        sReal = sJob->real;
        sRealCap = sJob->realCap;
        sDraws = sJob->draws;
        sDrawCap = sJob->drawCap;
    }
    sEvLen = sRealLen = 0;
    sNDraws = 0;
    sWS = sRS;
    sCur = -1;
    sOverflow = 0;
    sDirty = gif_pending();   /* a tag waits for its data in a path buffer: the pass would start from an empty one */
    Save(0);
    gGfxPassOpen = 1;
    if (sMode >= 2)
        PassSend(0, sThread && sJob ? sJob->serial : 0, 0, 0);   /* the real records of the window follow (with a pass thread: the window's serial) */
}

/* A buffer of a slot grows (by doubling, from 1 MB, up to LIMIT) as the window the game's thread copies into it needs. */
static int Grow(u8 **buf, u32 *cap, u32 used, u32 need, u32 limit)
{
    u32 nc = *cap ? *cap : (1u << 20);
    u8 *nb;
    while (nc < need && nc < limit)
        nc = nc > limit / 2 ? limit : nc * 2;
    if (nc < need)
        return 0;
    nb = Host_Alloc(nc);
    if (!nb)
        return 0;
    if (used)
        memcpy(nb, *buf, used);
    if (*buf)
        Host_Free(*buf);
    *buf = nb;
    *cap = nc;
    return 1;
}

static PEv *Ev(u32 kind, u32 n, u32 payload)
{
    PEv *e;
    if (sEvLen + 16 + payload > EV_CAP) {
        sOverflow = 1;
        return 0;
    }
    if (sEvLen + 16 + payload > sEvCap && !Grow(&sEv, &sEvCap, sEvLen, sEvLen + 16 + payload, EV_CAP)) {
        sOverflow = 1;
        return 0;
    }
    e = (PEv *)(sEv + sEvLen);
    e->kind = kind;
    e->n = n;
    e->draw = (u32)sCur;
    e->pad = 0;
    sEvLen += 16 + payload;
    return e;
}

static void Pass_Data_(int ch, const void *p, u32 qwords)
{
    PEv *e = Ev(ch == 1 ? 1 : 3, qwords, qwords * 16);
    if (!e)
        return;
    if (ch != 1)
        sN.nested++;
    memcpy(e + 1, p, (size_t)qwords * 16);
}

static void Pass_Tag_(u64 hi)
{
    PEv *e = Ev(2, 0, 16);
    u64 q[2] = {hi, 0};
    if (e)
        memcpy(e + 1, q, 16);
}

/* the sets' bookkeeping of the draw n of the window (its label is sDraws[n]): the step's main view, and an entry in the step's set */
static void Book(u32 n)
{
    PSet *cs;
    sDIdx[n] = -1;
    cs = sSet[0].d ? Cur() : 0;
    if (cs) {
        const IpcLabel *l = &sDraws[n];
        if (!cs->have) {   /* the step's main view: the first labelled draw's */
            memcpy(cs->view, l->cam, sizeof(cs->view));
            memcpy(cs->proj, l->cam + 16, sizeof(cs->proj));
            memcpy(cs->vp, l->vp, sizeof(cs->vp));
            cs->have = 1;
        }
        if (l->handle) {
            if (cs->nd < SET_DRAWS) {
                PDraw *pd = &cs->d[cs->nd];
                pd->handle = l->handle;
                pd->occ = Occurrence(l->handle);
                pd->nb = pd->bfirst = pd->used = 0;
                pd->hqn = 0;
                memcpy(pd->world, l->world, sizeof(pd->world));
                sDIdx[n] = (int)cs->nd;
                HashAdd(cs, cs->nd);
                cs->nd++;
            } else
                sN.set_full++;
        }
    }
}

static void Pass_Model_(const void *label)
{
    if (!gGfxPassOpen)
        return;
    if (sNDraws >= MAX_DRAWS) {
        sCur = -1;
        return;
    }
    if (sNDraws >= sDrawCap) {   /* a slot's labels grow */
        u32 nc = sDrawCap ? sDrawCap * 2 : 256;
        IpcLabel *nb = Host_Alloc(nc * (u32)sizeof(IpcLabel));
        if (!nb) {
            sCur = -1;
            sOverflow = 1;
            return;
        }
        if (sNDraws)
            memcpy(nb, sDraws, sNDraws * sizeof(IpcLabel));
        if (sDraws)
            Host_Free(sDraws);
        sDraws = nb;
        sDrawCap = nc;
    }
    memcpy(&sDraws[sNDraws], label, sizeof(IpcLabel));
    if (!sThread)
        Book(sNDraws);
    sCur = (int)sNDraws++;
}

static void Pass_TagEnd_(u32 taddr)
{
    if (gGfxPassOpen && sCur >= 0 && taddr == sDraws[sCur].last)
        sCur = -1;
}

/* (the copy's calls, timed: one bucket, T_COPY) */
void Pass_WindowBegin(void)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    Pass_WindowBegin_();
    if (sTime && t0 && gGfxPassOpen)
        sTAcc[T_COPY] += Ipc_TimeNs() - t0, sTCalls++;
}

void Pass_Data(int ch, const void *p, u32 qwords)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    Pass_Data_(ch, p, qwords);
    if (sTime && t0)
        sTAcc[T_COPY] += Ipc_TimeNs() - t0, sTCalls++;
}

void Pass_Tag(u64 hi)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    Pass_Tag_(hi);
    if (sTime && t0)
        sTAcc[T_COPY] += Ipc_TimeNs() - t0, sTCalls++;
}

void Pass_Model(const void *label)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    Pass_Model_(label);
    if (sTime && t0)
        sTAcc[T_COPY] += Ipc_TimeNs() - t0, sTCalls++;
}

void Pass_TagEnd(u32 taddr)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    Pass_TagEnd_(taddr);
    if (sTime && t0)
        sTAcc[T_COPY] += Ipc_TimeNs() - t0, sTCalls++;
}

/* ---- the output stage: the GS registers of the pass's stream, and the weight of a kick ----
 *
 * The stream of a window (Pass_Real: the real one; Pass_Out: the pass's) is walked tag by tag (the units hand it complete tags), and the registers that
 * decide how a kick blends are kept: PRIM and PRMODE, ALPHA, TEST, ZBUF, TEX0, PABE, PRMODECONT. A draw that is sent twice with a weight each (a flipbook's two frames,
 * a sprite that is born in the span) has its vertex colours scaled as they leave the pass: R, G, B for a blend that adds or subtracts the source (the picture is then
 * the weighted sum), the alpha for the blend that mixes it with what is there. Anything else is not scaled (the class HOLD). */

static u32 sBirthW = 256;        /* the weight of a packet that is born (marked in bits 16 to 31 of its tag), in 256ths */
static int sTwice;               /* the draw that is walked is sent twice (a flipbook): its depth test must pass both sends */
static u32 sWgt = 256;           /* the weight of what is sent, in 256ths (256: as it is) */
static struct {
    u32 rgbaq, hold, why, cls, kicks;
    u64 attr, alpha, test, zbuf, tex0, prim;   /* of the first colour that was seen */
} sWk;

#define CL_RGB 1u
#define CL_ALPHA 2u

static void GsReg(GsSt *s, u32 addr, u64 v)
{
    int i = -1;
    switch (addr) {
    case 0x00: i = GS_PRIM; v &= 0x7FF; break;
    case 0x1A: i = GS_PMC; break;
    case 0x1B: i = GS_PRMODE; break;
    case 0x42: case 0x43: i = GS_ALPHA + (int)(addr - 0x42); break;
    case 0x47: case 0x48: i = GS_TEST + (int)(addr - 0x47); break;
    case 0x4E: case 0x4F: i = GS_ZBUF + (int)(addr - 0x4E); break;
    case 0x06: case 0x07: i = GS_TEX0 + (int)(addr - 0x06); break;
    case 0x49: i = GS_PABE; break;
    default: return;
    }
    s->r[i] = v;
    s->known |= 1u << i;
}

/* What a colour written now is: CL_RGB, CL_ALPHA, or 0 (with sWk.why: 1 the blend is off, 2 PABE, 3 a blend that is not one of the two, 4 the texture function,
 * 5 the alpha test (of the alpha class) or the destination alpha test, 6 a depth test that the second send would fail, 7 a register not seen). */
static u32 ClassNow(const GsSt *s, u32 *why)
{
    u64 prim = s->r[GS_PRIM], at, al, te, zb, t0;
    u32 ctx, A, B, C, D, kn = s->known, tme, zok;
    *why = 7;
    if (!(kn & (1u << GS_PRIM)))
        return 0;
    if (!(kn & (1u << GS_PMC)) || (s->r[GS_PMC] & 1))
        at = prim;
    else if (kn & (1u << GS_PRMODE))
        at = s->r[GS_PRMODE];
    else
        return 0;
    ctx = (u32)(at >> 9) & 1;
    sWk.attr = at;
    if (!(at & 0x40)) {
        *why = 1;
        return 0;
    }
    if ((s->r[GS_PABE] & 1) && (kn & (1u << GS_PABE))) {
        *why = 2;
        return 0;
    }
    if (!(kn & (1u << (GS_ALPHA + ctx))) || !(kn & (1u << (GS_TEST + ctx))) || (sTwice && !(kn & (1u << (GS_ZBUF + ctx)))))
        return 0;
    al = s->r[GS_ALPHA + ctx];
    te = s->r[GS_TEST + ctx];
    zb = (kn & (1u << (GS_ZBUF + ctx))) ? s->r[GS_ZBUF + ctx] : 0;
    t0 = s->r[GS_TEX0 + ctx];
    A = (u32)al & 3;
    B = (u32)(al >> 2) & 3;
    C = (u32)(al >> 4) & 3;
    D = (u32)(al >> 6) & 3;
    (void)C;
    tme = (u32)(at >> 4) & 1;
    sWk.alpha = al;
    sWk.test = te;
    sWk.zbuf = zb;
    sWk.tex0 = t0;
    if (tme && !(kn & (1u << (GS_TEX0 + ctx))))
        return 0;
    if (tme && ((t0 >> 35) & 3)) {
        *why = 4;
        return 0;
    }
    if (te & 0x4000) {   /* DATE */
        *why = 5;
        return 0;
    }
    zok = !sTwice || ((zb >> 32) & 1) ? 1u : (((te >> 17) & 3) == 1 || ((te >> 17) & 3) == 2);
    if (!zok) {
        *why = 6;
        return 0;
    }
    if (D == 1 && ((A == 0 && B == 2) || (A == 2 && B == 0)))
        return CL_RGB;
    if (C == 0 && D == 1 && ((A == 0 && B == 1) || (A == 1 && B == 2))) {
        u32 atst = (u32)(te >> 1) & 7, aref = (u32)(te >> 4) & 0xFF, afail = (u32)(te >> 12) & 3;
        /* an alpha test that a scaled alpha could fail where the blend would have changed the picture: ALWAYS cannot; GREATER and NOTEQUAL with a compare value of 0 fail only
         * at an alpha of 0 (the blend adds nothing there), when a failed pixel is not written at all */
        if ((te & 1) && !(atst == 1 || ((atst == 6 || atst == 7) && aref == 0 && afail == 0))) {
            *why = 5;
            return 0;
        }
        return CL_ALPHA;
    }
    *why = 3;
    return 0;
}

static u32 Scale8(u32 v, u32 w)
{
    return ((v & 0xFFu) * w + 128u) >> 8;
}

static void ScaleRgbaq(u32 *d, u32 cls, u32 w)
{
    if (cls == CL_RGB) {
        d[0] = (d[0] & ~0xFFu) | Scale8(d[0], w);
        d[1] = (d[1] & ~0xFFu) | Scale8(d[1], w);
        d[2] = (d[2] & ~0xFFu) | Scale8(d[2], w);
    } else
        d[3] = (d[3] & ~0xFFu) | Scale8(d[3], w);
}

static void LogMark(u32 cls, u32 why)
{
    char ln[300];
    u32 n = 0, k;
    static const char hx[] = "0123456789abcdef";
    const u64 v[5] = {sWk.attr, sWk.alpha, sWk.test, sWk.zbuf, sWk.tex0};
    Str(ln, &n, "mh2-core: pass mark cls ");
    Num(ln, &n, cls);
    Str(ln, &n, " why ");
    Num(ln, &n, why);
    Str(ln, &n, " attr/alpha/test/zbuf/tex0 ");
    for (k = 0; k < 5; k++) {
        u32 b2 = 0;
        while (b2 < 15 && (v[k] >> (60 - 4 * b2)) == 0)
            b2++;
        for (; b2 < 16; b2++)
            ln[n++] = hx[(v[k] >> (60 - 4 * b2)) & 15];
        ln[n++] = ' ';
    }
    ln[n] = 0;
    Host_Log(ln);
}

/* MH2_PASS_WALKTAG: the counters of a packet from its tag. A register list without PRIM, the two TEX0 and A+D writes no register that ClassNow reads, so every colour of the
 * packet has one class: the colours and the kicks are the list's entries times the loop count. Returns 0 when the data must be visited: such a register in the list, or a
 * colour that is to be scaled (scale: the packet is written with a weight below 256). */
static int WalkTag(GsSt *s, u64 regs, u32 nloop, u32 nreg, int scale)
{
    u32 k, nc = 0, nk = 0;
    for (k = 0; k < nreg; k++) {
        u32 reg = (u32)(regs >> (k * 4)) & 15;
        if (reg == 0x1)
            nc++;
        else if (reg == 0x4 || reg == 0x5)
            nk++;
        else if (reg == 0x0 || reg == 0x6 || reg == 0x7 || (reg == 0xE && sWalkCtl != 1))
            return 0;
    }
    if (nc && nloop) {
        u32 why = 0, cls = ClassNow(s, &why);
        if (cls && scale && sWalkCtl != 2)
            return 0;
        sWk.rgbaq += nc * nloop;
        if (!sWk.cls && cls)
            sWk.cls = cls;
        if (!cls) {
            sWk.hold += nc * nloop;
            sWk.why = why;
        }
    }
    sWk.kicks += nk * nloop;
    return 1;
}

static void WtReport(void)
{
    char line[300];
    u32 n = 0;
    if (!sWalkTag || !sWt[0])
        return;
    Str(line, &n, "mh2-core: pass walk by tag ");
    Num(line, &n, CZ_INST);
    Str(line, &n, ": packets ");
    Num(line, &n, sWt[0]);
    Str(line, &n, " taken ");
    Num(line, &n, sWt[1]);
    Str(line, &n, " qw ");
    Num(line, &n, sWt[2]);
    Str(line, &n, " taken-qw ");
    Num(line, &n, sWt[3]);
    Str(line, &n, " compared ");
    Num(line, &n, sWt[4]);
    Str(line, &n, " differ ");
    Num(line, &n, sWt[5]);
    Str(line, &n, " control ");
    Num(line, &n, (u64)sWalkCtl);
    line[n] = 0;
    Host_Log(line);
}

static void D2Report(void)
{
    static const char *const nm[7] = {": walks ", " places ", " compared ", " places-compared ", " differ ", " full ", " most "};
    char line[300];
    u32 n = 0, i;
    if (!sD2x)
        return;
    Str(line, &n, "mh2-core: pass 2d index ");
    Num(line, &n, CZ_INST);
    for (i = 0; i < 7; i++) {
        Str(line, &n, nm[i]);
        Num(line, &n, sD2s[i]);
    }
    Str(line, &n, " mode ");
    Num(line, &n, (u64)sD2x);
    line[n] = 0;
    Host_Log(line);
}

static void DxReport(void)
{
    char line[300];
    u32 n = 0;
    D2Report();
    if (!sDIx && !sDHo)
        return;
    Str(line, &n, "mh2-core: pass draw index ");
    Num(line, &n, CZ_INST);
    Str(line, &n, ": walks ");
    Num(line, &n, sDx[0]);
    Str(line, &n, " compared ");
    Num(line, &n, sDx[1]);
    Str(line, &n, " differ ");
    Num(line, &n, sDx[2]);
    Str(line, &n, " hashes-taken ");
    Num(line, &n, sDx[3]);
    Str(line, &n, " compared ");
    Num(line, &n, sDx[4]);
    Str(line, &n, " differ ");
    Num(line, &n, sDx[5]);
    Str(line, &n, " modes ");
    Num(line, &n, (u64)sDIx);
    Str(line, &n, " ");
    Num(line, &n, (u64)sDHo);
    Str(line, &n, " control ");
    Num(line, &n, (u64)sDCtl);
    line[n] = 0;
    Host_Log(line);
}

/* Walk the tags in qwords of p, with the registers s; when wr is set (it is p, or a copy of it) the colours of the kicks whose class is not HOLD are scaled in wr by w. sWk counts what it saw. */
static void Walk(GsSt *s, const u8 *p, u8 *wr, u32 qwords, u32 w)
{
    u32 q = 0;
    while (q < qwords) {
        u64 t, regs;
        u32 nloop, flg, nreg, len, i, wt, mk, mkdrop = 0;
        memcpy(&t, p + (size_t)q * 16, 8);
        memcpy(&regs, p + (size_t)q * 16 + 8, 8);
        nloop = (u32)(t & 0x7FFF);
        flg = (u32)(t >> 58) & 3;
        nreg = (u32)(t >> 60) & 15;
        if (!nreg)
            nreg = 16;
        len = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        wt = w;
        mk = 0;
        if ((t >> 16) & 0xFFFFu) {
            mk = 1;   /* the mark of a packet that was born: its weight, and the mark does not leave the pass */
            if (wr) {
                u32 *tw = (u32 *)(wr + (size_t)q * 16);
                tw[0] &= 0xFFFFu;
                sN.mark_seen++;
                if (sBD == 1)
                    wt = sBirthW;
                else {   /* the packet draws nothing: its vertices become the kinds that do not kick (XYZF3, XYZ3), whatever its blend; its register writes stay for what follows */
                    u32 nr;
                    for (nr = 0; nr < (nreg < 16 ? nreg : 16); nr++) {
                        u32 sh = (nr & 7) * 4, *rw = &tw[2 + (nr >> 3)], rg = (*rw >> sh) & 15;
                        if (rg == 4 || rg == 5)
                            *rw |= 8u << sh;
                    }
                    mkdrop = 1;
                }
            } else
                sN.mark_leak++;
        }
        q++;
        if ((t >> 46) & 1)
            GsReg(s, 0x00, (t >> 47) & 0x7FF);
        if (flg == 0 || flg == 1) {
            u32 n = flg == 0 ? nloop * nreg : nloop * nreg;
            int chk = 0, wrote = 0;
            GsSt s1;
            __typeof__(sWk) w1;
            if (sWalkTag) {
                sWt[0]++;
                sWt[2] += len + 1;
                if (!mk && q + len <= qwords) {   /* (a marked packet and one the piece cuts short are visited) */
                    if (sWalkTag != 2) {
                        if (WalkTag(s, regs, nloop, nreg, wr && wt < 256)) {
                            sWt[1]++;
                            sWt[3] += len + 1;
                            q += len;
                            continue;
                        }
                    } else {
                        GsSt s0 = *s;
                        __typeof__(sWk) w0 = sWk;
                        if (WalkTag(s, regs, nloop, nreg, wr && wt < 256)) {
                            sWt[1]++;
                            sWt[3] += len + 1;
                            s1 = *s;
                            w1 = sWk;
                            chk = 1;
                        }
                        *s = s0;
                        sWk = w0;
                    }
                }
            }
            for (i = 0; i < n && (flg == 0 ? q + i : q + i / 2) < qwords; i++) {
                u32 reg = (u32)(regs >> ((i % nreg) * 4)) & 15;
                const u8 *d = flg == 0 ? p + (size_t)(q + i) * 16 : p + (size_t)q * 16 + (size_t)i * 8;
                u64 v;
                memcpy(&v, d, 8);
                if (reg == 0x1 || reg == 0x5 || reg == 0x4) {
                    if (reg != 0x1) {
                        sWk.kicks++;
                        continue;
                    }
                    {
                        u32 why = 0, cls = ClassNow(s, &why);
                        sWk.rgbaq++;
                        if (mk && sLog == 6)
                            LogMark(cls, why);
                        if (!sWk.cls && cls) {
                            sWk.cls = cls;
                        }
                        if (!cls) {
                            sWk.hold++;
                            sWk.why = why;
                        } else if (wr && wt < 256) {
                            u8 *e = wr + (d - p);
                            wrote = 1;
                            if (flg == 0)
                                ScaleRgbaq((u32 *)e, cls, wt);
                            else {   /* RGBAQ as 64 bits: R, G, B, A in the low four bytes */
                                u32 c4[4] = {e[0], e[1], e[2], e[3]};
                                ScaleRgbaq(c4, cls, wt);
                                e[0] = (u8)c4[0];
                                e[1] = (u8)c4[1];
                                e[2] = (u8)c4[2];
                                e[3] = (u8)c4[3];
                            }
                        }
                    }
                } else if (reg == 0x0)
                    GsReg(s, 0x00, v);
                else if (reg == 0x6 || reg == 0x7)
                    GsReg(s, reg, v);
                else if (reg == 0xE && flg == 0) {
                    GsReg(s, d[8], v);
                    if (mkdrop && wr && (d[8] == 4 || d[8] == 5))   /* (d is p's: the address as written) */
                        wr[(d - p) + 8] = (u8)(d[8] | 8u);
                }
            }
            if (chk) {   /* MH2_PASS_WALKTAG=2: the old way's registers and counters against the tag's, and nothing written */
                sWt[4]++;
                if (wrote || memcmp(&s1, s, sizeof(s1)) || memcmp(&w1, &sWk, sizeof(w1)))
                    sWt[5]++;
            }
        }
        q += len;
    }
}

/* a draw that is sent twice (a flipbook): its first copy is held here until its class is known */
#define CAP_QW 4096u
#define CAP_IT 256u
static u8 sCapBuf[CAP_QW * 16] __attribute__((aligned(16)));
static struct {
    int path;
    u32 q, off;
} sCapIt[CAP_IT];
static u32 sCapN, sCapQ;
static int sCapOn, sCapOver;
static u8 sScr[CAP_QW * 16] __attribute__((aligned(16)));

static void Capture(int path, const void *data, u32 qwords)
{
    if (sCapN >= CAP_IT || sCapQ + qwords > CAP_QW) {
        sCapOver = 1;
        return;
    }
    sCapIt[sCapN].path = path;
    sCapIt[sCapN].q = qwords;
    sCapIt[sCapN].off = sCapQ;
    memcpy(sCapBuf + (size_t)sCapQ * 16, data, (size_t)qwords * 16);
    sCapQ += qwords;
    sCapN++;
}

/* the pass's stream, mode 3 and 4: the registers are followed, and a weight below 256 scales the colours of the kicks */
static void Emit(u32 path, const void *data, u32 qwords)   /* the pass's piece to the renderer; the repeats of a window throw it away */
{
    u64 t0;
    if (sQuiet)
        return;
    if (!sTime) {
        PassSend(3, path, data, qwords);
        return;
    }
    t0 = Ipc_TimeNs();
    PassSend(3, path, data, qwords);
    sTAcc[T_PUT] += Ipc_TimeNs() - t0;
}

/* (MH2_PASS_CENSUS: the same, each of its parts timed) */
static void StageCz(int path, const void *data, u32 qwords)
{
    u64 c0 = __builtin_ia32_rdtsc(), c1;
    const void *p = data;
    u32 w = 256;
    if (!((sWgt >= 256 && path != 2 && path != 3) || qwords > CAP_QW || path < 1 || path > 3)) {
        memcpy(sScr, data, (size_t)qwords * 16);
        c1 = __builtin_ia32_rdtsc();
        sCzSt[1][0] += c1 - c0;
        sCzSt[1][1] += qwords;
        c0 = c1;
        p = sScr;
        w = sWgt;
    }
    Walk(&sOS, (const u8 *)p, p == data ? 0 : sScr, qwords, w);
    c1 = __builtin_ia32_rdtsc();
    sCzSt[0][0] += c1 - c0;
    sCzSt[0][1] += qwords;
    Emit((u32)path, p, qwords);
    sCzSt[2][0] += __builtin_ia32_rdtsc() - c1;
    sCzSt[2][1] += qwords;
}

static void Stage(int path, const void *data, u32 qwords)
{
    if (sCzCur) {
        StageCz(path, data, qwords);
        return;
    }
    if ((sWgt >= 256 && path != 2 && path != 3) || qwords > CAP_QW || path < 1 || path > 3) {
        Walk(&sOS, (const u8 *)data, 0, qwords, 256);
        Emit((u32)path, data, qwords);
        return;
    }
    memcpy(sScr, data, (size_t)qwords * 16);
    Walk(&sOS, sScr, sScr, qwords, sWgt);
    Emit((u32)path, sScr, qwords);
}

void Pass_Real(int path, const void *data, u32 qwords)
{
    u32 head[4];
    if (sMode >= 3)
        Walk(&sRS, (const u8 *)data, 0, qwords, 256);
    if (sThread && sMode >= 3)   /* the pass thread compares nothing with the real stream: the stream is not kept */
        return;
    if (sRealLen + 16 + qwords * 16 > REAL_CAP) {
        sOverflow = 1;
        return;
    }
    if (sRealLen + 16 + qwords * 16 > sRealCap && !Grow(&sReal, &sRealCap, sRealLen, sRealLen + 16 + qwords * 16, REAL_CAP)) {
        sOverflow = 1;
        return;
    }
    head[0] = (u32)path;
    head[1] = qwords;
    head[2] = head[3] = 0;
    memcpy(sReal + sRealLen, head, 16);
    memcpy(sReal + sRealLen + 16, data, (size_t)qwords * 16);
    sRealLen += 16 + qwords * 16;
}

/* The pass's stream: compared with the real one, item by item. */
static void PassOut_(int path, const void *data, u32 qwords)
{
    u32 head[4];
    if (path >= 1 && path <= 3) {
        sN.out_q[path][sOutDraw != 0xFFFFFFFFu] += qwords;
        sStepQ[path][sOutDraw != 0xFFFFFFFFu] += qwords;
    }
    if ((sNDropH || sNOnlyH) && sMode >= 3 && sOutDraw < MAX_DRAWS) {
        u32 k, h = sDraws[sOutDraw].handle;
        for (k = 0; k < sNDropH; k++)
            if (sDropH[k] == h)
                return;
        if (sNOnlyH) {
            for (k = 0; k < sNOnlyH && sOnlyH[k] != h; k++)
                ;
            if (k == sNOnlyH)
                return;
        }
    }
    if (sDropCls && sMode >= 3 && sOutDraw < MAX_DRAWS && (sCls[sOutDraw] & sDropCls))
        return;
    if (sMode >= 3 && sOutDraw == 0xFFFFFFFFu && ((path >= 1 && path <= 3 && (sDropUnl & (1u << (path - 1)))) || (sEvIdx >= sDropE0 && sEvIdx < sDropE1 && sLog != 5)))
        return;
    if (sDrop && sMode >= 3 && ((sDrop == 1 && path == 1 && sOutDraw != 0xFFFFFFFFu) || (sDrop == 2 && path == 2 && sOutDraw == 0xFFFFFFFFu)))
        return;
    if (sMode >= 3) {
        if (sCapOn)
            Capture(path, data, qwords);
        else
            Stage(path, data, qwords);
        return;
    }
    if (sMode >= 2)
        Emit((u32)path, data, qwords);   /* the renderer's in-between picture */
    if (sDiff)
        return;
    if (sRealCur + 16 > sRealLen) {
        sDiff = 1;
        sDiffAt = sRealCur;
        return;
    }
    memcpy(head, sReal + sRealCur, 16);
    if (head[0] != (u32)path || head[1] != qwords || sRealCur + 16 + qwords * 16 > sRealLen ||
        memcmp(sReal + sRealCur + 16, data, (size_t)qwords * 16)) {
        sDiff = 1;
        sDiffAt = sRealCur;
        return;
    }
    sRealCur += 16 + qwords * 16;
}

void Pass_Out(int path, const void *data, u32 qwords)
{
    if (sCzCur) {   /* MH2_PASS_CENSUS: inside a Resend, the time of the pass's own handling of the piece's stream */
        u64 t0 = __builtin_ia32_rdtsc();
        PassOut_(path, data, qwords);
        t0 = __builtin_ia32_rdtsc() - t0;
        sCzCur->out_ticks += t0;
        sCzCur->out_qw += qwords;
        if (sCzCurSite) {
            sCzCurSite->out_ticks += t0;
            sCzCurSite->out_qw += qwords;
        }
        return;
    }
    PassOut_(path, data, qwords);
}

/* ---- the matrices of a draw in the copy ---- */

static double Sqrt(double x)   /* (no libm in the core) */
{
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_set_sd(x), _mm_set_sd(x)));
}

static void Mul4(const double *a, const double *b, double *o)
{
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0;
            for (k = 0; k < 4; k++)
                s += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = s;
        }
}

static void ToD(const float *f, double *d)
{
    int i;
    for (i = 0; i < 16; i++)
        d[i] = f[i];
}

/* A draw's two matrices from its label and a view: the model-to-screen matrix (model x view x projection; form 2, a skinned model's world-to-screen: view x
 * projection) and the clip-test matrix (the same with the viewport matrix for the projection), in the game's row-vector layout (Vu1_MtxMulChain). */
static void DrawMatrices(const IpcLabel *l, const double *m, const double *view, int form, double *n, double *c)
{
    double p[16], vp[16], mv[16];
    int i;
    ToD(l->cam + 16, p);
    ToD(l->vp, vp);
    if (form == 1)
        Mul4(m, view, mv);
    else
        for (i = 0; i < 16; i++)
            mv[i] = view[i];
    Mul4(mv, p, n);
    Mul4(mv, vp, c);
}

/* A place (the translation row of a model's or a bone's matrix, a world point) that moved between two steps by more than sHoldPx pixels on the screen of the
 * draw's own view (the clip projection's x/w and y/w, scaled by the half of a 640 by 448 picture): the renderer holds a kick that moves so far (the step's own: a jump of the
 * animation, not a motion to blend across). A place behind the eye does not count. */
static int Jump(const IpcLabel *l, const float *a, const float *b)
{
    double v[16], p[16], m[16], xa, ya, wa, xb, yb, wb, dx, dy;
    if (!(sHoldPx > 0.0))
        return 0;
    ToD(l->cam, v);
    ToD(l->vp, p);   /* the clip matrix's projection: x/w and y/w are in -1..1 on the screen (the draw's own projection already holds the scale to the GS's pixels) */
    Mul4(v, p, m);
    wa = (double)a[12] * m[3] + (double)a[13] * m[7] + (double)a[14] * m[11] + m[15];
    wb = (double)b[12] * m[3] + (double)b[13] * m[7] + (double)b[14] * m[11] + m[15];
    if (!(wa > 1e-6) || !(wb > 1e-6))
        return 0;
    xa = ((double)a[12] * m[0] + (double)a[13] * m[4] + (double)a[14] * m[8] + m[12]) / wa * 320.0;
    ya = ((double)a[12] * m[1] + (double)a[13] * m[5] + (double)a[14] * m[9] + m[13]) / wa * 224.0;
    xb = ((double)b[12] * m[0] + (double)b[13] * m[4] + (double)b[14] * m[8] + m[12]) / wb * 320.0;
    yb = ((double)b[12] * m[1] + (double)b[13] * m[5] + (double)b[14] * m[9] + m[13]) / wb * 224.0;
    dx = xb - xa;
    dy = yb - ya;
    if (dx * dx + dy * dy > sHoldPx * sHoldPx) {
        if (sLog == 4 && sN.hold_px < 40) {
            char ln[300];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass hold h ");
            Num(ln, &n2, l->handle);
            Str(ln, &n2, " a ");
            Num(ln, &n2, (u64)(long long)(a[12] * 100.0f));
            Str(ln, &n2, " ");
            Num(ln, &n2, (u64)(long long)(a[13] * 100.0f));
            Str(ln, &n2, " ");
            Num(ln, &n2, (u64)(long long)(a[14] * 100.0f));
            Str(ln, &n2, " b ");
            Num(ln, &n2, (u64)(long long)(b[12] * 100.0f));
            Str(ln, &n2, " ");
            Num(ln, &n2, (u64)(long long)(b[13] * 100.0f));
            Str(ln, &n2, " ");
            Num(ln, &n2, (u64)(long long)(b[14] * 100.0f));
            Str(ln, &n2, " w*100 ");
            Num(ln, &n2, (u64)(long long)(wa * 100.0));
            Str(ln, &n2, " px*10 ");
            Num(ln, &n2, (u64)(Sqrt(dx * dx + dy * dy) * 10.0));
            Str(ln, &n2, " m0*1000 ");
            Num(ln, &n2, (u64)(long long)(m[0] * 1000.0));
            ln[n2] = 0;
            Host_Log(ln);
        }
        sN.hold_px++;
        return 1;
    }
    return 0;
}

/* Entries agree when they are within 1e-4 of the matrix's largest entry, or within 1e-5 of the sum of the absolute values of the products that make the entry
 * (ab: the same chain with every factor made positive): a model that stands near the eye, in a world whose coordinates are thousands of units, gets its place's
 * entries from terms that cancel, and the game's float chain rounds them by more than the first rule allows. */
static int Near(const float *f, const double *d, const double *ab, double big)
{
    int i;
    for (i = 0; i < 16; i++) {
        double x = (double)f[i] - d[i], tol = 1e-4 * big + 1e-6;
        if (x < 0)
            x = -x;
        if (ab && 1e-5 * ab[i] > tol)
            tol = 1e-5 * ab[i];
        if (!(x <= tol))
            return 0;
    }
    return 1;
}

static void AbsOf(const double *a, double *o)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = a[i] < 0 ? -a[i] : a[i];
}

/* The sums of absolute products behind DrawMatrices' two matrices. */
static void DrawAbs(const IpcLabel *l, const double *view, int form, double *an, double *ac)
{
    double m[16], p[16], vp[16], v[16], mv[16];
    int i;
    ToD(l->world, mv);
    AbsOf(mv, m);
    ToD(l->cam + 16, mv);
    AbsOf(mv, p);
    ToD(l->vp, mv);
    AbsOf(mv, vp);
    AbsOf(view, v);
    if (form == 1)
        Mul4(m, v, mv);
    else
        for (i = 0; i < 16; i++)
            mv[i] = v[i];
    Mul4(mv, p, an);
    Mul4(mv, vp, ac);
}

/* The draw's model-to-screen matrix in the copy: found by value (form 1: model x view x projection; form 2: view x projection) among the first six qwords of the
 * draw's data, with its clip-test matrix four qwords after it checked the same way. Returns the matrix's floats in the copy, or 0. */
static float *FindMatrixIn(u32 draw, int *formOut, u32 lo, u32 hi)
{
    double view[16], n[16], c[16], an[16], ac[16], wm[16];
    u32 off = 0, i;
    int form;
    ToD(sDraws[draw].cam, view);
    for (form = 1; form <= 2; form++) {
        double big = 0, bigc = 0;
        ToD(sDraws[draw].world, wm);
        DrawMatrices(&sDraws[draw], wm, view, form, n, c);
        DrawAbs(&sDraws[draw], view, form, an, ac);
        for (i = 0; i < 16; i++) {
            double a = n[i] < 0 ? -n[i] : n[i], b = c[i] < 0 ? -c[i] : c[i];
            big = a > big ? a : big;
            bigc = b > bigc ? b : bigc;
        }
        for (off = lo; off < hi;) {
            PEv *e = (PEv *)(sEv + off);
            u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
            if (e->kind == 1 && e->draw == draw && e->n >= 9) {
                float *f = (float *)(e + 1);
                u32 q;
                for (q = 0; q + 8 <= e->n && q < 6; q++)
                    if (Near(f + q * 4, n, an, big) && Near(f + q * 4 + 16, c, ac, bigc)) {
                        *formOut = form;
                        return f + q * 4;
                    }
            }
            off += 16 + bytes;
        }
    }
    return 0;
}

/* MH2_PASS_DRAWIDX: where each draw's pieces lie in the window's copy, in one walk of it: the offset of the header of the draw's first piece and the offset past its last (0 and 0 when
 * it has none). A walk for one draw's data then visits that span alone: outside it no header is the draw's, so the walk finds what it found over the whole copy. No moment changes a
 * header, so the index holds for every moment of the window and for a helper's copy of it. The control (MH2_PASS_DRAWCTL=1): every fourth draw's span begins past its first piece. */
static void IndexDraws(void)
{
    u32 d, off;
    for (d = 0; d < sNDraws; d++)
        sDLo[d] = sDHi[d] = 0;
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw < sNDraws) {
            if (!sDHi[e->draw])
                sDLo[e->draw] = sDCtl == 1 && (e->draw & 3) == 1 ? off + 16 + bytes : off;
            sDHi[e->draw] = off + 16 + bytes;
        }
        off += 16 + bytes;
    }
}

static float *FindMatrix(u32 draw, int *formOut)
{
    if (sDIx == 1) {
        sDx[0]++;
        return FindMatrixIn(draw, formOut, sDLo[draw], sDHi[draw]);
    }
    if (sDIx == 2) {   /* the compare: both ways, from the same form as it stood; the whole copy's answer is used */
        int f2 = *formOut;
        float *a = FindMatrixIn(draw, formOut, 0, sEvLen), *b = FindMatrixIn(draw, &f2, sDLo[draw], sDHi[draw]);
        sDx[1]++;
        if (a != b || f2 != *formOut)
            sDx[2]++;
        return a;
    }
    return FindMatrixIn(draw, formOut, 0, sEvLen);
}

/* The first max qwords of a draw's data in the copy between two offsets of it: their addresses into qp. Returns how many. */
static u32 GatherIn(u32 draw, float **qp, u32 max, u32 lo, u32 hi)
{
    u32 nq = 0, off;
    for (off = lo; off < hi && nq < max;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw == draw) {
            u32 i;
            for (i = 0; i < e->n && nq < max; i++)
                qp[nq++] = (float *)((u8 *)(e + 1) + i * 16);
        }
        off += 16 + bytes;
    }
    return nq;
}

/* ... over the whole copy, or (MH2_PASS_DRAWIDX) over the draw's span of it. max is at most VQW. */
static u32 Gather(u32 draw, float **qp, u32 max)
{
    if (sDIx == 1) {
        sDx[0]++;
        return GatherIn(draw, qp, max, sDLo[draw], sDHi[draw]);
    }
    if (sDIx == 2) {
        float *q2[VQW];
        u32 a = GatherIn(draw, qp, max, 0, sEvLen), b = GatherIn(draw, q2, max, sDLo[draw], sDHi[draw]);
        sDx[1]++;
        if (a != b || memcmp(qp, q2, a * sizeof(float *)))
            sDx[2]++;
        return a;
    }
    return GatherIn(draw, qp, max, 0, sEvLen);
}

/* ---- the camera of the in-between moment ---- */


/* The eye and the rotation (the rows: where the world's axes go) of a rigid view matrix; 0 when it is not one. */
static int ViewSplit(const double *v, double *eye, double *r)
{
    int i, j, k;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r[i * 3 + j] = v[i * 4 + j];
    for (i = 0; i < 3; i++)
        for (k = i; k < 3; k++) {
            double d = r[i * 3] * r[k * 3] + r[i * 3 + 1] * r[k * 3 + 1] + r[i * 3 + 2] * r[k * 3 + 2] - (i == k ? 1.0 : 0.0);
            if (d > 1e-3 || d < -1e-3)
                return 0;
        }
    if (v[3] > 1e-6 || v[3] < -1e-6 || v[7] > 1e-6 || v[7] < -1e-6 || v[11] > 1e-6 || v[11] < -1e-6 || v[15] > 1.000001 || v[15] < 0.999999)
        return 0;
    for (j = 0; j < 3; j++)
        eye[j] = -(v[12] * r[j * 3] + v[13] * r[j * 3 + 1] + v[14] * r[j * 3 + 2]);
    return 1;
}

static void RotQuat(const double *r, double *q)
{
#define RM(i, j) r[(j) * 3 + (i)]
    double tr = r[0] + r[4] + r[8], s;
    if (tr > 0) {
        s = Sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s;
        q[0] = (RM(2, 1) - RM(1, 2)) / s;
        q[1] = (RM(0, 2) - RM(2, 0)) / s;
        q[2] = (RM(1, 0) - RM(0, 1)) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        s = Sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
        q[3] = (RM(2, 1) - RM(1, 2)) / s;
        q[0] = 0.25 * s;
        q[1] = (RM(0, 1) + RM(1, 0)) / s;
        q[2] = (RM(0, 2) + RM(2, 0)) / s;
    } else if (r[4] > r[8]) {
        s = Sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
        q[3] = (RM(0, 2) - RM(2, 0)) / s;
        q[0] = (RM(0, 1) + RM(1, 0)) / s;
        q[1] = 0.25 * s;
        q[2] = (RM(1, 2) + RM(2, 1)) / s;
    } else {
        s = Sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
        q[3] = (RM(1, 0) - RM(0, 1)) / s;
        q[0] = (RM(0, 2) + RM(2, 0)) / s;
        q[1] = (RM(1, 2) + RM(2, 1)) / s;
        q[2] = 0.25 * s;
    }
#undef RM
}

static void QuatRot(const double *q, double *r)
{
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                   2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                   2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r[j * 3 + i] = m[i * 3 + j];
}

/* The view t of the way from a to b (the renderer's rule: the eye in a line, the rotation by a mean of the quaternions made unit). 0 when it is not a smooth
 * move of a rigid view: a camera cut (the eye more than 100 units, or the rotation more than about 16 degrees: the quaternions' dot below 0.99). */
static int ViewBlend(const float *fa, const float *fb, double t, double *o)
{
    double a[16], b[16], ea[3], eb[3], ra[9], rb[9], qa[4], qb[4], q[4], r[9], eye[3], d, n = 0, dist = 0;
    int i, j;
    ToD(fa, a);
    ToD(fb, b);
    if (!ViewSplit(a, ea, ra) || !ViewSplit(b, eb, rb))
        return 0;
    RotQuat(ra, qa);
    RotQuat(rb, qb);
    d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) {
        for (i = 0; i < 4; i++)
            qb[i] = -qb[i];
        d = -d;
    }
    for (i = 0; i < 3; i++)
        dist += (ea[i] - eb[i]) * (ea[i] - eb[i]);
    if (d < 0.99 || dist > 100.0 * 100.0)
        return 0;
    for (i = 0; i < 4; i++) {
        q[i] = (1 - t) * qa[i] + t * qb[i];
        n += q[i] * q[i];
    }
    n = 1.0 / Sqrt(n);
    for (i = 0; i < 4; i++)
        q[i] *= n;
    QuatRot(q, r);
    for (j = 0; j < 3; j++)
        eye[j] = ea[j] * (1 - t) + eb[j] * t;
    for (i = 0; i < 16; i++)
        o[i] = 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            o[i * 4 + j] = r[i * 3 + j];
    o[15] = 1;
    for (j = 0; j < 3; j++)
        o[12 + j] = -(eye[0] * r[j] + eye[1] * r[3 + j] + eye[2] * r[6 + j]);
    return 1;
}

/* ---- the models' matrices ---- */

/* A model's matrix in the game's row-vector layout: the rows are the images of the axes (a scale and a rotation), the last row the place. Blended as the
 * renderer blends a camera: the place by a mean, the rotation by the mean of the quaternions made unit, the scale by a mean. Returns 0 (o is the blend),
 * 1 (a cut: the place moved more than sMoveMax, the rotation more than about 120 degrees, or not a matrix this blends and too different to average), or
 * 2 (the two are the same bits, or the moment is the step itself: o is b). */
#define MOVE_MAX 300.0
/* What MatBlend decides from the two matrices alone, and what a moment's alpha does with it. MatPrep makes the decision once (a cut, the same bits, or the two matrices taken apart: their scale,
 * their rotation as a unit quaternion on the same side, their place); MatApply makes the blend at an alpha from that. MatBlend is the two in a row, and gives what it always gave. */
typedef struct MatP {
    int kind;                    /* 1 a cut, 2 the same bits, 3 a scale, a rotation and a place, 4 the entries' mean */
    int site;                    /* the call it was made for: 1 a model's matrix, 2 a bone, 3 a matrix of the data that is not a slot's */
    double a[16], b[16], sa[3], sb[3], qa[4], qb[4];
    float fa[16], fb[16];        /* the two ends, as the game made them */
} MatP;

static int MatPrep(const float *fa, const float *fb, MatP *p)
{
    double *a = p->a, *b = p->b, *sa = p->sa, *sb = p->sb, *qa = p->qa, *qb = p->qb, ra[9], rb[9], d, dist = 0;
    int i, j, rigid = 1;
    memcpy(p->fa, fa, 64);
    memcpy(p->fb, fb, 64);
    p->kind = 2;
    if (!memcmp(fa, fb, 64))
        return 2;
    ToD(fa, a);
    ToD(fb, b);
    p->kind = 1;
    for (i = 0; i < 3; i++)
        dist += (a[12 + i] - b[12 + i]) * (a[12 + i] - b[12 + i]);
    if (dist > MOVE_MAX * MOVE_MAX * sGate * sGate)
        return 1;
    for (i = 0; i < 3; i++) {
        sa[i] = Sqrt(a[i * 4] * a[i * 4] + a[i * 4 + 1] * a[i * 4 + 1] + a[i * 4 + 2] * a[i * 4 + 2]);
        sb[i] = Sqrt(b[i * 4] * b[i * 4] + b[i * 4 + 1] * b[i * 4 + 1] + b[i * 4 + 2] * b[i * 4 + 2]);
        if (!(sa[i] > 1e-6) || !(sb[i] > 1e-6))
            rigid = 0;
    }
    if (rigid) {
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++) {
                ra[i * 3 + j] = a[i * 4 + j] / sa[i];
                rb[i * 3 + j] = b[i * 4 + j] / sb[i];
            }
        for (i = 0; i < 3 && rigid; i++)
            for (j = i + 1; j < 3; j++) {
                double da = ra[i * 3] * ra[j * 3] + ra[i * 3 + 1] * ra[j * 3 + 1] + ra[i * 3 + 2] * ra[j * 3 + 2];
                double db = rb[i * 3] * rb[j * 3] + rb[i * 3 + 1] * rb[j * 3 + 1] + rb[i * 3 + 2] * rb[j * 3 + 2];
                if (da > 1e-3 || da < -1e-3 || db > 1e-3 || db < -1e-3)
                    rigid = 0;
            }
        if (rigid) {   /* right-handed */
            double ha = (ra[0] * (ra[4] * ra[8] - ra[5] * ra[7]) - ra[1] * (ra[3] * ra[8] - ra[5] * ra[6]) + ra[2] * (ra[3] * ra[7] - ra[4] * ra[6]));
            double hb = (rb[0] * (rb[4] * rb[8] - rb[5] * rb[7]) - rb[1] * (rb[3] * rb[8] - rb[5] * rb[6]) + rb[2] * (rb[3] * rb[7] - rb[4] * rb[6]));
            if (!(ha > 0) || !(hb > 0))
                rigid = 0;
        }
    }
    for (i = 3; i < 16; i += 4)
        if (a[i] != b[i]) {
            rigid = 0;
            break;
        }
    if (sLerp)
        rigid = 0;
    if (!rigid) {   /* not a scale, a rotation and a place: the entries' mean, when they are close */
        double big = 0;
        for (i = 0; i < 12; i++) {
            double x = a[i] - b[i];
            x = x < 0 ? -x : x;
            big = x > big ? x : big;
        }
        if (big > 0.5 * sGate && !sLerp)
            return 1;
        p->kind = 4;
        return 4;
    }
    RotQuat(ra, qa);
    RotQuat(rb, qb);
    d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) {
        for (i = 0; i < 4; i++)
            qb[i] = -qb[i];
        d = -d;
    }
    if (d < (sGate > 1.0 ? 0.0 : 0.5))
        return 1;
    p->kind = 3;
    return 3;
}

static int MatApply(const MatP *p, double t, float *o)
{
    double q[4], r[9], n;
    int i, j;
    if (p->kind == 1)
        return 1;
    if (p->kind == 2) {
        memcpy(o, p->fb, 64);
        return 2;
    }
    if (p->kind == 4) {
        for (i = 0; i < 16; i++)
            o[i] = (float)(p->a[i] * (1 - t) + p->b[i] * t);
        if (t >= 1.0) {
            memcpy(o, p->fb, 64);
            return 2;
        }
        return 0;
    }
    if (t >= 1.0) {
        memcpy(o, p->fb, 64);
        return 2;
    }
    for (i = 0, n = 0; i < 4; i++) {
        q[i] = (1 - t) * p->qa[i] + t * p->qb[i];
        n += q[i] * q[i];
    }
    n = 1.0 / Sqrt(n);
    for (i = 0; i < 4; i++)
        q[i] *= n;
    QuatRot(q, r);
    for (i = 0; i < 16; i++)
        o[i] = 0;
    for (i = 0; i < 3; i++) {
        double s = p->sa[i] * (1 - t) + p->sb[i] * t;
        for (j = 0; j < 3; j++)
            o[i * 4 + j] = (float)(s * r[i * 3 + j]);
        o[12 + i] = (float)(p->a[12 + i] * (1 - t) + p->b[12 + i] * t);
    }
    o[15] = 1.0f;
    return 0;
}

static int MatBlend(const float *fa, const float *fb, double t, float *o)
{
    MatP p;
    MatPrep(fa, fb, &p);
    return MatApply(&p, t, o);
}

/* The list of the window's decisions (step 4; development: MH2_PASS_TAPE=0 leaves it out, as it was): with several moments a step the first moment makes each matrix blend's decision
 * (MatPrep) and keeps it, with the two ends, in the order Models asks for them; the moments after the first take the next entry, when its two ends are the ones asked for, and
 * make only the blend at their alpha. An entry whose ends are not the ones asked for is not used (counted): the blend is made whole. MH2_PASS_TAPECTL=N changes the Nth entry that
 * blends (in the whole run), after the first moment has used it: the moments after it see a change, the first moment and every other entry do not. */
#define TAPE_MAX 16384u
static MatP *sTape;

static void TapeInit(void)
{
    sTapeOn = sMode >= 3 && sNMom > 1 && !sThread && !(PASS_ENV("MH2_PASS_TAPE") && PASS_ENV("MH2_PASS_TAPE")[0] == '0');
    if (sTapeOn) {
        const char *c = PASS_ENV("MH2_PASS_TAPECTL");
        sTape = Host_Alloc(TAPE_MAX * sizeof *sTape);
        if (!sTape)
            sTapeOn = 0;
        sTapeCtl = c ? (u64)(c[0] - '0') : 0;
        while (c && c[0] && c[1] >= '0' && c[1] <= '9') {
            sTapeCtl = sTapeCtl * 10 + (u64)(c[1] - '0');
            c++;
        }
    }
}

static int MatBlendT(const float *fa, const float *fb, double t, float *o, int site)
{
    MatP *p;
    int r;
    if (!sTapeMode)
        return MatBlend(fa, fb, t, o);
    if (sTapeMode == 1) {
        if (sTapeN >= TAPE_MAX) {
            sTapeOver++;
            return MatBlend(fa, fb, t, o);
        }
        p = &sTape[sTapeN++];
        p->site = site;
        sTapeRec++;
        MatPrep(fa, fb, p);
        r = MatApply(p, t, o);
        if (p->kind >= 3) {
            sTapeBlend++;
            if (sTapeCtl && sTapeBlend == sTapeCtl && !sTapeCtlDone) {   /* the control: this entry, as the moments after the first will see it */
                sTapeCtlDone = 1;
                if (p->kind == 3)
                    p->sb[0] = p->sb[0] * 1.5 + 0.25;
                else
                    p->b[12] += 3.0;
            }
        }
        return r;
    }
    if (sTapeI < sTapeN && sTape[sTapeI].site == site && !memcmp(sTape[sTapeI].fa, fa, 64) && !memcmp(sTape[sTapeI].fb, fb, 64)) {
        sTapeHit++;
        return MatApply(&sTape[sTapeI++], t, o);
    }
    sTapeMiss++;
    return MatBlend(fa, fb, t, o);
}

/* The bones of a draw in the copy: the matrix slots of the model (the label's table, slots 1 to 31) found by value among the first VQW qwords of the draw's
 * data, which the builders copy whole (Vu1_CopyMatrixSlots: slot k at a fixed qword plus 4 k). The draw's own base is the one most slots agree on. Fills pos
 * (the qword of slot k's first row, or -1) and returns how many were found; qp holds the qwords' addresses (a piece of the DMA ends where it ends: a matrix
 * can straddle two). */
static int sNm, sNq;
static int FindBones(u32 draw, const IpcLabel *l, int *pos, float **qp)
{
    u32 nq, k, q;
    u8 votes[VQW + 140];
    int base = -1, best = 0, found = 0;
    nq = Gather(draw, qp, VQW);
    sNq = (int)nq;
    for (k = 0; k < 32; k++)
        pos[k] = -1;
    if (nq < 20)
        return 0;
    memset(votes, 0, sizeof(votes));
    for (k = 1; k < 32; k++) {
        const float *s = l->slots + k * 16;
        int zero = 1, i;
        for (i = 0; i < 16; i++)
            if (s[i] != 0.0f)
                zero = 0;
        if (zero)
            continue;
        for (q = 8; q + 4 <= nq; q++)
            if (q >= 4 * k && !memcmp(qp[q], s, 16) && !memcmp(qp[q + 1], s + 4, 16) && !memcmp(qp[q + 2], s + 8, 16) && !memcmp(qp[q + 3], s + 12, 16)) {
                u32 b = q - 4 * k;
                if (b < VQW && votes[b] < 255) {
                    votes[b]++;
                    if (votes[b] > best) {
                        best = votes[b];
                        base = (int)b;
                    }
                }
            }
    }
    if (base < 0)
        return 0;
    for (k = 1; k < 32; k++) {
        const float *s = l->slots + k * 16;
        u32 p = (u32)base + 4 * k;
        if (p + 4 <= nq && !memcmp(qp[p], s, 16) && !memcmp(qp[p + 1], s + 4, 16) && !memcmp(qp[p + 2], s + 8, 16) && !memcmp(qp[p + 3], s + 12, 16)) {
            int zero = 1, i;
            for (i = 0; i < 16; i++)
                if (s[i] != 0.0f)
                    zero = 0;
            if (!zero) {
                pos[k] = (int)p;
                found++;
            }
        }
    }
    sNm = 0;
    if (sLog == 3)   /* the matrices in the slot region of the data that look like a bone (the w column 0 0 0 1) against the ones found equal to a slot */
        for (k = 1; k < 32; k++) {
            u32 p = (u32)base + 4 * k;
            const float *r0, *r1, *r2, *r3;
            if (p + 4 > nq)
                break;
            r0 = qp[p], r1 = qp[p + 1], r2 = qp[p + 2], r3 = qp[p + 3];
            if (r0[3] == 0.0f && r1[3] == 0.0f && r2[3] == 0.0f && r3[3] == 1.0f && (r0[0] != 0.0f || r0[1] != 0.0f || r0[2] != 0.0f))
                sNm++;
        }
    return found;
}

/* (diagnostics, MH2_PASS_LOG=3) a skinned draw whose bones the search did not find: where each non-zero slot of the label's table is in the draw's data (the qword, or -1),
 * the first three draws of each handle. */
static void DiagSkin(u32 d, const IpcLabel *l)
{
    static u32 seenh[16], nseen[16];
    float *qp[1200];
    u32 nq = 0, off, k, q, bl = 0, hh = l->handle & 15, pieces = 0;
    char big[1400];
    if (nseen[hh] >= 3 && seenh[hh] == l->handle)
        return;
    if (seenh[hh] != l->handle)
        seenh[hh] = l->handle, nseen[hh] = 0;
    nseen[hh]++;
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw == d) {
            u32 i;
            pieces++;
            for (i = 0; i < e->n && nq < 1200; i++)
                qp[nq++] = (float *)((u8 *)(e + 1) + i * 16);
        }
        off += 16 + bytes;
    }
    if (nseen[hh] == 1) {   /* the first draw of the handle: its data as words, and the label's matrices */
        u32 w;
        for (q = 0; q < nq && q < 120; q++) {
            char ln[200];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass skin-words h ");
            Num(ln, &n2, l->handle);
            Str(ln, &n2, " q ");
            Num(ln, &n2, q);
            for (w = 0; w < 4; w++) {
                u32 u;
                int sh;
                memcpy(&u, (u8 *)qp[q] + 4 * w, 4);
                ln[n2++] = ' ';
                for (sh = 28; sh >= 0; sh -= 4)
                    ln[n2++] = "0123456789abcdef"[(u >> sh) & 15];
            }
            ln[n2] = 0;
            Host_Log(ln);
        }
        for (k = 0; k < 32; k++) {
            char ln[400];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass skin-slot h ");
            Num(ln, &n2, l->handle);
            Str(ln, &n2, " k ");
            Num(ln, &n2, k);
            for (w = 0; w < 16; w++) {
                u32 u;
                int sh;
                memcpy(&u, l->slots + k * 16 + w, 4);
                ln[n2++] = ' ';
                for (sh = 28; sh >= 0; sh -= 4)
                    ln[n2++] = "0123456789abcdef"[(u >> sh) & 15];
            }
            ln[n2] = 0;
            Host_Log(ln);
        }
    }
    Str(big, &bl, "mh2-core: pass skin-diag h ");
    Num(big, &bl, l->handle);
    Str(big, &bl, " prog ");
    Num(big, &bl, l->prog);
    Str(big, &bl, " flags ");
    Num(big, &bl, l->flags);
    Str(big, &bl, " qwords ");
    Num(big, &bl, nq);
    Str(big, &bl, " pieces ");
    Num(big, &bl, pieces);
    Str(big, &bl, " slots");
    for (k = 1; k < 32; k++) {
        const float *sl = l->slots + k * 16;
        int zero = 1, i, at = -1;
        for (i = 0; i < 16; i++)
            if (sl[i] != 0.0f)
                zero = 0;
        if (zero)
            continue;
        for (q = 0; q + 4 <= nq && at < 0; q++)
            if (!memcmp(qp[q], sl, 16) && !memcmp(qp[q + 1], sl + 4, 16) && !memcmp(qp[q + 2], sl + 8, 16) && !memcmp(qp[q + 3], sl + 12, 16))
                at = (int)q;
        big[bl++] = ' ';
        Num(big, &bl, k);
        big[bl++] = ':';
        if (at < 0)
            big[bl++] = '-';
        Num(big, &bl, at < 0 ? 1 : (u64)at);
    }
    big[bl] = 0;
    Host_Log(big);
}

/* The matrices in the first qwords of a skinned draw's data that are not copies of the label's slots but look like a bone's (the rows of a rotation and a scale with w 0, a place
 * with w 1 and not near the origin): a builder that packs the bones of a part as the game moved them (the weapon's parts) puts matrices here that the label's table does not hold.
 * They are paired with the same draw's of the step before by their qword, as the slots are by their number. Returns how many; gpos has their qwords. */
#define GMAX 48
static int FindDataMats(const int *pos, float **qp, int *gpos)
{
    int n = 0, p, k, nq = sNq;
    for (p = 3; p + 4 <= nq && n < GMAX; p++) {
        const float *r[4];
        int i, skip = 0;
        double nr[3];
        for (k = 1; k < 32; k++)
            if (pos[k] >= 0 && p > pos[k] - 4 && p < pos[k] + 4)
                skip = 1;
        if (skip)
            continue;
        for (i = 0; i < 4; i++)
            r[i] = qp[p + i];
        if (!(r[0][3] == 0.0f && r[1][3] == 0.0f && r[2][3] == 0.0f && r[3][3] == 1.0f))
            continue;
        for (i = 0; i < 3; i++) {
            nr[i] = (double)r[i][0] * r[i][0] + (double)r[i][1] * r[i][1] + (double)r[i][2] * r[i][2];
            if (!(nr[i] > 0.09 && nr[i] < 9.0))
                skip = 1;
        }
        if (skip)
            continue;
        for (i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            double d = (double)r[i][0] * r[j][0] + (double)r[i][1] * r[j][1] + (double)r[i][2] * r[j][2];
            if (d > 0.05 * Sqrt(nr[i] * nr[j]) || d < -0.05 * Sqrt(nr[i] * nr[j]))
                skip = 1;
        }
        if (skip)
            continue;
        if (!((double)r[3][0] * r[3][0] + (double)r[3][1] * r[3][1] + (double)r[3][2] * r[3][2] > 100.0 * 100.0))
            continue;
        gpos[n++] = p;
        p += 3;
    }
    return n;
}

static void PutMat(float **qp, int p, const float *m)
{
    int i;
    for (i = 0; i < 4; i++)
        memcpy(qp[p + i], m + 4 * i, 16);
}

/* A hash of each draw's data after the matrices (what the builders pack from the model's vertices and the game's per-step data, not from the camera): one pass
 * over the copy. Two steps' draws that have the same matrices and a different hash are drawn from vertices the game moved itself (dust, a shadow, a trail): the
 * matrices cannot move them. */
static void HashDraws(void)
{
    u32 cnt[MAX_DRAWS], d, off;
    memset(cnt, 0, sizeof(u32) * sNDraws);
    for (d = 0; d < sNDraws; d++)
        sDH[d] = 1469598103934665603ull;
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw < sNDraws) {
            u32 i, skip = sForm[e->draw] == 2 ? 160 : 24;
            const u64 *q = (const u64 *)(e + 1);
            for (i = 0; i < e->n; i++, cnt[e->draw]++)
                if (cnt[e->draw] >= skip) {
                    sDH[e->draw] = (sDH[e->draw] ^ q[2 * i]) * 1099511628211ull;
                    sDH[e->draw] = (sDH[e->draw] ^ q[2 * i + 1]) * 1099511628211ull;
                }
        }
        off += 16 + bytes;
    }
}

/* The draws' hashes for the Models in hand. MH2_PASS_HASHONCE: a moment after the first hashes nothing: the copy it starts from is the kept image the first moment hashed and the forms
 * are the finding's, so its hashes are the first moment's (sDH1; a helper's are the window's share's). Not under the control that leaves the copy as the moment before left it. */
static void HashNow(void)
{
    if (sDHo && sLaterMo && !sMomCtl) {
        const u64 *src = sDCtl == 2 && !sShared ? sDHOld : sDH1;
        u32 d;
        if (sDHo == 2) {   /* the compare: the copy hashed again; the new hashes are used */
            HashDraws();
            sDx[4] += sNDraws;
            for (d = 0; d < sNDraws; d++)
                if (sDH[d] != src[d])
                    sDx[5]++;
            return;
        }
        memcpy(sDH, src, sNDraws * sizeof(u64));
        sDx[3] += sNDraws;
        return;
    }
    if (sDHo && !sLaterMo && sDCtl == 2)
        memcpy(sDHOld, sDH1, sNDraws * sizeof(u64));
    HashDraws();
    if (sDHo && !sLaterMo)
        memcpy(sDH1, sDH, sNDraws * sizeof(u64));
}

/* (diagnostics, MH2_PASS_LOG=2) a draw whose matrix the search did not find: the first qwords of its data and where its model-to-screen matrix is, if it is anywhere
 * in the first 400 qwords (any alignment of the row, either form, to 1e-3). */
static void DiagUnfound(u32 d)
{
    float *qp[400];
    u32 nq = 0, off, q, k;
    double view[16], n[16], c[16], wm[16];
    int form, at[3] = {-1, -1, -1};
    char line[400];
    u32 len = 0, pieces = 0, firstn = 0;
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw == d) {
            u32 i;
            if (!pieces)
                firstn = e->n;
            pieces++;
            for (i = 0; i < e->n && nq < 400; i++)
                qp[nq++] = (float *)((u8 *)(e + 1) + i * 16);
        }
        off += 16 + bytes;
    }
    ToD(sDraws[d].cam, view);
    for (form = 1; form <= 2; form++) {
        double big = 0;
        ToD(sDraws[d].world, wm);
        DrawMatrices(&sDraws[d], wm, view, form, n, c);
        for (k = 0; k < 16; k++) {
            double a = n[k] < 0 ? -n[k] : n[k];
            big = a > big ? a : big;
        }
        for (q = 0; q + 4 <= nq && at[form - 1] < 0; q++) {
            float m[16];
            memcpy(m, qp[q], 16);
            memcpy(m + 4, qp[q + 1], 16);
            memcpy(m + 8, qp[q + 2], 16);
            memcpy(m + 12, qp[q + 3], 16);
            if (Near(m, n, 0, big * 10.0))
                at[form - 1] = (int)q;
        }
    }
    if (nq >= 12) {   /* (the first draws of a handle: the words of the matrices) */
        static u32 seen[8], nseen[8];
        u32 hh = sDraws[d].handle & 7, w;
        if (nseen[hh] < 3 && seen[hh] != sDraws[d].handle + 0x80000000u) {
            static char big[1400];
            u32 bl = 0, part;
            const float *src[5];
            src[0] = sDraws[d].world;
            src[1] = sDraws[d].cam;
            src[2] = sDraws[d].cam + 16;
            src[3] = sDraws[d].vp;
            Str(big, &bl, "mh2-core: pass unfound-words h ");
            Num(big, &bl, sDraws[d].handle);
            for (part = 0; part < 6; part++) {
                Str(big, &bl, part == 0 ? " W" : part == 1 ? " V" : part == 2 ? " P" : part == 3 ? " VP" : part == 4 ? " N" : " C");
                for (w = 0; w < 16; w++) {
                    float v = part < 4 ? src[part][w] : qp[3 + (part - 4) * 4 + w / 4][w % 4];
                    u32 u;
                    int sh;
                    memcpy(&u, &v, 4);
                    big[bl++] = ' ';
                    for (sh = 28; sh >= 0; sh -= 4)
                        big[bl++] = "0123456789abcdef"[(u >> sh) & 15];
                }
            }
            big[bl] = 0;
            Host_Log(big);
            nseen[hh]++;
        }
        (void)seen;
    }
    if (nq >= 12) {   /* the matrix at qword 3, form 1: its largest entry, and the error of each entry relative to it, in 1e-6 */
        double big = 0, err = 0;
        int worst = 0;
        float m[16];
        ToD(sDraws[d].world, wm);
        DrawMatrices(&sDraws[d], wm, view, 1, n, c);
        for (k = 0; k < 4; k++)
            memcpy(m + 4 * k, qp[3 + k], 16);
        for (k = 0; k < 16; k++) {
            double a = n[k] < 0 ? -n[k] : n[k], e = (double)m[k] - n[k];
            e = e < 0 ? -e : e;
            big = a > big ? a : big;
            if (e > err) {
                err = e;
                worst = (int)k;
            }
        }
        for (k = 0; k < 4; k++)
            memcpy(m + 4 * k, qp[7 + k], 16);
        big = err = 0;
        for (k = 0; k < 16; k++) {
            double a = c[k] < 0 ? -c[k] : c[k], e = (double)m[k] - c[k];
            e = e < 0 ? -e : e;
            big = a > big ? a : big;
            if (e > err) {
                err = e;
                worst = (int)k;
            }
        }
        Str(line, &len, "mh2-core: pass unfound-err CLIP big ");
        Num(line, &len, (u64)big);
        Str(line, &len, " worst entry ");
        Num(line, &len, (u64)worst);
        Str(line, &len, " err-ppm ");
        Num(line, &len, (u64)(err / big * 1e6));
        Str(line, &len, " got ");
        Num(line, &len, (u64)(m[worst] < 0 ? -m[worst] : m[worst]));
        Str(line, &len, " want ");
        Num(line, &len, (u64)(n[worst] < 0 ? -n[worst] : n[worst]));
        line[len] = 0;
        Host_Log(line);
        len = 0;
    }
    Str(line, &len, "mh2-core: pass unfound d ");
    Num(line, &len, d);
    Str(line, &len, " h ");
    Num(line, &len, sDraws[d].handle);
    Str(line, &len, " prog ");
    Num(line, &len, sDraws[d].prog);
    Str(line, &len, " flags ");
    Num(line, &len, sDraws[d].flags);
    Str(line, &len, " site ");
    Num(line, &len, sDraws[d].site);
    Str(line, &len, " pieces ");
    Num(line, &len, pieces);
    Str(line, &len, " first ");
    Num(line, &len, firstn);
    Str(line, &len, " qwords ");
    Num(line, &len, nq);
    Str(line, &len, " at1 ");
    if (at[0] < 0)
        Str(line, &len, "none");
    else
        Num(line, &len, (u64)at[0]);
    Str(line, &len, " at2 ");
    if (at[1] < 0)
        Str(line, &len, "none");
    else
        Num(line, &len, (u64)at[1]);
    line[len] = 0;
    Host_Log(line);
}

static void LogDraw(u32 d, const PDraw *me, int form, int own_view, int found, int pd_ok, int changed, const double *c)
{
    char line[300];
    u32 n = 0;
    const IpcLabel *l = &sDraws[d];
    s64 sx = 0, sy = 0;
    if (c && c[15] > 1e-9) {
        sx = (s64)(c[12] / c[15]);
        sy = (s64)(c[13] / c[15]);
    }
    Str(line, &n, "mh2-core: pass draw step ");
    Num(line, &n, sStepNo);
    Str(line, &n, " d ");
    Num(line, &n, d);
    Str(line, &n, " h ");
    Num(line, &n, l->handle);
    Str(line, &n, " occ ");
    Num(line, &n, me ? me->occ : 0xFFFFFFFFu);
    Str(line, &n, " flags ");
    Num(line, &n, l->flags);
    Str(line, &n, " prog ");
    Num(line, &n, l->prog);
    Str(line, &n, " site ");
    Num(line, &n, l->site);
    Str(line, &n, " form ");
    Num(line, &n, (u64)form);
    Str(line, &n, " ownview ");
    Num(line, &n, (u64)own_view);
    Str(line, &n, " found ");
    Num(line, &n, (u64)found);
    Str(line, &n, " partner ");
    Num(line, &n, (u64)pd_ok);
    Str(line, &n, " changed ");
    Num(line, &n, (u64)changed);
    Str(line, &n, " sx ");
    if (sx < 0) {
        line[n++] = '-';
        sx = -sx;
    }
    Num(line, &n, (u64)sx);
    Str(line, &n, " sy ");
    if (sy < 0) {
        line[n++] = '-';
        sy = -sy;
    }
    Num(line, &n, (u64)sy);
    line[n] = 0;
    Host_Log(line);
}

static int Inv4(const double *a, double *o)
{
    double m[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            m[i][j] = j < 4 ? a[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (i = 0; i < 4; i++) {
        int piv = i;
        double d;
        for (k = i + 1; k < 4; k++)
            if ((m[k][i] < 0 ? -m[k][i] : m[k][i]) > (m[piv][i] < 0 ? -m[piv][i] : m[piv][i]))
                piv = k;
        if ((m[piv][i] < 0 ? -m[piv][i] : m[piv][i]) < 1e-12)
            return 0;
        if (piv != i)
            for (j = 0; j < 8; j++) {
                double t = m[i][j];
                m[i][j] = m[piv][j];
                m[piv][j] = t;
            }
        d = 1.0 / m[i][i];
        for (j = 0; j < 8; j++)
            m[i][j] *= d;
        for (k = 0; k < 4; k++)
            if (k != i) {
                double f = m[k][i];
                for (j = 0; j < 8; j++)
                    m[k][j] -= f * m[i][j];
            }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            o[i * 4 + j] = m[i][4 + j];
    return 1;
}

/* The VU1 draws that no label names (the pass's copy holds them under draw -1): their model-to-screen matrix N and clip-test matrix C are found by the relation
 * that holds for every draw of the main camera, C = N * P^-1 * VP (N = M V P, C = M V VP), with no label and no model matrix: a pair of matrices four qwords apart in
 * the first qwords of a piece that has it. Each is made again for the in-between camera, N' = N * (P^-1 V^-1 V' P) and C' = C * (VP^-1 V^-1 V' VP): the camera only (the
 * draw has no partner). Returns how many were rewritten. */
static void Unlabelled(const double *vb)
{
    PSet *cur = Cur();
    double view[16], vinv[16], proj[16], pinv[16], vp[16], vpinv[16], t1[16], kn[16], kc[16], rel[16], absrel[16], tmp[16];
    u32 off, i;
    ToD(cur->view, view);
    ToD(cur->proj, proj);
    ToD(cur->vp, vp);
    if (!Inv4(proj, pinv) || !Inv4(vp, vpinv))
        return;
    {   /* the view's inverse: a rigid view is [R 0; t 1] in row-vector form */
        double r[9], e[3];
        int a, b;
        if (!ViewSplit(view, e, r))
            return;
        for (i = 0; i < 16; i++)
            vinv[i] = 0;
        for (a = 0; a < 3; a++)
            for (b = 0; b < 3; b++)
                vinv[a * 4 + b] = r[b * 3 + a];
        vinv[15] = 1;
        for (b = 0; b < 3; b++)
            vinv[12 + b] = e[b];
    }
    Mul4(vinv, vb, t1);
    Mul4(pinv, t1, tmp);
    Mul4(tmp, proj, kn);
    Mul4(vpinv, t1, tmp);
    Mul4(tmp, vp, kc);
    Mul4(pinv, vp, rel);
    AbsOf(rel, absrel);
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        if (e->kind == 1 && e->draw == 0xFFFFFFFFu && e->n >= 12) {
            float *f = (float *)(e + 1);
            u32 q;
            for (q = 0; q + 8 <= e->n && q < 6; q++) {
                double x[16], want[16], ab[16], big = 0, nz = 0;
                for (i = 0; i < 16; i++) {
                    double v = f[q * 4 + i];
                    x[i] = v;
                    nz += v < 0 ? -v : v;
                }
                if (!(nz > 1e-3))
                    continue;
                Mul4(x, rel, want);
                {
                    double ax[16];
                    AbsOf(x, ax);
                    Mul4(ax, absrel, ab);
                }
                for (i = 0; i < 16; i++) {
                    double a = want[i] < 0 ? -want[i] : want[i];
                    big = a > big ? a : big;
                }
                if (Near(f + q * 4 + 16, want, ab, big)) {
                    double nn[16], cc[16], y[16];
                    sN.unl_found++;
                    if (vb) {
                        for (i = 0; i < 16; i++)
                            y[i] = f[q * 4 + 16 + i];
                        Mul4(x, kn, nn);
                        Mul4(y, kc, cc);
                        for (i = 0; i < 16; i++) {
                            f[q * 4 + i] = (float)nn[i];
                            f[q * 4 + 16 + i] = (float)cc[i];
                        }
                        sN.unl_rewritten++;
                    }
                    break;
                }
            }
        }
        off += 16 + bytes;
    }
}

/* Mode 3 and 4: each draw of the window is made as the game would from the in-between moment: the step's main view blended with the step before's (the
 * view of the draws that have it), and the model's matrix (a rigid model's, or each bone's matrix of a skinned one) blended with the same draw's of the step
 * before, paired by handle and occurrence. A draw with no partner (a birth), or with one too far (a cut), keeps the step's own matrix and is counted; a draw
 * whose matrix the search did not find is the step's own whole. A draw whose blended matrices are the step's own bits (the moment is the step itself) is
 * not written again. The bones of every skinned draw go into the step's set for the next step's pairing. */
/* (diagnostics, MH2_PASS_LOG=3 with the skip test's mode) how far the matrix the pass writes for the moment (half way from the step before the previous one to this one) is from the
 * previous step's own: the place (units) and the rotation (the Frobenius distance of the unit rows, about the angle in radians for a small one); and how far the matrix moved. */
static void PoseErr(const float *mid, const float *real, double *pl, double *an)
{
    double a[16], b[16], d = 0, e = 0;
    int i, j;
    ToD(mid, a);
    ToD(real, b);
    for (i = 0; i < 3; i++)
        e += (a[12 + i] - b[12 + i]) * (a[12 + i] - b[12 + i]);
    *pl = Sqrt(e);
    for (i = 0; i < 3; i++) {
        double na = Sqrt(a[i * 4] * a[i * 4] + a[i * 4 + 1] * a[i * 4 + 1] + a[i * 4 + 2] * a[i * 4 + 2]);
        double nb = Sqrt(b[i * 4] * b[i * 4] + b[i * 4 + 1] * b[i * 4 + 1] + b[i * 4 + 2] * b[i * 4 + 2]);
        if (!(na > 1e-9) || !(nb > 1e-9))
            continue;
        for (j = 0; j < 3; j++) {
            double x = a[i * 4 + j] / na - b[i * 4 + j] / nb;
            d += x * x;
        }
    }
    *an = Sqrt(d) * 0.70710678;
}

static struct {
    u32 n;
    double pl_sum, pl_max, an_sum, an_max, mv_sum, mva_sum;
} sPh;

static void PhaseAdd(const float *mid, const float *p1, const float *p2, const float *cur)
{
    double pl, an, mv, mva, z;
    PoseErr(mid, p1, &pl, &an);
    PoseErr(p2, cur, &mv, &mva);
    z = 0;
    sPh.n++;
    sPh.pl_sum += pl;
    sPh.an_sum += an;
    sPh.mv_sum += mv;
    sPh.mva_sum += mva;
    sPh.pl_max = pl > sPh.pl_max ? pl : sPh.pl_max;
    sPh.an_max = an > sPh.an_max ? an : sPh.an_max;
    (void)z;
}

static void PhaseLog(const char *what, u32 handle, u32 occ)
{
    char ln[300];
    u32 n = 0;
    u64 k;
    if (!sPh.n)
        return;
    Str(ln, &n, "mh2-core: pass phase ");
    Str(ln, &n, what);
    Str(ln, &n, " h ");
    Num(ln, &n, handle);
    Str(ln, &n, " occ ");
    Num(ln, &n, occ);
    Str(ln, &n, " n ");
    Num(ln, &n, sPh.n);
    k = (u64)(sPh.pl_sum / sPh.n * 100.0 + 0.5);
    Str(ln, &n, " place-err-mean*100 ");
    Num(ln, &n, k);
    Str(ln, &n, " max*100 ");
    Num(ln, &n, (u64)(sPh.pl_max * 100.0 + 0.5));
    Str(ln, &n, " ang-err-mean*1000 ");
    Num(ln, &n, (u64)(sPh.an_sum / sPh.n * 1000.0 + 0.5));
    Str(ln, &n, " max*1000 ");
    Num(ln, &n, (u64)(sPh.an_max * 1000.0 + 0.5));
    Str(ln, &n, " move-place*100 ");
    Num(ln, &n, (u64)(sPh.mv_sum / sPh.n * 100.0 + 0.5));
    Str(ln, &n, " move-ang*1000 ");
    Num(ln, &n, (u64)(sPh.mva_sum / sPh.n * 1000.0 + 0.5));
    ln[n] = 0;
    Host_Log(ln);
    memset(&sPh, 0, sizeof(sPh));
}

/* The parameters in a rigid draw's header that the game moves from step to step by themselves (a water's texture scroll: a qword (0, t, 0, 1) whose t goes up a
 * little each step): the matrices cannot move them, and the step's own value would show the later step's texture place. Each word of a header qword (outside the
 * draw's matrices) that differs from the paired draw's of the step before is blended as a number, when it is a float in a plain range and the change is a tenth
 * or less of its size (a wrap, a jump, a word that is not a float stay the step's own); a qword with one word that cannot be blended is left whole. The header
 * is saved for the next steps' pairing whether or not it is blended. Returns the qwords blended. */
static int sFlipQ[MAX_DRAWS];       /* the draw's flipbook offset qword (in its header), or -1 */
static float *sFlipP[MAX_DRAWS];    /* ... where it is in the copy, */
static float sFlipOld[MAX_DRAWS][2];   /* ... and the step before's components 0 and 1 */

/* A texture offset that steps from one frame of a sheet to the next (a flipbook) is not a place that moves: the frame between two is a place in the sheet that is no frame.
 * The qword is a texture matrix's translation row (its components 2 and 3 are 0 and 1, the two rows before it did not change), its component 0 or 1 changed by something that is
 * not a whole number within an eighth (a scroll's wrap is). */
static int FlipRow(const float *a, const float *b, u32 q, const float *a1, const float *b1, const float *a2, const float *b2)
{
    int k, far_ = 0, any = 0;
    (void)q;
    if (memcmp(a + 2, b + 2, 8) || a[2] != 0.0f || a[3] != 1.0f || memcmp(a1, b1, 16) || memcmp(a2, b2, 16))
        return 0;
    for (k = 0; k < 2; k++) {
        double dx = (double)b[k] - (double)a[k], r;
        if (!memcmp(a + k, b + k, 4))
            continue;
        if (!(a[k] > -1e4f && a[k] < 1e4f && b[k] > -1e4f && b[k] < 1e4f))
            return 0;
        dx = dx < 0 ? -dx : dx;
        r = dx - (double)(long)(dx + 0.5);
        r = r < 0 ? -r : r;
        any = 1;
        if (r > 0.125)
            far_ = 1;
    }
    return any && far_;
}

/* A row of a draw's header that a hold may put the step before's words into: every word that differs between the two steps is a float in the range
 * the blend itself takes. A word that is not one (a VIFcode, a count, a tag: the header's first quadwords hold the VIFcodes of the packets, and a code
 * that differs between the steps is a packet of another size) is never put into the step's own packet: the VIF unit would take the step before's count
 * for this step's data and run on into the packets that follow. */
static int HoldSafe(const float *a, const float *b)
{
    int k;
    for (k = 0; k < 4; k++) {
        double x, y;
        if (!memcmp(a + k, b + k, 4))
            continue;
        x = a[k] < 0 ? -(double)a[k] : (double)a[k];
        y = b[k] < 0 ? -(double)b[k] : (double)b[k];
        if (!(x < 1e6 && y < 1e6) || (x != 0.0 && x < 1e-6) || (y != 0.0 && y < 1e-6))
            return 0;
    }
    return 1;
}

static u32 HdrBlend(u32 d, PDraw *me, const PDraw *pd)
{
    float *qp[HQW];
    float lhq[HQW * 4], *hq = sShared ? lhq : me->hq;   /* (a helper's Models writes nothing of a set: the header's words are the copy's own, the ones the first moment kept) */
    u32 nq, q, nbl = 0;
    int mat = -1;
    nq = Gather(d, qp, HQW);
    if (!sShared)
        me->hqn = 0;
    if (nq < HQW)
        return 0;
    for (q = 0; q < HQW; q++) {
        memcpy(hq + q * 4, qp[q], 16);
        if (qp[q] == sF[d])
            mat = (int)q;
    }
    if (!sShared)
        me->hqn = HQW;
    if (sNoHdr || !pd || pd->hqn != HQW || sAlpha >= 1.0)
        return 0;
    for (q = 0; q < HQW; q++) {
        const float *a = pd->hq + q * 4, *b = hq + q * 4;
        double o[4];
        int k, ok = 1, any = 0;
        if (mat >= 0 && q + 0 >= (u32)mat && q < (u32)mat + 8)
            continue;
        for (k = 0; k < 4 && ok; k++) {
            double x = a[k], y = b[k], ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, big = ax > ay ? ax : ay, dx = y - x;
            dx = dx < 0 ? -dx : dx;
            o[k] = y;
            if (!memcmp(a + k, b + k, 4))
                continue;
            if (!(ax < 1e6 && ay < 1e6) || (ax != 0.0 && ax < 1e-6) || (ay != 0.0 && ay < 1e-6) || !(dx <= 0.1 * big || dx <= 0.125)) {
                ok = 0;
                break;
            }
            o[k] = x * (1.0 - sAlpha) + y * sAlpha;
            any = 1;
        }
        if (ok && any) {
            for (k = 0; k < 4; k++)
                qp[q][k] = (float)o[k];
            nbl++;
        } else if (!ok && sHeldNow && sHold2 && sAlpha < 0.5 && (sHoldSafe ? HoldSafe(a, b) : 1) && !(sFlipMode && q >= 2 && sFlipQ[d] < 0 && pd->dh == sDH[d] &&
                   FlipRow(a, b, q, pd->hq + (q - 1) * 4, hq + (q - 1) * 4, pd->hq + (q - 2) * 4, hq + (q - 2) * 4))) {   /* a draw held whole: its jumping rows too */
            for (k = 0; k < 4; k++)
                qp[q][k] = a[k];
            sN.held_hdr++;
        } else if (!ok && sFlipMode && q >= 2 && sFlipQ[d] < 0 && pd->dh == sDH[d] &&
                   FlipRow(a, b, q, pd->hq + (q - 1) * 4, hq + (q - 1) * 4, pd->hq + (q - 2) * 4, hq + (q - 2) * 4)) {
            sFlipQ[d] = (int)q;
            sFlipP[d] = qp[q];
            sFlipOld[d][0] = a[0];
            sFlipOld[d][1] = a[1];
            sN.flip_cand++;
        }
    }
    return nbl;
}

/* 2D packets that slide: the game's HUD sends sprites through VIF1's DIRECT to GIF PATH2 (a GIF tag and its PACKED vertices: the RGBAQ, ST, XYZ2 of each corner). A
 * packet of the step that has a partner in the step before (the same words but the places of its vertices, the n-th of its kind) is made with every vertex
 * between the two places, when none moved by more than the hold distance: a message that slides in is where it is half way, not where it ends. A packet whose
 * other words differ (a colour, a texture, a size), or that moved far, is the step's own. The VIF stream is followed word by word (the DMA tags' VIF codes and
 * the data), so that the DIRECT's data is found whether it is in a tag or after it. */
#define SPAN_QW 2048u            /* a DIRECT's data that the DMA cut into pieces is looked at when it is this long or shorter */
#define SPAN_SEGS 64u

typedef struct D2Pl {
    u32 off, q;                  /* MH2_PASS_D2IDX: a place of the 2D walk: the offset in the copy, the quadwords (with D2_SEG and D2_END) */
} D2Pl;

typedef struct V2 {
    u32 skip, dq, cl, wl;
    u32 spn, sp_tot, sp_unl;     /* the pieces of a DIRECT's data that has been gathered so far (the pieces, the quadwords, whether it is unlabelled) */
    u32 *sp_p[SPAN_SEGS];
    u32 sp_q[SPAN_SEGS];
} V2;

static u32 sSpanBuf[SPAN_QW * 4];

static u64 Fnv(u64 h, u32 w)
{
    h ^= w;
    h *= 1099511628211ull;
    return h;
}

/* GIF packets in qwc quadwords (a DIRECT's data, or a piece of PATH3's): each tag with its data is a packet. Returns the quadwords of the last packet's data that
 * the next piece has (0 when the packets end in this one). */
static u32 GifPackets(u32 *w, u32 qwc, PSet *cur, const PSet *past)
{
    u32 q = 0;
    while (q < qwc) {
        u32 *t = w + q * 4;
        u32 nloop = t[0] & 0x7FFF, flg = (t[1] >> 26) & 3, nreg = (t[1] >> 28) & 15, dq, i, k, nv = 0, nc = 0, voff;
        u64 key = 1469598103934665603ull, regs = (u64)t[2] | ((u64)t[3] << 32);
        u32 *xw[512], *yw[512], *cw[64];
        u8 ysh[512];
        if (nreg == 0)
            nreg = 16;
        if (flg == 0)
            dq = nloop * nreg;
        else if (flg == 1)
            dq = (nloop * nreg + 1) / 2;
        else if (flg == 2)
            dq = nloop;
        else
            return 0;
        if (q + 1 + dq > qwc)
            return q + 1 + dq - qwc;
        for (i = 0; i < 4; i++)
            key = Fnv(key, t[i]);
        if (flg == 0) {   /* PACKED: a qword a register; a place is x in the first word and y in the second, 16 bits each */
            for (i = 0; i < dq; i++) {
                u32 *d = t + 4 + i * 4, reg = (u32)((regs >> (4 * (i % nreg))) & 15);
                if (reg == 4 || reg == 5 || reg == 10 || reg == 11) {
                    if (nv < 512) {
                        xw[nv] = d;
                        yw[nv] = d + 1;
                        ysh[nv] = 0;
                    }
                    nv++;
                    key = Fnv(key, d[0] & 0xFFFF0000u);
                    key = Fnv(key, d[1] & 0xFFFF0000u);
                    key = Fnv(key, d[2]);
                    key = Fnv(key, d[3]);
                } else if (reg == 1 && sFade) {   /* RGBAQ: the colour is not in the key (it is what a fade moves) */
                    if (nc < 64)
                        cw[nc] = d;
                    nc++;
                    for (k = 0; k < 4; k++)
                        key = Fnv(key, d[k] & ~0xFFu);
                } else
                    for (k = 0; k < 4; k++)
                        key = Fnv(key, d[k]);
            }
        } else if (flg == 1) {   /* REGLIST: 64 bits a register; a place is x and y in the first word, 16 bits each */
            u32 s, total = nloop * nreg;
            for (s = 0; s < total; s++) {
                u32 *d = t + 4 + (s / 2) * 4 + (s % 2) * 2, reg = (u32)((regs >> (4 * (s % nreg))) & 15);
                if (reg == 4 || reg == 5 || reg == 10 || reg == 11) {
                    if (nv < 512) {
                        xw[nv] = d;
                        yw[nv] = d;
                        ysh[nv] = 16;
                    }
                    nv++;
                    key = Fnv(key, 0);
                    key = Fnv(key, d[1]);
                } else if (reg == 1 && sFade) {   /* RGBAQ: the colour is not in the key, Q is */
                    if (nc < 64)
                        cw[nc] = d;
                    nc++;
                    key = Fnv(key, 0);
                    key = Fnv(key, d[1]);
                } else {
                    key = Fnv(key, d[0]);
                    key = Fnv(key, d[1]);
                }
            }
            if (total & 1) {
                key = Fnv(key, t[4 + (total / 2) * 4 + 2]);
                key = Fnv(key, t[4 + (total / 2) * 4 + 3]);
            }
        } else
            for (i = 0; i < dq * 4; i++)
                key = Fnv(key, t[4 + i]);
        if (nv && nv <= 512 && nc <= 64 && cur->np2 < P2_MAX && cur->nv2 + nv + nc <= P2V_MAX) {
            P2 *p = &cur->p2[cur->np2];
            u32 occ = 0;
            for (k = 0; k < cur->np2; k++)
                if (cur->p2[k].key == key)
                    occ++;
            p->key = key;
            p->occ = occ;
            p->nv = nv;
            p->nc = nc;
            p->prim = (t[1] >> 14) & 1 ? (t[1] >> 15) & 0x7FFu : 0xFFFFu;
            p->tex = 0;
            for (k = 0; k < nreg && k < 16; k++) {
                u32 rg = (u32)((regs >> (4 * k)) & 15);
                if (rg == 2 || rg == 3)
                    p->tex |= 1;
                else if (rg == 14)
                    p->tex |= 2;
            }
            p->form = (u32)Fnv(Fnv(Fnv(1469598103934665603ull, t[1]), t[2]), t[3]) ^ (nv << 8) ^ (nc << 20);
            p->off = voff = cur->nv2;
            for (k = 0; k < nv; k++)
                cur->p2v[voff + k] = (xw[k][0] & 0xFFFFu) | (((yw[k][0] >> ysh[k]) & 0xFFFFu) << 16);
            for (k = 0; k < nc; k++)
                cur->p2v[voff + nv + k] = flg == 0 ? ((cw[k][0] & 0xFFu) | ((cw[k][1] & 0xFFu) << 8) | ((cw[k][2] & 0xFFu) << 16) | ((cw[k][3] & 0xFFu) << 24)) : cw[k][0];
            cur->np2++;
            cur->nv2 += nv + nc;
            sN.p2_packets++;
            if (past && past->p2 && sAlpha < 1.0) {
                const P2 *pp = 0;
                for (k = 0; k < past->np2; k++)
                    if (past->p2[k].key == key && past->p2[k].occ == occ && past->p2[k].nv == nv && past->p2[k].nc == nc) {
                        pp = &past->p2[k];
                        break;
                    }
                if (!pp && (sBD || sLog == 6)) {   /* a packet with no partner: is there one of its kind near it in the step before? If not it is born in the span: it fades in */
                    double best = -1.0, gt = sHoldPx > 0.0 ? sHoldPx * 16.0 * (double)sBack : 1e9;
                    u32 near = 0, same = 0;
                    char ln[260];
                    u32 n2 = 0, a0 = cur->p2v[voff];
                    for (k = 0; k < past->np2; k++)
                        if (past->p2[k].form == cur->p2[cur->np2 - 1].form) {
                            u32 b0 = past->p2v[past->p2[k].off];
                            double dx = (double)(a0 & 0xFFFFu) - (double)(b0 & 0xFFFFu), dy = (double)(a0 >> 16) - (double)(b0 >> 16), dd;
                            dx = dx < 0 ? -dx : dx;
                            dy = dy < 0 ? -dy : dy;
                            dd = dx > dy ? dx : dy;
                            same++;
                            if (best < 0 || dd < best)
                                best = dd;
                            if (dd <= gt)
                                near++;
                        }
                    if (!near && sBD && nv && sHoldPx > 0.0) {
                        t[0] |= 1u << 16;
                        sN.bd_born++;
                    } else
                        sN.bd_changed++;
                    if (sLog == 6) {
                    Str(ln, &n2, "mh2-core: pass 2d nopartner step ");
                    Num(ln, &n2, sStepNo);
                    Str(ln, &n2, " tag1 ");
                    Num(ln, &n2, t[1]);
                    Str(ln, &n2, " nv ");
                    Num(ln, &n2, nv);
                    Str(ln, &n2, " nc ");
                    Num(ln, &n2, nc);
                    Str(ln, &n2, " at ");
                    Num(ln, &n2, a0 & 0xFFFFu);
                    Str(ln, &n2, " ");
                    Num(ln, &n2, a0 >> 16);
                    Str(ln, &n2, " same-form ");
                    Num(ln, &n2, same);
                    Str(ln, &n2, " near ");
                    Num(ln, &n2, near);
                    Str(ln, &n2, " nearest ");
                    Num(ln, &n2, best < 0 ? 0xFFFFFFFFu : (u64)best);
                    ln[n2] = 0;
                    Host_Log(ln);
                    }
                }
                if (pp) {
                    double gate = sHoldPx > 0.0 ? sHoldPx * 16.0 * (double)sBack : 1e9, nx[512], ny[512];   /* (the span of two steps in the skip test: twice the distance) */
                    int ok = 1, moved = 0;
                    for (k = 0; k < nv && ok; k++) {
                        u32 a = past->p2v[pp->off + k], b = cur->p2v[voff + k];
                        double ax = (double)(a & 0xFFFFu), ay = (double)(a >> 16), bx = (double)(b & 0xFFFFu), by = (double)(b >> 16);
                        double ddx = bx - ax, ddy = by - ay;
                        if ((ddx < 0 ? -ddx : ddx) > gate || (ddy < 0 ? -ddy : ddy) > gate)
                            ok = 0;
                        nx[k] = ax + ddx * sAlpha + 0.5;
                        ny[k] = ay + ddy * sAlpha + 0.5;
                        if (a != b)
                            moved = 1;
                    }
                    if (ok && sFade && nc) {   /* the colours of the packet that did not move: a fade's sprite */
                        u32 any = 0;
                        for (k = 0; k < nc; k++) {
                            u32 a = past->p2v[pp->off + nv + k], b = cur->p2v[voff + nv + k], r = 0, c;
                            if (a == b)
                                continue;
                            for (c = 0; c < 4; c++) {
                                double ca = (double)((a >> (8 * c)) & 0xFFu), cb = (double)((b >> (8 * c)) & 0xFFu);
                                r |= ((u32)(ca + (cb - ca) * sAlpha + 0.5) & 0xFFu) << (8 * c);
                            }
                            if (flg == 0)
                                for (c = 0; c < 4; c++)
                                    cw[k][c] = (cw[k][c] & ~0xFFu) | ((r >> (8 * c)) & 0xFFu);
                            else
                                cw[k][0] = r;
                            any = 1;
                        }
                        if (any)
                            sN.p2_faded++;
                    }
                    if (!ok && sHold2 && sAlpha < 0.5 && sP2) {   /* a packet that jumps: where the step before had it */
                        for (k = 0; k < nv; k++) {
                            u32 a = past->p2v[pp->off + k];
                            if (ysh[k] == 0) {
                                xw[k][0] = (xw[k][0] & 0xFFFF0000u) | (a & 0xFFFFu);
                                yw[k][0] = (yw[k][0] & 0xFFFF0000u) | (a >> 16);
                            } else
                                xw[k][0] = a;
                        }
                        sN.held_p2++;
                    }
                    if (ok && moved && sP2) {
                        for (k = 0; k < nv; k++) {
                            if (ysh[k] == 0) {
                                xw[k][0] = (xw[k][0] & 0xFFFF0000u) | ((u32)nx[k] & 0xFFFFu);
                                yw[k][0] = (yw[k][0] & 0xFFFF0000u) | ((u32)ny[k] & 0xFFFFu);
                            } else
                                xw[k][0] = ((u32)nx[k] & 0xFFFFu) | (((u32)ny[k] & 0xFFFFu) << 16);
                        }
                        sN.p2_blended++;
                    }
                }
            }
        }
        q += 1 + dq;
    }
    return 0;
}

/* The packets of the step before that the step has no partner for, once its window's 2D packets are all known: a packet of its kind near it in the step is a changed sprite, otherwise it is dead
 * (the census; MH2_PASS_LOG=6 gives a line for each one). */
static void DeadCensus(const PSet *cur, const PSet *past)
{
    u32 k, j;
    if (!past || !past->p2 || !cur->np2 || sAlpha >= 1.0)
        return;
    for (k = 0; k < past->np2; k++) {
        const P2 *pp = &past->p2[k];
        double best = -1.0, gt = sHoldPx > 0.0 ? sHoldPx * 16.0 * (double)sBack : 1e9;
        u32 near = 0, same = 0, a0 = past->p2v[pp->off], used = 0;
        for (j = 0; j < cur->np2 && !used; j++) {
            const P2 *cp = &cur->p2[j];
            if (cp->key == pp->key && cp->occ == pp->occ && cp->nv == pp->nv && cp->nc == pp->nc)
                used = 1;
            else if (cp->form == pp->form) {
                u32 b0 = cur->p2v[cp->off];
                double dx = (double)(a0 & 0xFFFFu) - (double)(b0 & 0xFFFFu), dy = (double)(a0 >> 16) - (double)(b0 >> 16), dd;
                dx = dx < 0 ? -dx : dx;
                dy = dy < 0 ? -dy : dy;
                dd = dx > dy ? dx : dy;
                same++;
                if (best < 0 || dd < best)
                    best = dd;
                if (dd <= gt)
                    near++;
            }
        }
        if (used)
            continue;
        if (near)
            sN.bd_dead_changed++;
        else
            sN.bd_dead++;
        if (sLog == 6) {
            char ln[260];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass 2d dead step ");
            Num(ln, &n2, sStepNo);
            Str(ln, &n2, " form ");
            Num(ln, &n2, pp->form);
            Str(ln, &n2, " nv ");
            Num(ln, &n2, pp->nv);
            Str(ln, &n2, " nc ");
            Num(ln, &n2, pp->nc);
            Str(ln, &n2, " at ");
            Num(ln, &n2, a0 & 0xFFFFu);
            Str(ln, &n2, " ");
            Num(ln, &n2, a0 >> 16);
            Str(ln, &n2, " colour ");
            Num(ln, &n2, pp->nc ? past->p2v[pp->off + pp->nv] : 0);
            Str(ln, &n2, " prim ");
            Num(ln, &n2, pp->prim);
            Str(ln, &n2, " tex ");
            Num(ln, &n2, pp->tex);
            Str(ln, &n2, " same-form ");
            Num(ln, &n2, same);
            Str(ln, &n2, " near ");
            Num(ln, &n2, near);
            Str(ln, &n2, " nearest ");
            Num(ln, &n2, best < 0 ? 0xFFFFFFFFu : (u64)best);
            ln[n2] = 0;
            Host_Log(ln);
        }
    }
}

/* MH2_PASS_D2IDX: the places of the unlabelled GIF data the 2D walk comes to, in its order: the offset in the copy and the quadwords of each GifPackets it makes (a DIRECT's data in
 * one piece, or what an unlabelled PATH3 piece has past a packet that ran on into it), and for a DIRECT's data that the DMA cut into pieces each piece (D2_SEG, the last with
 * D2_END), which are gathered and looked at as one. The window's first moment's walk notes them; a later moment starts from the copy as the window left it, no moment changes a VIF
 * code or a GIF tag, so its walk would come to the same places, and it visits them from the list instead. A list that is full is not used (the walk is made as before). */
#define D2_MAX 8192u
#define D2_SEG 0x80000000u
#define D2_END 0x40000000u
#define D2_QM 0x3FFFFFFFu
static D2Pl *sD2;                /* Host_Alloc'ed when the switch is first used */
static u32 sD2N, sD2At, sD2Seen;
static int sD2Ok, sD2Mode, sD2Diff;   /* the list is the window's; 1 the walk notes its places, 2 it compares them with the list; a place differed */

static int D2Room(void)
{
    if (!sD2) {
        sD2 = Host_Alloc(D2_MAX * sizeof(D2Pl));
        if (!sD2) {
            Host_Log("mh2-core: pass: no memory for the 2D walk's places: the walk as before");
            sD2x = 0;
            return 0;
        }
    }
    return 1;
}

static void D2Note(const u32 *p, u32 q, u32 fl)
{
    u32 off = (u32)((const u8 *)p - sEv);
    if (sD2Mode == 1) {
        if (sDCtl == 3 && !fl && (sD2Seen++ & 7u) == 3u)   /* the control: this place is not in the list */
            return;
        if (sD2N < D2_MAX) {
            sD2[sD2N].off = off;
            sD2[sD2N].q = q | fl;
            sD2N++;
        } else
            sD2Ok = 0;
    } else {
        if (sD2At >= sD2N || sD2[sD2At].off != off || sD2[sD2At].q != (q | fl))
            sD2Diff = 1;
        sD2At++;
    }
}

/* The 2D walk of a later moment, from the list. */
static void Direct2DList(PSet *cur, const PSet *pp)
{
    u32 i = 0;
    cur->np2 = cur->nv2 = 0;
    while (i < sD2N) {
        if (!(sD2[i].q & D2_SEG)) {
            GifPackets((u32 *)(sEv + sD2[i].off), sD2[i].q, cur, pp);
            i++;
        } else {
            u32 i0 = i, tot = 0, at = 0, k, last;
            u64 was = sN.p2_blended + sN.p2_faded;
            do {
                u32 q = sD2[i].q & D2_QM;
                memcpy(sSpanBuf + tot * 4, sEv + sD2[i].off, q * 16);
                tot += q;
                last = sD2[i].q & D2_END;
                i++;
            } while (!last && i < sD2N);
            GifPackets(sSpanBuf, tot, cur, pp);
            if (sN.p2_blended + sN.p2_faded != was)
                for (k = i0; k < i; k++) {
                    u32 q = sD2[k].q & D2_QM;
                    memcpy(sEv + sD2[k].off, sSpanBuf + at * 4, q * 16);
                    at += q;
                }
        }
    }
    sD2s[0]++;
    sD2s[1] += sD2N;
    DeadCensus(cur, pp);
}

static void Direct2D(void)
{
    PSet *cur = Cur(), *past = Past(sBack);
    V2 v;
    u32 off, gskip = 0;
    sD2Mode = 0;
    if (sD2x && !sLaterMo && D2Room()) {   /* the window's first moment: its walk notes the places */
        sD2N = sD2Seen = 0;
        sD2Ok = 1;
        sD2Mode = 1;
    }
    if (!cur->p2)
        return;
    if (sD2x && sLaterMo && sD2Ok && !sMomCtl) {
        if (sD2x == 1) {
            Direct2DList(cur, past && past->have ? past : 0);
            return;
        }
        sD2Mode = 2;
        sD2At = 0;
        sD2Diff = 0;
    }
    cur->np2 = cur->nv2 = 0;
    v.skip = v.dq = v.spn = 0;
    v.cl = v.wl = 1;
    for (off = 0; off < sEvLen;) {
        PEv *e = (PEv *)(sEv + off);
        u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
        u32 tmp[2], *w = 0, nw = 0, j = 0;
        if (e->kind == 3) {   /* PATH3: GIF packets, one after the other, whole or in pieces of up to 64 quadwords */
            u32 qn = e->n, at = 0;
            if (gskip) {
                at = gskip < qn ? gskip : qn;
                gskip -= at;
            }
            if (at < qn && e->draw == 0xFFFFFFFFu) {
                if (sD2Mode)
                    D2Note((u32 *)(e + 1) + at * 4, qn - at, 0);
                gskip = GifPackets((u32 *)(e + 1) + at * 4, qn - at, cur, past && past->have ? past : 0);
            } else if (at < qn) {
                /* a labelled draw's PATH3 data is not looked at, but its packets' lengths are followed */
                u32 *g = (u32 *)(e + 1) + at * 4, qq = qn - at, q2 = 0;
                while (q2 < qq) {
                    u32 f = (g[q2 * 4 + 1] >> 26) & 3, nl = g[q2 * 4] & 0x7FFF, nr = (g[q2 * 4 + 1] >> 28) & 15, d2;
                    if (!nr)
                        nr = 16;
                    d2 = f == 0 ? nl * nr : f == 1 ? (nl * nr + 1) / 2 : f == 2 ? nl : 0;
                    if (q2 + 1 + d2 > qq) {
                        gskip = q2 + 1 + d2 - qq;
                        break;
                    }
                    q2 += 1 + d2;
                }
            }
            off += 16 + bytes;
            continue;
        }
        if (e->kind == 1) {
            w = (u32 *)(e + 1);
            nw = e->n * 4;
        } else if (e->kind == 2) {
            memcpy(tmp, e + 1, 8);   /* the DMA tag's upper half: two VIF codes */
            w = tmp;
            nw = 2;
        }
        while (j < nw) {
            u32 code, cmd, num, imm;
            if (v.skip) {
                u32 t = nw - j < v.skip ? nw - j : v.skip;
                j += t;
                v.skip -= t;
                continue;
            }
            if (v.dq) {
                u32 availq = (nw - j) / 4;
                if (!v.spn && (j & 3) == 0 && availq >= v.dq) {
                    if (e->kind == 1 && e->draw == 0xFFFFFFFFu) {
                        if (sD2Mode)
                            D2Note(w + j, v.dq, 0);
                        GifPackets(w + j, v.dq, cur, past && past->have ? past : 0);
                    }
                    j += v.dq * 4;
                    v.dq = 0;
                } else if (e->kind == 1 && (j & 3) == 0 && availq && v.spn < SPAN_SEGS && (v.spn ? v.sp_tot + v.dq <= SPAN_QW : v.dq <= SPAN_QW)) {
                    /* the data runs on into the next piece: the pieces are gathered, and the packets in them are looked at when the last has come */
                    u32 take = availq < v.dq ? availq : v.dq;
                    if (!v.spn) {
                        v.sp_tot = 0;
                        v.sp_unl = e->draw == 0xFFFFFFFFu;
                        v.sp_tot = 0;
                    }
                    v.sp_p[v.spn] = w + j;
                    v.sp_q[v.spn] = take;
                    memcpy(sSpanBuf + v.sp_tot * 4, w + j, take * 16);
                    v.sp_tot += take;
                    v.spn++;
                    j += take * 4;
                    v.dq -= take;
                    if (!v.dq) {
                        if (v.sp_unl) {
                            u64 was = sN.p2_blended + sN.p2_faded;
                            u32 i3, at = 0;
                            if (sD2Mode)
                                for (i3 = 0; i3 < v.spn; i3++)
                                    D2Note(v.sp_p[i3], v.sp_q[i3], D2_SEG | (i3 + 1 == v.spn ? D2_END : 0));
                            GifPackets(sSpanBuf, v.sp_tot, cur, past && past->have ? past : 0);
                            if (sN.p2_blended + sN.p2_faded != was)
                                for (i3 = 0; i3 < v.spn; i3++) {
                                    memcpy(v.sp_p[i3], sSpanBuf + at * 4, v.sp_q[i3] * 16);
                                    at += v.sp_q[i3];
                                }
                        }
                        v.spn = 0;
                    }
                } else {   /* the data does not start on a quadword, or is too long: not looked at */
                    v.spn = 0;
                    u32 words = v.dq * 4, t = nw - j < words ? nw - j : words;
                    j += t;
                    words -= t;
                    v.dq = words / 4;
                    v.skip = words & 3;
                }
                continue;
            }
            code = w[j++];
            cmd = (code >> 24) & 0x7F;
            num = (code >> 16) & 0xFF;
            imm = code & 0xFFFF;
            if ((cmd & 0x60) == 0x60) {   /* UNPACK */
                u32 vn = (cmd >> 2) & 3, vl = cmd & 3, vec = vl == 3 ? 16 : (vl == 0 ? 32 : vl == 1 ? 16 : 8) * (vn + 1), cnt = num ? num : 256;
                if (v.cl > v.wl && v.wl)
                    cnt = cnt / v.cl * v.wl + (cnt % v.cl < v.wl ? cnt % v.cl : v.wl);
                v.skip = (cnt * vec + 31) / 32;
            } else if (cmd == 0x01) {
                v.cl = imm & 0xFF;
                v.wl = (imm >> 8) & 0xFF;
            } else if (cmd == 0x20)
                v.skip = 1;
            else if (cmd == 0x30 || cmd == 0x31)
                v.skip = 4;
            else if (cmd == 0x4A)
                v.skip = (num ? num : 256) * 2;
            else if (cmd == 0x50 || cmd == 0x51)
                v.dq = imm ? imm : 65536u;
        }
        off += 16 + bytes;
    }
    if (sD2Mode == 1) {
        if (!sD2Ok)
            sD2s[5]++;
        if (sD2N > sD2s[6])
            sD2s[6] = sD2N;
    } else if (sD2Mode == 2) {
        sD2s[2]++;
        sD2s[3] += sD2At;
        if (sD2Diff || sD2At != sD2N)
            sD2s[4]++;
    }
    sD2Mode = 0;
    DeadCensus(cur, past && past->have ? past : 0);
}

static void Models(void)
{
    double vb[16];
    PSet *cur = Cur(), *past = Past(sBack);
    int viewok = 0, vexact = 0;
    u32 d, i;
    sSkipMom = 0;
    sHeldNow = 0;
    if (cur->have && past->have && past->d) {
        viewok = ViewBlend(past->view, cur->view, sAlpha >= 1.0 ? 0.0 : sAlpha, vb);
        vexact = viewok && (sAlpha >= 1.0 || !memcmp(past->view, cur->view, sizeof(cur->view)));   /* the camera does not move: the step's own */
        if (!viewok) {
            sN.cam_cut += sNDraws;
            if (sHold2 && sAlpha < 0.5) {
                sSkipMom = 1;
                sN.skip_mom++;
            }
        }
    }
    if (viewok && sLog == 3 && sMode == 4 && Past(1)->have) {   /* the camera: the pass's moment against the previous step's own */
        double e1[3], e2[3], e3[3], e4[3], r1[9], r2[9], r3[9], r4[9], p1v[16], pv[16], cv[16], de = 0, dr = 0, mv = 0, mr = 0;
        char ln[300];
        u32 n = 0, i2;
        ToD(Past(1)->view, p1v);
        ToD(past->view, pv);
        ToD(cur->view, cv);
        if (ViewSplit(vb, e1, r1) && ViewSplit(p1v, e2, r2) && ViewSplit(pv, e3, r3) && ViewSplit(cv, e4, r4)) {
            for (i2 = 0; i2 < 3; i2++) {
                de += (e1[i2] - e2[i2]) * (e1[i2] - e2[i2]);
                mv += (e3[i2] - e4[i2]) * (e3[i2] - e4[i2]);
            }
            for (i2 = 0; i2 < 9; i2++) {
                dr += (r1[i2] - r2[i2]) * (r1[i2] - r2[i2]);
                mr += (r3[i2] - r4[i2]) * (r3[i2] - r4[i2]);
            }
            Str(ln, &n, "mh2-core: pass phase camera eye-err*100 ");
            Num(ln, &n, (u64)(Sqrt(de) * 100.0 + 0.5));
            Str(ln, &n, " ang-err*1000 ");
            Num(ln, &n, (u64)(Sqrt(dr) * 707.1 + 0.5));
            Str(ln, &n, " move-eye*100 ");
            Num(ln, &n, (u64)(Sqrt(mv) * 100.0 + 0.5));
            Str(ln, &n, " move-ang*1000 ");
            Num(ln, &n, (u64)(Sqrt(mr) * 707.1 + 0.5));
            ln[n] = 0;
            Host_Log(ln);
        }
    }
    HashNow();
    for (d = 0; d < sNDraws; d++) {
        const IpcLabel *l = &sDraws[d];
        double n[16], c[16], world[16], *view = 0;
        float newm[16];
        int form = sForm[d], own_view, changed = 0, r, pos[32], k;
        float *qp[VQW];
        PDraw *pd = 0, *me = sDIdx[d] >= 0 ? &cur->d[sDIdx[d]] : 0, *p1 = 0;
        memset(&sPh, 0, sizeof(sPh));
        int found = 0, vanim = 0, cut = 0, nob = 0, ng = 0, held = 0, gpos[GMAX];
        float gcur[GMAX][16], gbl[GMAX][16];
        sCls[d] = l->handle ? 4 : 128;
        sFlipQ[d] = -1;
        if (!sF[d]) {
            sCls[d] = 8;
            sN.cam_nomatrix++;
            if (sLog == 1)
                LogDraw(d, me, 0, 0, 0, 0, 0, 0);
            if (sLog == 2)
                DiagUnfound(d);
            continue;
        }
        own_view = !memcmp(l->cam, cur->view, 16 * sizeof(float));
        if (!own_view)
            sN.cam_other++;
        if (me && past->d && past->have)
            pd = Lookup(past, l->handle, me->occ);
        if (me && sLog == 3 && sMode == 4 && pd && Past(1)->d && Past(1)->have)
            p1 = Lookup(Past(1), l->handle, me->occ);
        if (me && !sShared)
            me->dh = sDH[d];
        if (pd && !sShared)
            pd->used = 1;
        if (sLog == 3 && pd) {
            char ln[120];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass dh h ");
            Num(ln, &n2, l->handle);
            Str(ln, &n2, pd->dh != sDH[d] ? " differs" : " same");
            ln[n2] = 0;
            Host_Log(ln);
        }
        ToD(l->world, world);
        if (form == 1) {
            if (pd) {
                r = MatBlendT(pd->world, l->world, sAlpha, newm, 1);
                if (r == 0 && Jump(l, pd->world, l->world))
                    r = 1;
                if (r == 1) {
                    cut = 1, sN.mod_cut++;
                    if (sHold2 && sAlpha < 0.5) {   /* below one half a model that jumps is where the step before had it */
                        memcpy(newm, pd->world, sizeof(newm));
                        ToD(newm, world);
                        changed = 1;
                        held = 1;
                        sN.held_mod++;
                    }
                } else if (r == 2)
                    sN.mod_still++;
                else {
                    if (p1 && r == 0) {
                        PhaseAdd(newm, p1->world, pd->world, l->world);
                        PhaseLog("rigid", l->handle, me->occ);
                    }
                    sN.mod_rigid++;
                    ToD(newm, world);
                    changed = 1;
                }
            } else if (l->handle)
                sN.mod_birth++;
        } else {
            found = FindBones(d, l, pos, qp);
            if (sLog == 3) {   /* bones: the table's non-zero slots against the ones found in the data (a draw with fewer found than non-zero moves some of its bones only) */
                u32 nzs = 0, i2;
                char ln[200];
                u32 n2 = 0;
                for (k = 1; k < 32; k++) {
                    int z = 1;
                    for (i2 = 0; i2 < 16; i2++)
                        if (l->slots[k * 16 + i2] != 0.0f)
                            z = 0;
                    nzs += !z;
                }
                if (found == 1)
                    DiagSkin(d, l);
                Str(ln, &n2, "mh2-core: pass bones h ");
                Num(ln, &n2, l->handle);
                Str(ln, &n2, " nonzero ");
                Num(ln, &n2, nzs);
                Str(ln, &n2, " found ");
                Num(ln, &n2, (u64)found);
                Str(ln, &n2, " looks-like-bones ");
                Num(ln, &n2, (u64)sNm);
                ln[n2] = 0;
                Host_Log(ln);
            }
            ng = sDataMats ? FindDataMats(pos, qp, gpos) : 0;
            for (k = 0; k < ng; k++) {
                memcpy(gcur[k], qp[gpos[k]], 16);
                memcpy(gcur[k] + 4, qp[gpos[k] + 1], 16);
                memcpy(gcur[k] + 8, qp[gpos[k] + 2], 16);
                memcpy(gcur[k] + 12, qp[gpos[k] + 3], 16);
            }
            if (!found && !ng) {
                sN.skin_nobones++;
                nob = 1;
                if (sLog == 3)
                    DiagSkin(d, l);
            }
            else if (pd) {
                float blended[32][16];
                int did[32], gdid[GMAX], nbones = 0, ok = 1, ngb = 0;
                memset(did, 0, sizeof(did));
                memset(gdid, 0, sizeof(gdid));
                for (k = 1; k < 32 && ok; k++) {
                    const PBone *pb = 0;
                    u32 j;
                    if (pos[k] < 0)
                        continue;
                    for (j = 0; j < pd->nb; j++)
                        if (past->b[pd->bfirst + j].slot == (u32)k) {
                            pb = &past->b[pd->bfirst + j];
                            break;
                        }
                    if (!pb) {
                        sN.bone_nopartner++;
                        if (sLog == 6) {   /* (census, MH2_PASS_LOG=6) a bone found in this draw's data that the draw of the step before did not have: which, and what the step before had */
                            char ln[300];
                            u32 n2 = 0, nf = 0, i2;
                            for (i2 = 1; i2 < 32; i2++)
                                nf += pos[i2] >= 0;
                            Str(ln, &n2, "mh2-core: pass bone nopartner step ");
                            Num(ln, &n2, sStepNo);
                            Str(ln, &n2, " h ");
                            Num(ln, &n2, l->handle);
                            Str(ln, &n2, " prog ");
                            Num(ln, &n2, (u64)(u32)l->prog);
                            Str(ln, &n2, " site ");   /* the draw's call site (PC_GS_MODEL's return address, decimal) */
                            Num(ln, &n2, (u64)(u32)l->site);
                            Str(ln, &n2, " slot ");
                            Num(ln, &n2, (u64)k);
                            Str(ln, &n2, " found-now ");
                            Num(ln, &n2, nf);
                            Str(ln, &n2, " found-before ");
                            Num(ln, &n2, pd->nb);
                            Str(ln, &n2, " before-slots");
                            for (i2 = 0; i2 < pd->nb && i2 < 12; i2++) {
                                Str(ln, &n2, " ");
                                Num(ln, &n2, past->b[pd->bfirst + i2].slot);
                            }
                            ln[n2] = 0;
                            Host_Log(ln);
                        }
                        continue;
                    }
                    r = MatBlendT(pb->m, l->slots + k * 16, sAlpha, blended[k], 2);
                    if (r == 0 && Jump(l, pb->m, l->slots + k * 16))
                        r = 1;
                    if (r == 1)
                        ok = 0;
                    else if (r == 0) {
                        if (p1) {
                            PSet *s1 = Past(1);
                            u32 j1;
                            for (j1 = 0; j1 < p1->nb; j1++)
                                if (s1->b[p1->bfirst + j1].slot == (u32)k) {
                                    PhaseAdd(blended[k], s1->b[p1->bfirst + j1].m, pb->m, l->slots + k * 16);
                                    break;
                                }
                        }
                        did[k] = 1;
                        nbones++;
                    }
                }
                for (k = 0; k < ng && ok; k++) {   /* the matrices that are not slots: by their qword */
                    const PBone *pb = 0;
                    u32 j;
                    for (j = 0; j < pd->nb; j++)
                        if (past->b[pd->bfirst + j].slot == 1000u + (u32)gpos[k]) {
                            pb = &past->b[pd->bfirst + j];
                            break;
                        }
                    if (!pb)
                        continue;
                    r = MatBlendT(pb->m, gcur[k], sAlpha, gbl[k], 3);
                    if (r == 1)
                        ok = 0;
                    else if (r == 0) {
                        gdid[k] = 1;
                        ngb++;
                    }
                }
                if (p1 && sPh.n)
                    PhaseLog("skin", l->handle, me->occ);
                if (!ok) {
                    cut = 1, sN.skin_cut++;
                    if (sHold2 && sAlpha < 0.5) {   /* every bone of the draw as the step before had it */
                        int nh = 0;
                        for (k = 1; k < 32; k++) {
                            u32 j;
                            if (pos[k] < 0)
                                continue;
                            for (j = 0; j < pd->nb; j++)
                                if (past->b[pd->bfirst + j].slot == (u32)k) {
                                    PutMat(qp, pos[k], past->b[pd->bfirst + j].m);
                                    nh++;
                                    break;
                                }
                        }
                        for (k = 0; k < ng; k++) {
                            u32 j;
                            for (j = 0; j < pd->nb; j++)
                                if (past->b[pd->bfirst + j].slot == 1000u + (u32)gpos[k]) {
                                    PutMat(qp, gpos[k], past->b[pd->bfirst + j].m);
                                    nh++;
                                    break;
                                }
                        }
                        if (nh) {
                            changed = 1;
                            held = 1;
                            sN.held_skin++;
                        }
                    }
                } else if (nbones || ngb) {
                    for (k = 1; k < 32; k++)
                        if (did[k])
                            PutMat(qp, pos[k], blended[k]);
                    for (k = 0; k < ng; k++)
                        if (gdid[k])
                            PutMat(qp, gpos[k], gbl[k]);
                    sN.data_mats += (u64)ngb;
                    sN.skin_blended++;
                    sN.bones_blended += (u64)nbones;
                    changed = 1;
                } else
                    sN.skin_still++;
            } else if (l->handle)
                sN.skin_birth++;
            if (me && (found || ng) && sRecord) {   /* what the next step pairs with (the first moment's) */
                me->bfirst = cur->nb;
                for (k = 1; k < 32; k++)
                    if (pos[k] >= 0) {
                        if (cur->nb >= SET_BONES) {
                            sN.bones_over++;
                            break;
                        }
                        cur->b[cur->nb].slot = (u32)k;
                        memcpy(cur->b[cur->nb].m, l->slots + k * 16, 64);
                        cur->nb++;
                        me->nb++;
                    }
                for (k = 0; k < ng && cur->nb < SET_BONES; k++) {
                    cur->b[cur->nb].slot = 1000u + (u32)gpos[k];
                    memcpy(cur->b[cur->nb].m, gcur[k], 64);
                    cur->nb++;
                    me->nb++;
                }
            }
        }
        if (form == 1 && me) {
            u32 nb2;
            u64 th = sTime ? Ipc_TimeNs() : 0;
            sHeldNow = held;
            nb2 = HdrBlend(d, me, pd);
            sHeldNow = 0;
            if (sTime)
                sTAcc[sLaterMo ? T_LHDR : T_HDR] += Ipc_TimeNs() - th;
            if (nb2) {
                sN.hdr_draws++;
                sN.hdr_qwords += nb2;
            }
        }
        if (pd && !(changed & 1) && pd->dh != sDH[d]) {   /* the matrices are still (or not blended) and the vertices are not the step before's */
            sN.vanim++;
            vanim = 1;
        }
        if (own_view && viewok)
            view = vb;
        sCls[d] = l->handle ? (changed & 1 && !held ? 1 : (!pd ? 16 : (nob ? 64 : (cut ? 32 : (vanim ? 2 : 4))))) : 128;
        if (view && !vexact)
            changed |= 2;
        if (!changed) {
            if (sLog == 1)
                LogDraw(d, me, form, own_view, 1, pd != 0, vanim ? 4 : 0, 0);
            continue;
        }
        if (!view)   /* the model moved, the view did not: the step's own */
            ToD(l->cam, vb), view = vb;
        {   /* the step's own matrices, and what the in-between moment adds to them: the sum is the game's own bits when nothing moved, and the rounding of the
             * game's float chain stays what it was (a whole new product would jitter every vertex by a sixteenth of a pixel) */
            double w0[16], v0[16], n0[16], c0[16];
            ToD(l->world, w0);
            ToD(l->cam, v0);
            DrawMatrices(l, w0, v0, form, n0, c0);
            DrawMatrices(l, world, view, form, n, c);
            for (i = 0; i < 16; i++) {
                sF[d][i] = (float)((double)sF[d][i] + (n[i] - n0[i]));
                sF[d][16 + i] = (float)((double)sF[d][16 + i] + (c[i] - c0[i]));
            }
        }
        sN.cam_done++;
        if (sLog == 1) {
            double o[16];
            if (form == 1)
                LogDraw(d, me, form, own_view, 1, pd != 0, changed | (vanim ? 4 : 0), c);
            else {   /* a skinned draw has no model matrix: the place of its first bone */
                ToD(l->slots + 16, o);
                {
                    double p4[4] = {o[12], o[13], o[14], 1.0}, q[4];
                    int a, b;
                    for (a = 0; a < 4; a++) {
                        q[a] = 0;
                        for (b = 0; b < 4; b++)
                            q[a] += p4[b] * c[b * 4 + a];
                    }
                    o[12] = q[0];
                    o[13] = q[1];
                    o[14] = q[2];
                    o[15] = q[3];
                }
                LogDraw(d, me, form, own_view, 1, pd != 0, changed | (vanim ? 4 : 0), o);
            }
        }
    }
    if (sLog == 4 && sHdrH)
        for (d = 0; d < sNDraws; d++) {
            u32 off, nq = 0;
            if (sDraws[d].handle != sHdrH)
                continue;
            for (off = 0; off < sEvLen && nq < 40;) {
                PEv *e = (PEv *)(sEv + off);
                u32 bytes = e->kind == 2 ? 16 : e->kind == 1 || e->kind == 3 ? e->n * 16 : 0;
                if (e->kind == 1 && e->draw == d) {
                    u32 i, k;
                    for (i = 0; i < e->n && nq < 40; i++, nq++) {
                        char ln[160];
                        u32 n2 = 0;
                        const u32 *w = (const u32 *)((u8 *)(e + 1) + i * 16);
                        Str(ln, &n2, "mh2-core: pass hdr step ");
                        Num(ln, &n2, sStepNo);
                        Str(ln, &n2, " q ");
                        Num(ln, &n2, nq);
                        for (k = 0; k < 4; k++) {
                            int b;
                            ln[n2++] = ' ';
                            for (b = 28; b >= 0; b -= 4)
                                ln[n2++] = "0123456789abcdef"[(w[k] >> b) & 15];
                        }
                        ln[n2] = 0;
                        Host_Log(ln);
                    }
                }
                off += 16 + bytes;
            }
        }
    if (sLog == 4)   /* the class of each draw of the step (MH2_PASS_DROPCLS's bits), with its handle, occurrence, program and form */
        for (d = 0; d < sNDraws; d++) {
            char ln[200];
            u32 n2 = 0;
            Str(ln, &n2, "mh2-core: pass cls step ");
            Num(ln, &n2, sStepNo);
            Str(ln, &n2, " h ");
            Num(ln, &n2, sDraws[d].handle);
            Str(ln, &n2, " occ ");
            Num(ln, &n2, sDIdx[d] >= 0 ? cur->d[sDIdx[d]].occ : 0xFFFFFFFFu);
            Str(ln, &n2, " prog ");
            Num(ln, &n2, sDraws[d].prog);
            Str(ln, &n2, " form ");
            Num(ln, &n2, (u64)sForm[d]);
            Str(ln, &n2, " cls ");
            Num(ln, &n2, sCls[d]);
            ln[n2] = 0;
            Host_Log(ln);
        }
    if (sP2 || sFade) {
        u64 t2 = sTime ? Ipc_TimeNs() : 0;
        Direct2D();
        if (sTime)
            sTAcc[sLaterMo ? T_L2D : T_2D] += Ipc_TimeNs() - t2;
    }
    if (viewok && !vexact)
        Unlabelled(vb);
}

static void TFlush(void)
{
    u32 k;
    if (!sTWin)
        return;
    sTAcc[T_TOTAL] = sTAcc[T_COPY] + sTAcc[T_FIND] + sTAcc[T_MODELS] + sTAcc[T_SAVELOAD] + sTAcc[T_RESEND] + sTAcc[T_SPIN] + sTAcc[T_REPEAT];
    if (sTV && sTN < TM_MAX) {
        for (k = 0; k < T_N; k++)
            sTV[k][sTN] = (u32)(sTAcc[k] / 1000ull);
        sTN++;
    }
    sTWinAll += sTWin;
    sTWin = 0;
    for (k = 0; k < T_N; k++)
        sTAcc[k] = 0;
    sSpinDone = 0;
}

static void StepEnd(u64 vblank)
{
    PSet *cs;
    sSweepStep += sSweepOn;
    if (sTime)
        TFlush();
    if (sMode <= 0 || !sSet[0].d)
        return;
    cs = Cur();
    if (!cs->have)
        return;
    if (sLog == 1) {
        char line[300];
        u32 n = 0;
        Str(line, &n, "mh2-core: pass step ");
        Num(line, &n, sStepNo);
        Str(line, &n, " ends at vblank ");
        Num(line, &n, vblank);
        Str(line, &n, " draws ");
        Num(line, &n, cs->nd);
        Str(line, &n, " qwords p1 unlabelled ");
        Num(line, &n, sStepQ[1][0]);
        Str(line, &n, " labelled ");
        Num(line, &n, sStepQ[1][1]);
        Str(line, &n, " p2 unlabelled ");
        Num(line, &n, sStepQ[2][0]);
        Str(line, &n, " labelled ");
        Num(line, &n, sStepQ[2][1]);
        Str(line, &n, " p3 ");
        Num(line, &n, sStepQ[3][0] + sStepQ[3][1]);
        line[n] = 0;
        Host_Log(line);
    }
    if (sLog == 3) {   /* a line a step: what the pass did to it (the counters' difference since the step before) */
        char line[400];
        u32 n = 0;
        static const char *nm[] = {"cam_cut", "mod_rigid", "mod_still", "mod_cut", "mod_birth", "skin_blended", "skin_still", "skin_cut", "skin_birth", "vanim", "deaths", "unl_rewritten", "cam_nomatrix"};
        const u64 now[] = {sN.cam_cut, sN.mod_rigid, sN.mod_still, sN.mod_cut, sN.mod_birth, sN.skin_blended, sN.skin_still, sN.skin_cut, sN.skin_birth, sN.vanim, sN.deaths, sN.unl_rewritten, sN.cam_nomatrix};
        static u64 was[13];
        u32 k;
        Str(line, &n, "mh2-core: pass step-summary vblank ");
        Num(line, &n, vblank);
        for (k = 0; k < 13; k++) {
            line[n++] = ' ';
            Str(line, &n, nm[k]);
            line[n++] = ' ';
            Num(line, &n, now[k] - was[k]);
            was[k] = now[k];
        }
        line[n] = 0;
        Host_Log(line);
    }
    sStepNo++;
    memset(sStepQ, 0, sizeof(sStepQ));
    if (sMode >= 3) {
        PSet *ps = Past(sBack);
        u32 i;
        if (ps->d && ps->have)
            for (i = 0; i < ps->nd; i++)
                if (!ps->d[i].used)
                    sN.deaths++;
    }
    sCurSet = (sCurSet + 1) % 3;
    cs = Cur();
    cs->nd = cs->nb = 0;
    cs->np2 = cs->nv2 = 0;
    cs->have = 0;
    memset(cs->hash, 0, SET_HASH * sizeof(u16));
    memset(sOcc, 0, sizeof(sOcc));
}

void Pass_VBlankStart(u64 vblank)
{
#ifndef MH2_PASS_COPY
    sLvCur = vblank;
    sLvIn = 1;
#else
    (void)vblank;
#endif
}

void Pass_StepEnd(u64 vblank)
{
#ifndef MH2_PASS_COPY
    if (sLive) {   /* the stamp before this step's VSync record names the plan the step's moments were made at (0: none, or not for this record) */
        if (sLVb && sLVb == (u32)(vblank + Pal_FreezeBias())) {
            Ipc_GsPlanUsed(sLVb);
            sLv[5]++;
        }
        sLFor = 0;
        sLVb = 0;
        sLNMom = 0;
        sLvIn = 0;
    }
#endif
    if (sThread) {   /* the pass thread rotates its sets in the order the steps came */
        PassJob_PushStep(vblank);
        return;
    }
    StepEnd(vblank);
}

/* ---- the pass ---- */

/* one event of the copy, sent; returns the offset of the next */
static u32 SendEv(u32 off)
{
    {
        PEv *e = (PEv *)(sEv + off);
        const u8 *d = (const u8 *)(e + 1);
        u64 r0, r1, pr;
        sOutDraw = e->draw;
        sEvIdx++;
        if (sLog == 5 && sEvIdx >= sDropE0 && sEvIdx < sDropE1) {   /* (development) the events of the range: kind, size, and the words of the first qwords */
            char ln[600];
            u32 n2 = 0, k, nw = (e->kind == 2 ? 2u : e->n * 4u);
            static const char hx[] = "0123456789abcdef";
            Str(ln, &n2, "mh2-core: pass ev step ");
            Num(ln, &n2, sStepNo);
            Str(ln, &n2, " idx ");
            Num(ln, &n2, sEvIdx);
            Str(ln, &n2, " kind ");
            Num(ln, &n2, e->kind);
            Str(ln, &n2, " n ");
            Num(ln, &n2, e->n);
            Str(ln, &n2, " draw ");
            Num(ln, &n2, e->draw);
            Str(ln, &n2, " w");
            for (k = 0; k < nw && k < 48; k++) {
                u32 x = ((const u32 *)d)[k], b2;
                ln[n2++] = ' ';
                for (b2 = 0; b2 < 8; b2++)
                    ln[n2++] = hx[(x >> (28 - 4 * b2)) & 15];
            }
            ln[n2] = 0;
            Host_Log(ln);
        }
        vu1_stats(&r0, &pr);
        switch (e->kind) {
        case 1:
            vif_write(1, d, e->n);
            off += 16 + e->n * 16;
            break;
        case 2: {
            u64 hi;
            memcpy(&hi, d, 8);
            vif_write_tag(1, hi);
            off += 32;
            break;
        }
        case 3:
            gif_path_write(3, d, e->n);
            off += 16 + e->n * 16;
            break;
        default:
            return sEvLen;
        }
        vu1_stats(&r1, &pr);   /* the VU1 programs started by this piece: of a labelled draw, or not */
        if (e->draw == 0xFFFFFFFFu)
            sN.kicks_unl += r1 - r0;
        else
            sN.kicks_lab += r1 - r0;
    }
    return off;
}

static const char *sHx = "0123456789abcdef";

static void LogFlip(u32 d, int ok, int kept)
{
    char ln[500];
    u32 n = 0, k;
    const u64 v[5] = {sWk.attr, sWk.alpha, sWk.test, sWk.zbuf, sWk.tex0};
    Str(ln, &n, "mh2-core: pass flip step ");
    Num(ln, &n, sStepNo);
    Str(ln, &n, " h ");
    Num(ln, &n, sDraws[d].handle);
    Str(ln, &n, " prog ");
    Num(ln, &n, sDraws[d].prog);
    Str(ln, &n, " q ");
    Num(ln, &n, (u64)sFlipQ[d]);
    Str(ln, &n, " ok ");
    Num(ln, &n, (u64)ok);
    Str(ln, &n, " kept ");
    Num(ln, &n, (u64)kept);
    Str(ln, &n, " cls ");
    Num(ln, &n, sWk.cls);
    Str(ln, &n, " why ");
    Num(ln, &n, sWk.hold ? sWk.why : 0);
    Str(ln, &n, " colours ");
    Num(ln, &n, sWk.rgbaq);
    Str(ln, &n, " held ");
    Num(ln, &n, sWk.hold);
    Str(ln, &n, " kicks ");
    Num(ln, &n, sWk.kicks);
    Str(ln, &n, " items ");
    Num(ln, &n, sCapN);
    Str(ln, &n, " over ");
    Num(ln, &n, (u64)sCapOver);
    Str(ln, &n, " attr/alpha/test/zbuf/tex0 ");
    for (k = 0; k < 5; k++) {
        u32 b2;
        for (b2 = 0; b2 < 16 && (v[k] >> (60 - 4 * b2)) == 0 && b2 < 15; b2++)
            ;
        for (; b2 < 16; b2++)
            ln[n++] = sHx[(v[k] >> (60 - 4 * b2)) & 15];
        ln[n++] = ' ';
    }
    Str(ln, &n, "old ");
    {
        u32 f0, f1;
        memcpy(&f0, &sFlipOld[d][0], 4);
        memcpy(&f1, &sFlipOld[d][1], 4);
        Num(ln, &n, f0);
        ln[n++] = ' ';
        Num(ln, &n, f1);
    }
    Str(ln, &n, " new ");
    {
        u32 f0, f1;
        memcpy(&f0, sFlipP[d], 4);
        memcpy(&f1, sFlipP[d] + 1, 4);
        Num(ln, &n, f0);
        ln[n++] = ' ';
        Num(ln, &n, f1);
    }
    ln[n] = 0;
    Host_Log(ln);
}

/* A flipbook draw is sent twice: with the step before's offset at the weight 1 - a, and with this step's at a. The first is held (Capture) until its kicks are known to
 * be of a class that the weight can scale; if one is not, it is dropped and the draw is sent once, as the step's own. */
static u32 SendFlip(u32 off)
{
    u32 d = ((PEv *)(sEv + off))->draw, end = off, k, wa = (u32)(sAlpha * 256.0 + 0.5), w1, w2;
    float cur[2];
    int ok, p3 = 0;
    while (end < sEvLen && ((PEv *)(sEv + end))->draw == d) {
        PEv *e = (PEv *)(sEv + end);
        if (e->kind == 3)
            p3 = 1;
        end += 16 + (e->kind == 2 ? 16u : e->n * 16u);
    }
    if (wa > 256)
        wa = 256;
    w2 = wa;
    w1 = 256 - wa;
    if (p3) {
        sN.flip_p3++;
        for (k = off; k < end;)
            k = SendEv(k);
        return end;
    }
    Save(2);
    sTwice = 1;
    memcpy(cur, sFlipP[d], 8);
    memcpy(sFlipP[d], sFlipOld[d], 8);
    sCapOn = 1;
    sCapN = sCapQ = 0;
    sCapOver = 0;
    for (k = off; k < end;)
        k = SendEv(k);
    sCapOn = 0;
    memcpy(sFlipP[d], cur, 8);
    {
        GsSt t = sOS;
        memset(&sWk, 0, sizeof(sWk));
        for (k = 0; k < sCapN; k++)
            Walk(&t, sCapBuf + (size_t)sCapIt[k].off * 16, 0, sCapIt[k].q, 256);
        ok = !sCapOver && sWk.rgbaq > 0 && !sWk.hold;
    }
    if (sLog == 6)
        LogFlip(d, ok, ok && sFlipMode == 1);
    Load(2);
    if (ok && sFlipMode == 1) {
        for (k = 0; k < sCapN; k++) {
            u8 *it = sCapBuf + (size_t)sCapIt[k].off * 16;
            Walk(&sOS, it, w1 < 256 ? it : 0, sCapIt[k].q, w1);
            Emit((u32)sCapIt[k].path, it, sCapIt[k].q);
        }
        sWgt = w2;
        sN.flip_draws++;
    } else if (!ok) {
        u32 why = sCapOver ? 8u : sWk.rgbaq ? sWk.why : 9u;
        sN.flip_held[why > 9 ? 9 : why]++;
    }
    for (k = off; k < end;)
        k = SendEv(k);
    sWgt = 256;
    sTwice = 0;
    return end;
}

/* MH2_PASS_CENSUS: the rows of the piece that Resend sends next: its kind, and its draw's call site and microprogram when it has a label */
static void CzPick(const PEv *e, int flip)
{
    u32 r, d = e->draw;
    if (flip)
        r = CZ_FLIP;
    else if (e->kind == 2)
        r = CZ_TAG;
    else if (e->kind == 3)
        r = CZ_PATH3;
    else if (d == 0xFFFFFFFFu)
        r = CZ_UNL;
    else if (d >= MAX_DRAWS || d >= sNDraws)
        r = CZ_OTHER;
    else
        switch (sCls[d]) {
        case 1: r = CZ_MOVED; break;
        case 2: r = CZ_VANIM; break;
        case 4: r = CZ_STILL; break;
        case 8: r = CZ_NOMTX; break;
        case 16: r = CZ_NOPART; break;
        case 32: r = CZ_CUT; break;
        case 64: r = CZ_NOBONE; break;
        case 128: r = CZ_NOHANDLE; break;
        default: r = CZ_OTHER; break;
        }
    sCzRow = r;
    sCzCur = &sCz[r][e->pad & 1u];
    sCzCurSite = 0;
    if (d < MAX_DRAWS && d < sNDraws) {
        u32 site = sDraws[d].site, prog = sDraws[d].prog, h = (site * 2654435761u ^ prog * 40503u) % CZ_SITES, k;
        for (k = 0; k < CZ_SITES; k++, h = (h + 1) % CZ_SITES) {
            if (!sCzSiteUsed[h]) {
                if (sCzSiteN + 1 >= CZ_SITES) {
                    k = CZ_SITES;
                    break;
                }
                sCzSiteUsed[h] = 1;
                sCzSiteKey[h][0] = site;
                sCzSiteKey[h][1] = prog;
                sCzSiteN++;
                break;
            }
            if (sCzSiteKey[h][0] == site && sCzSiteKey[h][1] == prog)
                break;
        }
        sCzCurSite = k < CZ_SITES ? &sCzSite[h] : &sCzSite[CZ_SITES];
    }
}

static void CzAdd(CzAcc *a, u64 ticks, u32 qw, u64 runs)
{
    a->ticks += ticks;
    a->pieces++;
    a->qw += qw;
    a->runs += runs;
}

static void Resend(void)
{
    u32 off = 0;
    u64 ct = 0;
    sEvIdx = 0;
    sOS = sWS;
    sWgt = 256;
    if (sCensus)
        ct = __builtin_ia32_rdtsc();
    while (off < sEvLen) {
        PEv *e = (PEv *)(sEv + off);
        int flip = sMode >= 3 && sFlipMode && e->draw < MAX_DRAWS && sFlipQ[e->draw] >= 0;
        u32 o0 = off;
        u64 r0 = 0, r1, pr;
        if (sCensus) {
            CzPick(e, flip);
            vu1_stats(&r0, &pr);
        }
        if (flip)
            off = SendFlip(off);
        else
            off = SendEv(off);
        if (sCensus) {
            u64 t1 = __builtin_ia32_rdtsc(), dt = t1 - ct;
            vu1_stats(&r1, &pr);
            if (sCensus == 2 && sCzRow == CZ_MOVED)   /* the control: this row's time is not entered */
                dt = 0;
            CzAdd(sCzCur, dt, (off - o0) / 16, r1 - r0);
            if (sCzCurSite)
                CzAdd(sCzCurSite, dt, (off - o0) / 16, r1 - r0);
            ct = t1;
        }
    }
    if (sCensus) {
        sCzCur = sCzCurSite = 0;
        sCzMoments++;
    }
}

/* MH2_PASS_ALPHAS=sweep: the pictures a 165 Hz display makes in a step of the game's 30 a second, 5.5 of them, the display's phase against the step drifting from step to step
 * as an unlocked clock's does (the step's number times the golden ratio): picture k at (k + u) / 5.5 of the step, u in [0, 1). The ones under 8/256 and from 256/256 are not asked for;
 * every alpha from 8 to 255 comes up over the steps. Five or six moments a step; the same for every window of a step. */
static void SweepSet(void)
{
    u32 u = (u32)((sSweepStep * 40503ull) & 0xFFFFu), k;
    sNMom = 0;
    for (k = 0; k < MOM_MAX; k++) {
        u32 a = (u32)((((u64)k * 65536ull + u) * 512ull + 11ull * 32768ull) / (11ull * 65536ull));
        if (a >= 8 && a <= 255)
            sMomA[sNMom++] = a;
    }
    if (!sNMom)
        sNMom = 1, sMomA[0] = 128;
}

static void MPut(u32 k, u64 ns)
{
#ifdef MH2_PASS_HELPER
    PassHelp_MomPut(k, ns);   /* the rows are the deciding copy's */
#else
    if (sMV && k < MV_N && sMVn[k] < TM_MAX)
        sMV[k][sMVn[k]++] = (u32)(ns / 1000ull);
#endif
}

/* The package of a moment (MH2_PASS_DEFER; plan 11, 5.2): what a moment's Resend reads of what its Models made, kept aside: the copy after Models, sFlipQ, sFlipP as an offset
 * into the copy, sFlipOld, sCls, the alpha, the skip flag, the moment's number and count. A later moment's Resend gets these and nothing of the sets (Cur and Past are fatal
 * while it runs); the units' snapshot and the output stage's start are the window's own and read only. */
/* (MomPkg is in gfx_pass_job.h: the helpers take the same package.) */
static MomPkg *sPkg[MOM_MAX];
static double sStaleAlpha;   /* DEFER 2: the last package's alpha */

static MomPkg *PkgNew(void)
{
    MomPkg *k = Host_Alloc(sizeof(MomPkg));
    if (!k)
        { Host_Log("mh2-core: pass: no memory for a package"); __builtin_trap(); }
    k->ev = 0;
    k->cap = 0;
    k->job = 0;
    return k;
}

/* MH2_PASS_CENSUS: the pieces of a draw any of whose bytes are not the window's own (this moment's Models wrote them) are marked in the moment's copy, and so is such a piece outside the draws */
static void CzMark(u8 *ev, const u8 *keep)
{
    u32 off, pass;
    memset(sCzWr, 0, sNDraws < MAX_DRAWS ? sNDraws : MAX_DRAWS);
    for (pass = 0; pass < 2; pass++)
        for (off = 0; off + 16 <= sEvLen;) {
            PEv *e = (PEv *)(ev + off);
            u32 len = e->kind == 2 ? 16u : e->kind == 1 || e->kind == 3 ? e->n * 16u : 0u;
            if (e->kind < 1 || e->kind > 3 || off + 16 + len > sEvLen)
                break;
            if (!pass) {
                e->pad = memcmp(ev + off + 16, keep + off + 16, len) ? 1u : 0u;
                if (e->pad && e->draw < MAX_DRAWS)
                    sCzWr[e->draw] = 1;
            } else if (e->draw < MAX_DRAWS && sCzWr[e->draw])
                e->pad = 1;
            off += 16 + len;
        }
}

static void PkgFill(MomPkg *k, u32 mo)
{
    u32 d;
    if (sEvLen > k->cap) {   /* grown as a window needs: the copy's own bytes, a window's length */
        u32 nc = sEvLen + (sEvLen >> 2) + 4096u;
        u8 *nb = Host_Alloc(nc);
        if (!nb)
            { Host_Log("mh2-core: pass: no memory for a package's copy"); __builtin_trap(); }
        if (k->ev)
            Host_Free(k->ev);
        k->ev = nb;
        k->cap = nc;
    }
    memcpy(k->ev, sEv, sEvLen);
    if (sCensus && sKeep)
        CzMark(k->ev, sKeep);
    k->models = 0;
    k->evLen = sEvLen;
    k->nDraws = sNDraws;
    k->mo = mo;
    k->nm = sNMom;
    for (d = 0; d < sNDraws; d++) {
        k->flipQ[d] = sFlipQ[d];
        k->flipOff[d] = sFlipQ[d] >= 0 ? (u32)((u8 *)sFlipP[d] - sEv) : 0;
        k->flipOld[d][0] = sFlipOld[d][0];
        k->flipOld[d][1] = sFlipOld[d][1];
        k->cls[d] = sCls[d];
    }
    k->alpha = sAlpha;
    k->skip = sSkipMom;
}

static void PkgSave(u32 mo)
{
    if (!sPkg[mo])
        sPkg[mo] = PkgNew();
    PkgFill(sPkg[mo], mo);
    sStaleAlpha = sAlpha;
}

static void PkgApply(MomPkg *k, u32 mo)
{
    u32 d;
    if (k->evLen != sEvLen || k->nDraws != sNDraws)
        { Host_Log("mh2-core: pass: a package of another window"); __builtin_trap(); }
    if (!(sDefer == 3 && mo == 0))   /* DEFER 3: moment 0's Resend on the last moment's copy */
        memcpy(sEv, k->ev, sEvLen);
    for (d = 0; d < sNDraws; d++) {
        sFlipQ[d] = k->flipQ[d];
        sFlipP[d] = k->flipQ[d] >= 0 ? (float *)(sEv + k->flipOff[d]) : 0;
        sFlipOld[d][0] = k->flipOld[d][0];
        sFlipOld[d][1] = k->flipOld[d][1];
        sCls[d] = k->cls[d];
    }
    sAlpha = k->alpha;
    sSkipMom = k->skip;
}

static void PkgLoad(u32 mo)
{
    PkgApply(sPkg[mo], mo);
}

/* MH2_PASS_HMODELS (development; with helpers): what a later moment's Models reads, kept in the window's slot so that the helper that runs the moment's Resend makes its Models too, and
 * the deciding copy goes on to the next window as soon as the first moment's is made. The deciding copy makes it for each window: the copy as the window left it (its kept image is the
 * slot's, not the one buffer), the first moment's list of the matrix blends' decisions (recorded into the slot's own list), where the finding put each draw's matrix, each draw's form and its
 * entry in the current set, the current set's header and the sets the pairing reads. A helper reads all of it and writes none of it: its Models writes its own copy of the window and its own
 * room for the 2D packets, and nothing of a set (the first moment's Models has made the step's records). The sets stand still for it: the deciding copy only adds the next windows' draws to
 * the current set, and its step's end (which turns the sets and clears one) waits until every helper's Models of the step is made (PassHelp_ModelsWait). */
typedef struct WinShare {
    u8 *keep;                    /* the copy as the window left it */
    u32 keepCap;
    PSet cur;                    /* the current set's header when the window's first moment was made (its pointers are the deciding copy's set) */
    PSet *past, *past1, *other;  /* Past(sBack) and Past(1) of that time; the third set (the control's) */
    MatP *tape;                  /* the first moment's decisions, in the order Models asks for them */
    u32 tapeN;
    int tapeOn;
    u32 fOff[MAX_DRAWS];         /* sF as an offset into the copy, or 0xFFFFFFFF */
    int form[MAX_DRAWS], dIdx[MAX_DRAWS];
    u32 dLo[MAX_DRAWS], dHi[MAX_DRAWS];   /* MH2_PASS_DRAWIDX: each draw's span of the copy (sDLo, sDHi) */
    u64 dh[MAX_DRAWS];           /* MH2_PASS_HASHONCE: the first moment's hashes */
    D2Pl d2[8192];               /* MH2_PASS_D2IDX: the 2D walk's places (D2_MAX of them) */
    u32 d2N;
    int d2Ok;
} WinShare;

#ifdef MH2_PASS_HELPER
static P2 *sHP2;                 /* this helper's room for the 2D packets of the window its Models looks at */
static u32 *sHP2v;
static u64 sHModN;

/* A helper: the later moment's Models, from the window's share into this helper's copy. What a Resend reads of a Models (sFlipQ, sFlipP, sFlipOld, sCls, the skip flag, the alpha) is left
 * in this copy's own variables, as PkgApply leaves a package's. */
static void ShareModels(MomPkg *k)
{
    WinShare *ws = k->job->share;
    u32 d;
    if (!ws || !ws->keep || k->evLen > ws->keepCap)
        Host_Fatal("pass helper: a moment's Models without the window's share");
    if (!sHP2) {
        sHP2 = Host_Alloc(P2_MAX * sizeof(P2));
        sHP2v = Host_Alloc(P2V_MAX * sizeof(u32));
        if (!sHP2 || !sHP2v)
            Host_Fatal("pass helper: no memory for the 2D packets");
    }
    memcpy(sEv, ws->keep, k->evLen);
    for (d = 0; d < k->nDraws; d++) {
        sF[d] = ws->fOff[d] != 0xFFFFFFFFu ? (float *)(sEv + ws->fOff[d]) : 0;
        sForm[d] = ws->form[d];
        sDIdx[d] = ws->dIdx[d];
        sFlipQ[d] = -1;
    }
    if (sDIx)
        for (d = 0; d < k->nDraws; d++) {
            sDLo[d] = ws->dLo[d];
            sDHi[d] = ws->dHi[d];
        }
    if (sDHo)
        memcpy(sDH1, ws->dh, k->nDraws * sizeof(u64));
    if (sD2x && D2Room()) {
        sD2N = ws->d2N;
        sD2Ok = ws->d2Ok;
        if (sD2N)
            memcpy(sD2, ws->d2, sD2N * sizeof(D2Pl));
    }
    sShCur = ws->cur;
    sShCur.p2 = ws->cur.p2 ? sHP2 : 0;
    sShCur.p2v = sHP2v;
    sShCur.np2 = sShCur.nv2 = 0;
    sShPast = ws->past;
    sShPast1 = ws->past1;
    sHModN++;
    if (sHModCtl == 1 && (sHModN % 50u) == 0)   /* the control: this Models pairs with the set of another step (the one a turn of the sets would have put there) */
        sShPast = ws->other;
    sAlpha = k->alpha;
    sTape = ws->tape;
    sTapeN = ws->tapeN;
    sTapeI = 0;
    sTapeMode = ws->tapeOn ? 2 : 0;
    sRecord = 0;
    sLaterMo = 1;
    sShared = 1;
    Models();
    sShared = 0;
    sLaterMo = 0;
    sTapeMode = 0;
    sRecord = 1;
}
#endif

#if defined(MH2_PASS_COPY) && !defined(MH2_PASS_HELPER)
static PassJob *sRunJob;   /* the slot of the window this copy runs */

/* The deciding copy, before a window's pass: the slot's share is there, its kept image is long enough, and this window's kept image and list of decisions are the slot's. */
static void ShareSetup(PassJob *j)
{
    WinShare *ws = j->share;
    if (!ws) {
        ws = Host_Alloc(sizeof(WinShare));
        if (ws && sTapeOn)
            ws->tape = Host_Alloc(TAPE_MAX * sizeof(MatP));
        if (!ws || (sTapeOn && !ws->tape))
            Host_Fatal("pass: no memory for a window's share");
        j->share = ws;
    }
    if (j->evLen > ws->keepCap || !ws->keep) {
        u32 nc = j->evLen + (j->evLen >> 2) + 4096u;
        u8 *nb = Host_Alloc(nc);
        if (!nb)
            Host_Fatal("pass: no memory for a window's kept image");
        if (ws->keep)
            Host_Free(ws->keep);
        ws->keep = nb;
        ws->keepCap = nc;
    }
    sKeep = ws->keep;
    if (sTapeOn)
        sTape = ws->tape;
}

/* ... and after the first moment's Models: what the helpers' Models read. */
static void ShareFill(PassJob *j)
{
    WinShare *ws = j->share;
    u32 d;
    ws->cur = *Cur();
    ws->past = Past(sBack);
    ws->past1 = Past(1);
    ws->other = Past(sBack == 1 ? 2 : 1);
    ws->tapeN = sTapeN;
    ws->tapeOn = sTapeOn;
    for (d = 0; d < sNDraws; d++) {
        ws->fOff[d] = sF[d] ? (u32)((u8 *)sF[d] - sEv) : 0xFFFFFFFFu;
        ws->form[d] = sForm[d];
        ws->dIdx[d] = sDIdx[d];
    }
    if (sDIx)
        for (d = 0; d < sNDraws; d++) {
            ws->dLo[d] = sDLo[d];
            ws->dHi[d] = sDHi[d];
        }
    if (sDHo)
        memcpy(ws->dh, sDCtl == 2 ? sDHOld : sDH1, sNDraws * sizeof(u64));
    ws->d2Ok = sD2x && sD2Ok;
    ws->d2N = ws->d2Ok ? sD2N : 0;
    if (ws->d2N)
        memcpy(ws->d2, sD2, ws->d2N * sizeof(D2Pl));
}
static int sHanded;        /* the window's moments went to the helpers: the last of them sends "all sent" and gives the slot back */

/* The helpers' way (MH2_PASS_HELPERS=n): each moment's Models in turn as in the DEFER path, but each moment's package is queued for a helper at once, and the Resends are the helpers'. The
 * window's slot goes with the packages (its countdown is the number of moments); this copy does not touch it again. */
static void HelpWindow(void)
{
    PassJob *j = sRunJob;
    u32 mo;
    int hm = sHMod && j->share && sNMom > 1 && sKeep == ((WinShare *)j->share)->keep;   /* MH2_PASS_HMODELS: the later moments' Models are the helpers' */
    sHanded = 1;
    __atomic_store_n(&j->left, (int)sNMom, __ATOMIC_RELEASE);
    if (hm) {
        ShareFill(j);
        PassHelp_ModelsAdd(sNMom - 1);
    }
    if (sCensus) {   /* from the window's end on the game's thread to its first package: the queue, the copy, the finding and moment 0's Models */
        sCzDecWin++;
        if (j->tEnd)
            sCzDecHeadNs += Ipc_TimeNs() - j->tEnd;
    }
    for (mo = 0; mo < sNMom; mo++) {
        MomPkg *k;
        u64 c0 = sCensus ? __builtin_ia32_rdtsc() : 0, c1 = c0, c2 = c0;
        if (mo && hm) {   /* the package names the moment; the helper makes the rest from the window's share */
            k = PassHelp_Get();
            if (sCensus)
                c2 = __builtin_ia32_rdtsc();   /* (no Models here: the wait for a free package alone) */
            k->models = 1;
            k->evLen = sEvLen;
            k->nDraws = sNDraws;
            k->mo = mo;
            k->nm = sNMom;
            k->alpha = (double)sMomA[mo] / 256.0;
            k->skip = 0;
        } else if (mo) {
            __typeof__(sN) keep = sN;
            u64 keepq[4][2];
            memcpy(keepq, sStepQ, sizeof(keepq));
            if (!sMomCtl)
                memcpy(sEv, sKeep, sEvLen);
            sAlpha = (double)sMomA[mo] / 256.0;
            sRecord = 0;
            sTapeMode = sTapeOn ? 2 : 0;
            sTapeI = 0;
            sLaterMo = 1;
            Models();
            sLaterMo = 0;
            sTapeMode = 0;
            sRecord = 1;
            if (sCensus)
                c1 = __builtin_ia32_rdtsc();
            k = PassHelp_Get();
            if (sCensus)
                c2 = __builtin_ia32_rdtsc();
            PkgFill(k, mo);
            sN = keep;
            memcpy(sStepQ, keepq, sizeof(keepq));
        } else {
            k = PassHelp_Get();
            if (sCensus)
                c2 = __builtin_ia32_rdtsc();
            PkgFill(k, 0);
        }
        k->job = j;
        k->win = (u32)sN.windows;
        PassHelp_Submit(k);
        if (sCensus && mo < 6) {
            sCzDec[mo][0] += c1 - c0;
            sCzDec[mo][1] += c2 - c1;
            sCzDec[mo][2] += __builtin_ia32_rdtsc() - c2;
            sCzDec[mo][3]++;
        }
    }
    sN.run++;
}
#endif

static void WindowRun_(void)
{
    int changed = 0, same, still = 0;
    u32 i, mo;
    u64 tw = 0, rs1 = 0, rsl = 0;
    sSweepOn = 1;
    if (sSweep)
        SweepSet();
    if (sLive) {   /* the moments are the plan's: the job's (the pass thread) or the game's thread's decision */
#if defined(MH2_PASS_COPY) && !defined(MH2_PASS_HELPER)
        sNMom = sRunJob->nMom;
        for (i = 0; i < MOM_MAX; i++)
            sMomA[i] = sRunJob->momA[i];
#else
        sNMom = sLNMom;
        for (i = 0; i < MOM_MAX; i++)
            sMomA[i] = sLA[i];
#endif
        sLSkip = !sNMom;   /* no moment for this step: the window's Models run (the sets go on, so that the next step has its partners) and its Resend does not; the step is not made from the pass */
        if (sLSkip) {
            sNMom = 1;
            sMomA[0] = 128;
        }
        sAlpha = (double)sMomA[0] / 256.0;
    }
    sTWin++;
    sN.windows++;
    sN.bytes += sEvLen;
    if (sEvLen > sN.max_ev)
        sN.max_ev = sEvLen;
    if (sRealLen > sN.max_real)
        sN.max_real = sRealLen;
    sN.draws += sNDraws;
    if (sSpinMs && !sSpinDone) {   /* the control: a busy wait, once a step */
        u64 t0 = Ipc_TimeNs();
        sSpinDone = 1;
        while (Ipc_TimeNs() - t0 < (u64)sSpinMs * 1000000ull)
            ;
        sTAcc[T_SPIN] += Ipc_TimeNs() - t0;
    }
    if (sOverflow) {
        sN.skipped_over++;
        if (sMode >= 2)
            PassSend(5, 0, 0, 0);
        return;
    }
    if (sDirty) {
        sN.skipped_dirty++;
        if (sMode >= 2)
            PassSend(5, 0, 0, 0);
        return;
    }
    tw = sTime ? Ipc_TimeNs() : 0;
    if (sDIx)
        IndexDraws();
    sD2Ok = 0;
    for (i = 0; i < sNDraws; i++) {
        sFlipQ[i] = -1;
        sF[i] = FindMatrix(i, &sForm[i]);
        if (sF[i])
            sN.found++;
    }
    if (sTime) {
        u64 t1 = Ipc_TimeNs();
        sTAcc[T_FIND] += t1 - tw;
        tw = t1;
    }
    if (sNMom > 1 || sStill || (sCensus && sKeep))
        memcpy(sKeep, sEv, sEvLen);
    if (sNMom > 1)
        sAlpha = (double)sMomA[0] / 256.0;
    sTapeMode = sTapeOn ? 1 : 0;
    sTapeN = sTapeI = 0;
    if (sMode >= 3)
        Models();
    if (sLive && sLSkip) {
        sLSkip = 0;
        sTapeMode = 0;
        if (sTime)
            sTAcc[T_MODELS] += Ipc_TimeNs() - tw;
        if (sMode >= 2)
            PassSend(5, 0, 0, 0);
        sN.run++;
        return;
    }
    if (sStill)
        still = !memcmp(sKeep, sEv, sEvLen);
    if (sTime) {
        u64 t1 = Ipc_TimeNs();
        sTAcc[T_MODELS] += t1 - tw;
        tw = t1;
    }
    sTapeMode = 0;
#if defined(MH2_PASS_COPY) && !defined(MH2_PASS_HELPER)
    if (sMode >= 3 && !sCtl && PassHelp_On()) {   /* the moments' Resends go to the helpers (MH2_PASS_HELPERS) */
        HelpWindow();
        return;
    }
#endif
    if (sDefer && sMode >= 3 && sNMom > 1) {   /* every moment's Models first, each into a package; then moment 0's package back, and the Resends from the packages */
        u64 td = sTime ? Ipc_TimeNs() : 0;
        PkgSave(0);
        for (mo = 1; mo < sNMom; mo++) {
            __typeof__(sN) keep = sN;
            u64 keepq[4][2];
            memcpy(keepq, sStepQ, sizeof(keepq));
            if (!sMomCtl)
                memcpy(sEv, sKeep, sEvLen);
            sAlpha = (double)sMomA[mo] / 256.0;
            sRecord = 0;
            sTapeMode = sTapeOn ? 2 : 0;
            sTapeI = 0;
            sLaterMo = 1;
            Models();
            sLaterMo = 0;
            sTapeMode = 0;
            sRecord = 1;
            PkgSave(mo);
            sN = keep;
            memcpy(sStepQ, keepq, sizeof(keepq));
        }
        PkgLoad(0);
        sNoSets = 1;
        if (sTime) {
            u64 t1 = Ipc_TimeNs();
            sTAcc[T_LMODELS] += t1 - td;
            sTAcc[T_REPEAT] += t1 - td;
            tw = t1;
        }
    }
    if (sMode >= 2)
        PassSend(2, sMode >= 3 ? (u32)(sAlpha * 256.0 + 0.5) | (sSkipMom ? 0x8000u : 0u) | (sNMom > 1 ? (sNMom << 24) : 0u) : 256u, 0, 0);
    if (sCtl)   /* the control: one entry of one draw's matrix, changed in the copy */
        for (i = 0; i < sNDraws && !changed; i++)
            if (sF[i]) {
                sF[i][0] = sF[i][0] * 1.25f + 0.5f;
                changed = 1;
                sN.ctl_changed++;
            }
    if (sDefer == 2 && sMode >= 3 && sNMom > 1)
        sAlpha = sStaleAlpha;   /* the control: the output stage works with the last moment's alpha (the begin record kept its own) */
    sBirthW = sAlpha >= 1.0 ? 256u : (u32)(sAlpha * 256.0 + 0.5);
    tw = sTime ? Ipc_TimeNs() : 0;
    if (!PASS_COPY)
        Save(1);                   /* what the real window left (on the pass thread: the game's thread took it) */
    Load(0);                       /* what it started from */
    gif_sandbox(1, sSb);
    if (sTime) {
        u64 t1 = Ipc_TimeNs();
        sTAcc[T_SAVELOAD] += t1 - tw;
        tw = t1;
    }
    sRealCur = 0;
    sDiff = 0;
    gGfxPassRun = 1;
    Resend();
    gGfxPassRun = 0;
    if (sMode >= 2)
        PassSend(4, 0, 0, 0);   /* (the end of a moment: on the pass thread this puts the moment's record to the ring, and its time is Resend's) */
    if (sTime) {
        u64 t1 = Ipc_TimeNs();
        sTAcc[T_RESEND] += t1 - tw;
        rs1 = t1 - tw;
        tw = t1;
    }
    if (sTime && sWinEnd)
        MPut(MV_MOM0, Ipc_TimeNs() - sWinEnd);
    for (mo = 1; mo < sNMom; mo++) {   /* the moments after the first: the copy put back, Models and the records of the step left alone, the units put back, Resend */
        __typeof__(sN) keep = sN;
        u64 keepq[4][2];
        u64 t0 = sTime ? Ipc_TimeNs() : 0;
        memcpy(keepq, sStepQ, sizeof(keepq));
        u64 tl = sTime ? Ipc_TimeNs() : 0;
        if (sDefer && sMode >= 3) {   /* the package of its Models, made before the Resends */
            PkgLoad(mo);
        } else {
            if (!sMomCtl)
                memcpy(sEv, sKeep, sEvLen);
            sAlpha = (double)sMomA[mo] / 256.0;
            tl = sTime ? Ipc_TimeNs() : 0;
            sRecord = 0;
            sTapeMode = sTapeOn ? 2 : 0;
            sTapeI = 0;
            sLaterMo = 1;
            Models();
            sLaterMo = 0;
            sTapeMode = 0;
            sRecord = 1;
        }
        if (sTime) {
            u64 t1 = Ipc_TimeNs();
            sTAcc[T_LMODELS] += t1 - tl;
            tl = t1;
        }
        PassSend(2, (u32)(sAlpha * 256.0 + 0.5) | (sSkipMom ? 0x8000u : 0u) | (mo << 16) | (sNMom << 24), 0, 0);
        if (sDefer == 2 && sMode >= 3)
            sAlpha = sStaleAlpha;   /* the control (the begin record kept its own alpha) */
        sBirthW = sAlpha >= 1.0 ? 256u : (u32)(sAlpha * 256.0 + 0.5);
        gif_sandbox(0, 0);
        gif_sandbox_clear();
        Load(0);
        gif_sandbox(1, sSb);
        if (sTime) {
            u64 t1 = Ipc_TimeNs();
            sTAcc[T_LLOAD] += t1 - tl;
            tl = t1;
        }
        gGfxPassRun = 1;
        Resend();
        gGfxPassRun = 0;
        PassSend(4, 0, 0, 0);
        if (sTime) {
            u64 t1 = Ipc_TimeNs();
            sTAcc[T_LRESEND] += t1 - tl;
            rsl += t1 - tl;
        }
        if (sTime && sWinEnd)
            MPut(MV_MOM0 + mo, Ipc_TimeNs() - sWinEnd);
        sN = keep;
        memcpy(sStepQ, keepq, sizeof(keepq));
        if (sTime)
            sTAcc[T_REPEAT] += Ipc_TimeNs() - t0;
    }
    sNoSets = 0;
    if (sStill && sTime) {
        if (still)
            sStillWins++;
        else
            sChgWins++;
        MPut(still ? MV_RS_STILL : MV_RS_CHG, rs1);
        if (sNMom > 1)
            MPut(still ? MV_LRS_STILL : MV_LRS_CHG, rsl);
    }
    if (sTime)
        tw = Ipc_TimeNs();   /* the later moments are T_REPEAT's: the saves and loads below start here (they once held the moments' time too) */
    if (sMode >= 3 && sRepeat > 1) {   /* the cost of a moment after the first: the copy put back, Resend again, the stream thrown away (the counters are put back) */
        __typeof__(sN) keep = sN;
        u64 keepq[4][2];
        int r;
        memcpy(keepq, sStepQ, sizeof(keepq));
        sQuiet = 1;
        for (r = 1; r < sRepeat; r++) {
            u64 t0 = Ipc_TimeNs();
            gif_sandbox(0, 0);
            gif_sandbox_clear();
            Load(0);
            gif_sandbox(1, sSb);
            gGfxPassRun = 1;
            Resend();
            gGfxPassRun = 0;
            sTAcc[T_REPEAT] += Ipc_TimeNs() - t0;
        }
        sQuiet = 0;
        sN = keep;
        memcpy(sStepQ, keepq, sizeof(keepq));
        tw = Ipc_TimeNs();
    }
    if (sRealCur != sRealLen)
        sDiff = 1, sDiffAt = sRealCur;
    same = gif_sandbox_same();
    gif_sandbox(0, 0);
    gif_sandbox_clear();
    {   /* the units the pass leaves are the real window's, to the byte */
        int end_same = !memcmp(g_vu_mem, sVu1, sizeof(g_vu_mem));
        vu1_state(sVut, 0);
        end_same = end_same && !memcmp(sVut, sVus1, vu1_state(0, 0));
        vif_state(sVift, 0);
        end_same = end_same && !memcmp(sVift, sVif1, vif_state(0, 0));
        if ((!end_same || !same) && sMode < 3)
            sN.end_differ++;
    }
    if (!PASS_COPY)
        Load(1);                   /* and put back */
    if (sTime)
        sTAcc[T_SAVELOAD] += Ipc_TimeNs() - tw;
    sN.run++;
    if (changed) {
        if (sDiff)
            sN.ctl_differ++;
    } else if (sDiff && sMode < 3) {
        char line[120];
        u32 n = 0;
        sN.differ++;
        if (sN.differ <= 8) {
            Str(line, &n, "mh2-core: pass: window ");
            Num(line, &n, sN.windows);
            Str(line, &n, " differs at byte ");
            Num(line, &n, sDiffAt);
            Str(line, &n, " of ");
            Num(line, &n, sRealLen);
            Str(line, &n, " at event ");
            Num(line, &n, sEvIdx);
            Str(line, &n, " draw ");
            Num(line, &n, sOutDraw == 0xFFFFFFFFu ? 99999u : sOutDraw < MAX_DRAWS ? sDraws[sOutDraw].handle : 88888u);
            line[n] = 0;
            Host_Log(line);
        }
    }
}

/* A window's whole pass, timed from outside so that the parts' times have a sum to be checked against (T_WALL). */
static void WindowRun(void)
{
    u64 t0 = sTime ? Ipc_TimeNs() : 0;
    WindowRun_();
    if (sTime)
        sTAcc[T_WALL] += Ipc_TimeNs() - t0;
}

/* The window is closed (the game's thread). With the pass on a thread of its own the slot goes to it, with what the pass needs of the game's thread's state. */
static void PushWindow(void)
{
    PassJob *j = sJob;
    sJob = 0;
    if (!j)
        return;
    if (sMode < 3) {   /* the identity pass: the units and the path buffers as the real window left them, for the end-state compare */
        Save(1);
        if (!j->gifReal) {
            j->gifRealCap = 4 * (12 + GFX_PASS_SB_QW * 16);
            j->gifReal = Host_Alloc(j->gifRealCap);
        }
        j->gifRealLen = j->gifReal ? gif_real_export(j->gifReal, j->gifRealCap) : 0;
        if (!j->gifRealLen)
            sOverflow = 1;
    }
    j->ev = sEv;
    j->evLen = sEvLen;
    j->evCap = sEvCap;
    j->real = sReal;
    j->realLen = sRealLen;
    j->realCap = sRealCap;
    j->draws = sDraws;
    j->nDraws = sNDraws;
    j->drawCap = sDrawCap;
    memcpy(j->ws, &sWS, sizeof(sWS));
    j->dirty = sDirty;
    j->overflow = sOverflow;
    j->csr = _mm_getcsr();
    j->pages = vif_image_pages();
    j->realVu = g_vu_mem;
    j->kind = 1;
    j->tEnd = sWinEnd;
    j->nMom = sLNMom;
    for (u32 k = 0; k < MOM_MAX; k++)
        j->momA[k] = sLA[k];
    sEv = sReal = 0;
    sDraws = 0;
    sBuf = 0;
    PassJob_Push(j);
}

void Pass_WindowEnd(void)
{
    if (!gGfxPassOpen)
        return;
    gGfxPassOpen = 0;
    sWinEnd = sTime || sThread ? Ipc_TimeNs() : 0;
    if (sMode >= 2)
        PassSend(1, 0, 0, 0);   /* the real records of the window are done */
    if (sThread) {
        PushWindow();
        return;
    }
    WindowRun();
}

#ifdef MH2_PASS_COPY
void PPN(vif_set_pages)(const u16 *pages);   /* gfx_pass_vif.c */
#ifndef MH2_PASS_HELPER
/* The pass thread's side (gfx_pass_run.c calls it): a window's job. The slot's buffers are this file's for the run (sEv, sDraws and the snapshots: the game's thread has
 * made them and does not touch the slot until the job is given back), the sets' bookkeeping of the window's draws is done here, in order, and then the pass runs. */
static int RunJob(PassJob *j)   /* 1: the window's moments went to the helpers (the slot is theirs now) */
{
    u32 i;
    int handed;
    sRunJob = j;
    sBuf = &j->buf;
    sEv = j->ev;
    sEvLen = j->evLen;
    if (sHMod && sMode >= 3 && PassHelp_On())
        ShareSetup(j);
    sEvCap = j->evCap;
    sReal = j->real;
    sRealLen = j->realLen;
    sRealCap = j->realCap;
    sDraws = j->draws;
    sDrawCap = j->drawCap;
    memcpy(&sWS, j->ws, sizeof(sWS));
    sDirty = j->dirty;
    sOverflow = j->overflow;
    for (i = 0; i < j->nDraws; i++)
        Book(i);
    sNDraws = j->nDraws;
    if (j->pages)
        PPN(vif_set_pages)(j->pages);
    if (sMode < 3 && j->gifRealLen)
        gif_real_import(j->gifReal);
    _mm_setcsr(j->csr);
    sSerial = j->serial;
    sWinEnd = j->tEnd;
    sHanded = 0;
    WindowRun();
    handed = sHanded;
    sHanded = 0;
    if (!handed && sMode >= 2)
        PassSend(6, 0, 0, 0);   /* all of the window's records are sent (a helper's window: the helper that ends it sends this) */
    return handed;
}

static void RunStep(u64 vblank)
{
    if (sHMod && sHModCtl != 2)   /* MH2_PASS_HMODELS: the helpers' Models of the step's windows read the sets: the turn waits for the last of them */
        PassHelp_ModelsWait();
    StepEnd(vblank);
}
#endif   /* !MH2_PASS_HELPER */
#endif

