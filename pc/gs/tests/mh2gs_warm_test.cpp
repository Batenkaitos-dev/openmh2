/* The warm-up exemption's rule (pc/gs/mh2gs_warm.hpp), without a GPU and without the game: sequences of steps
 * (the bytes of each step's own chain, whether it was late) and which of them the rule leaves out of the give-up's history.
 * Part of the port (CC0 1.0).
 *
 *   mh2gs_warm_test [control]
 *
 * Without an argument every case must come out right. With `control` the first case is run with the exemption off (n = 0):
 * its checks must then fail, which shows that the test can fail. Exit status 0 when it came out as it must. */
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../mh2gs_warm.hpp"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const uint64_t K = 1024, M = 1024 * 1024;

/* A menu's steps, then a scene's: the scene's first n steps are the stretch, and nothing after them. */
static void scene_start(uint32_t n)
{
    Mh2PassWarm w;
    w.n = n;
    uint64_t vs = 4000;
    for (int i = 0; i < 20; i++, vs += 2)
        CHECK(w.step(470 * K, vs, false) == 0);
    vs = 5243;   /* (a loading screen between: the VSyncs are not consecutive, and the rule does not ask them to be) */
    CHECK(w.step(3500 * K, vs, false) == 2);
    CHECK(w.vs0 == 5243);
    const bool lates[7] = {false, true, true, true, false, true, false};
    for (int i = 0; i < 7; i++)
    {
        vs += 2;
        CHECK(w.step(3500 * K, vs, lates[i]) == 1);
        CHECK(w.left == uint32_t(6 - i));
    }
    CHECK(w.mask == 0x3a);   /* 0111010: the late steps of the eight, the first the oldest */
    for (int i = 0; i < 40; i++)
    {
        vs += 2;
        CHECK(w.step(3400 * K, vs, (i & 1) != 0) == 0);
    }
    CHECK(w.starts == 1);
    CHECK(w.steps == 8);
    CHECK(w.late == 4);
}

/* A scene that builds up over three steps is one start, and the stretch is counted from its first step. */
static void build_up()
{
    Mh2PassWarm w;
    w.n = 4;
    CHECK(w.step(400 * K, 100, false) == 0);
    CHECK(w.step(1200 * K, 102, false) == 2);
    CHECK(w.step(2600 * K, 104, false) == 1);
    CHECK(w.step(3500 * K, 106, false) == 1);
    CHECK(w.step(3500 * K, 108, false) == 1);
    CHECK(w.left == 0);
    CHECK(w.step(3500 * K, 110, true) == 0);
    CHECK(w.starts == 1);
    CHECK(w.steps == 4);
}

/* What is not a start: the first step of all, a growth under twice, a growth under MIN_GROW, a scene that started while the
 * pictures came from the second renderer. */
static void not_a_start()
{
    Mh2PassWarm w;
    w.n = 8;
    CHECK(w.step(4 * M, 10, true) == 0);            /* nothing before it */
    CHECK(w.step(7 * M, 12, true) == 0);            /* under twice */
    CHECK(w.step(100 * K, 14, false) == 0);
    CHECK(w.step(300 * K, 16, false) == 0);         /* three times, but 200 KB more only */
    CHECK(w.step(555 * K, 18, false) == 0);         /* 255 KB more, under twice */
    w.seen_step(500 * K, 20);
    w.seen_step(4 * M, 22);                         /* the scene starts here, in the other phase */
    CHECK(w.step(4 * M, 24, true) == 0);
    CHECK(w.starts == 0);
    CHECK(w.steps == 0);
    /* and the edge: exactly twice and exactly MIN_GROW more is a start */
    w.seen_step(Mh2PassWarm::MIN_GROW, 26);
    CHECK(w.step(2 * Mh2PassWarm::MIN_GROW, 28, false) == 2);
    CHECK(w.starts == 1);
}

/* Two scenes one after the other: a start each, the second only after the first stretch has ended. */
static void two_scenes()
{
    Mh2PassWarm w;
    w.n = 2;
    CHECK(w.step(300 * K, 2, false) == 0);
    CHECK(w.step(3 * M, 4, true) == 2);
    CHECK(w.step(3 * M, 6, true) == 1);
    CHECK(w.step(3 * M, 8, true) == 0);
    CHECK(w.step(300 * K, 10, false) == 0);
    CHECK(w.step(3 * M, 12, false) == 2);
    CHECK(w.step(3 * M, 14, false) == 1);
    CHECK(w.step(3 * M, 16, false) == 0);
    CHECK(w.starts == 2);
    CHECK(w.steps == 4);
    CHECK(w.late == 2);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "control"))
    {
        scene_start(0);
        if (failures == 0)
        {
            printf("mh2gs_warm_test control: FAIL: the exemption off passed the checks of the exemption on\n");
            return 1;
        }
        printf("mh2gs_warm_test control: OK (%d checks failed with the exemption off, as they must)\n", failures);
        return 0;
    }
    scene_start(8);
    build_up();
    not_a_start();
    two_scenes();
    if (failures)
    {
        printf("mh2gs_warm_test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("mh2gs_warm_test: OK\n");
    return 0;
}
