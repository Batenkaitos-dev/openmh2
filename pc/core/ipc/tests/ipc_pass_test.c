/*
 * The pass's ring, the labels and the live plans at the core's side (host_ipc.c), in both builds.
 *   ipc_pass_test ring     the block has the pass's ring: a label is kept and found once, a record goes into the ring, a live plan is read
 *   ipc_pass_test noring   the block has none: nothing goes into the ring; a release build keeps no label (Pal_GsLabels is 0), a development build keeps them
 *   ipc_pass_test control  the noring checks on a block with the ring: must fail (the test's control)
 * The host's side is played by the test itself (the block made here, the rings read here).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "ipc/core_ipc.h"
#include "ipc/mh2ipc.h"
#include "ipc/mh2ipc_os.h"

extern int Pal_GsLabels;

#define FAIL(...)                                   \
    do {                                            \
        fprintf(stderr, "ipc_pass_ring: FAIL: ");   \
        fprintf(stderr, __VA_ARGS__);               \
        fprintf(stderr, "\n");                      \
        exit(1);                                    \
    } while (0)

static Mh2IpcMap hm;
static Mh2IpcHeader *H;
static unsigned char *B;

static void attach(int ring)
{
    if (mh2ipc_create(&hm, MH2IPC_TOTAL_BYTES + (ring ? MH2IPC_PASS_BYTES : 0)) != 0)
        FAIL("mh2ipc_create");
    H = (Mh2IpcHeader *)hm.base;
    B = (unsigned char *)hm.base;
    mh2ipc_layout(H, mh2ipc_pid());
    if (ring)
        mh2ipc_layout_pass(H);
    H->host_flags = MH2IPC_HOST_GS;
#ifdef _WIN32
    char env[160];
    snprintf(env, sizeof(env), "%s=%s", MH2IPC_ENV, hm.name);
    _putenv(env);
#else
    setenv(MH2IPC_ENV, hm.name, 1);
#endif
    if (Ipc_Init() != 1 || !Ipc_Attached())
        FAIL("Ipc_Init did not attach");
}

/* A label set for a chain at 0x1230 and looked for at its first tag: 1 when it was kept (and it is then used up). */
static int label_kept(void)
{
    float world[16], cam[32], vp[16];
    IpcLabel lab;
    for (int i = 0; i < 16; i++)
        world[i] = vp[i] = (float)i;
    for (int i = 0; i < 32; i++)
        cam[i] = (float)(100 + i);
    Ipc_GsModelSet(0x1230, 0x1270, 77, 1, 2, world, cam, 0x4000, vp);
    memset(&lab, 0, sizeof(lab));
    if (!Ipc_GsModelTag(0x1230, &lab))
        return 0;
    if (lab.top != 0x1230 || lab.last != 0x1270 || lab.handle != 77 || lab.prog != 2 || lab.world[5] != 5.0f || lab.cam[31] != 131.0f)
        FAIL("the label came back changed");
    if (Ipc_GsModelTag(0x1230, &lab))
        FAIL("a label was found twice");
    return 1;
}

static void with_ring(void)
{
    static const unsigned int data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned int alpha[MH2IPC_LIVE_MAX], vb = 1000, n = 0, fb = 0;
    const unsigned int set[3] = {64, 128, 192};
    if (Pal_GsLabels != 1)
        FAIL("ring: Pal_GsLabels is %d", Pal_GsLabels);
    if (!label_kept())
        FAIL("ring: the label was not kept");
    if (Ipc_GsPassRing(0x37, 9, data, 2) != 1)
        FAIL("ring: Ipc_GsPassRing did not send");
    const Mh2IpcRec *rec = mh2ipc_rec_peek(&H->pass, B);
    if (!rec || rec->type != MH2IPC_REC_PASS || rec->arg0 != 0x37 || rec->arg1 != 9 || rec->bytes != 32 || memcmp(rec + 1, data, 32) != 0)
        FAIL("ring: the pass's record");
    mh2ipc_rec_release(&H->pass, rec);
    if (Ipc_GsLivePlan(&vb, &n, alpha, &fb))
        FAIL("ring: a live plan before the host wrote one");
    mh2ipc_liveplan_write(&H->live[(1000u >> 1) & 1u], 1000, 3, set, 0);
    vb = 1000;
    if (!Ipc_GsLivePlan(&vb, &n, alpha, &fb) || vb != 1000 || n != 3 || alpha[0] != 64 || alpha[2] != 192)
        FAIL("ring: the live plan");
}

static void without_ring(void)
{
    static const unsigned int data[4] = {1, 2, 3, 4};
    unsigned int head = H->gs.head;
    if (Pal_GsLabels != 0)
        FAIL("noring: Pal_GsLabels is %d", Pal_GsLabels);
    if (label_kept())
        FAIL("noring: a release build kept a label");
    if (H->gs.head != head)
        FAIL("noring: a release build put a record without the ring");
    if (Ipc_GsPassRing(0x37, 9, data, 1) != 0)
        FAIL("noring: Ipc_GsPassRing sent");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "ring")) {
        attach(1);
        with_ring();
    } else if (!strcmp(mode, "noring")) {
        attach(0);
        without_ring();
    } else if (!strcmp(mode, "control")) {
        attach(1);
        without_ring();
    } else {
        FAIL("usage: ipc_pass_test ring|noring|control");
    }
    Ipc_Exit(0);
    mh2ipc_close(&hm);
    printf("ipc_pass_ring: %s OK\n", mode);
    return 0;
}
