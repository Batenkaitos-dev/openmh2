/* The room of the moments' records (pc/gs/mh2gs_pool.hpp), without a GPU and without the game: records taken
 * in pieces into vectors of the pool, against the same records taken into fresh vectors.
 * Part of the port (CC0 1.0).
 *
 *   mh2gs_pool_test [control]
 *
 * Without an argument every case must come out right. With `control` the first case is run with the pool off: its checks
 * must then fail, which shows that the test can fail. Exit status 0 when it came out as it must. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../mh2gs_pool.hpp"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd()
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* a record's pieces appended to v, as the reader appends them; returns whether the vector's memory moved while it grew */
static bool append(std::vector<uint8_t> &v, size_t bytes, uint32_t seed)
{
    const uint8_t *at = v.data();
    bool moved = false;
    size_t done = 0;
    while (done < bytes)
    {
        uint8_t piece[4096];
        const size_t n = bytes - done < sizeof(piece) ? bytes - done : sizeof(piece);
        for (size_t i = 0; i < n; i++)
            piece[i] = uint8_t((done + i) * 31u + seed);
        v.insert(v.end(), piece, piece + n);
        if (at && v.data() != at)
            moved = true;
        at = v.data();
        done += n;
    }
    return moved;
}

/* One record after another through one pool: the room is given at once, and a later record takes an earlier one's. */
static void reuse(bool on)
{
    Mh2PassPool p;
    p.on = on;
    std::vector<uint8_t> a, b;
    p.take(a, 100000);
    CHECK(a.capacity() >= 100000 && a.empty());
    CHECK(p.taken == 1 && p.fresh == 1 && p.reused == 0);
    CHECK(!append(a, 100000, 1));   /* the room was there: the vector never moved */
    const uint8_t *room = a.data();
    p.give(a);
    CHECK(a.capacity() == 0 && a.empty());
    CHECK(p.given == 1 && p.kept.size() == 1 && p.bytes >= 100000);
    p.take(b, 80000);
    CHECK(p.reused == 1 && p.fresh == 1 && b.empty() && b.capacity() >= 100000);
    CHECK(b.data() == room);        /* the first record's room */
    CHECK(p.kept.empty() && p.bytes == 0);
    CHECK(!append(b, 80000, 2));
}

/* The smallest kept room that holds the record; when none does, the largest, grown once. */
static void fit()
{
    Mh2PassPool p;
    p.on = true;
    std::vector<uint8_t> v[3], x, y;
    const size_t caps[3] = {1000, 5000, 3000};
    for (int i = 0; i < 3; i++)
        p.take(v[i], caps[i]);
    for (int i = 0; i < 3; i++)
        p.give(v[i]);
    CHECK(p.kept.size() == 3 && p.fresh == 3);
    p.take(x, 2500);
    CHECK(x.capacity() >= 3000 && x.capacity() < 5000 && p.reused == 1 && p.grown == 0);
    p.take(y, 9000);
    CHECK(y.capacity() >= 9000 && p.reused == 2 && p.grown == 1);
    CHECK(p.kept.size() == 1 && p.kept[0].capacity() >= 1000 && p.kept[0].capacity() < 3000);
}

/* The pool's limits: the count, a single vector's room, the room in all. */
static void limits()
{
    Mh2PassPool p;
    p.on = true;
    p.max_n = 2;
    p.keep_cap = 10000;
    p.max_bytes = 12000;
    std::vector<uint8_t> a, b, c, d;
    p.take(a, 4000);
    p.take(b, 4000);
    p.take(c, 4000);
    p.take(d, 20000);
    p.give(d);   /* over a single vector's limit */
    CHECK(p.let_go == 1 && p.kept.empty() && d.capacity() == 0);
    p.give(a);
    p.give(b);
    CHECK(p.given == 2 && p.kept.size() == 2);
    p.give(c);   /* the count */
    CHECK(p.let_go == 2 && p.kept.size() == 2 && c.capacity() == 0);
    p.max_n = 8;
    std::vector<uint8_t> e;
    p.take(e, 9000);   /* takes the largest kept (4000) and grows it */
    p.give(e);         /* 4000 kept + 9000: over the room in all */
    CHECK(p.let_go == 3 && p.kept.size() == 1 && p.bytes <= p.max_bytes);
}

/* 400 records of every size through the pool, four alive at a time: each holds the bytes a fresh vector holds. */
static void same_bytes()
{
    Mh2PassPool p;
    p.on = true;
    std::vector<uint8_t> live[4];
    int bad = 0, moved = 0;
    for (uint32_t r = 0; r < 400; r++)
    {
        std::vector<uint8_t> &v = live[r & 3];
        p.give(v);
        const size_t bytes = (rnd() % 300000u) & ~size_t(15);
        std::vector<uint8_t> plain;
        append(plain, bytes, r);
        p.take(v, bytes);
        moved += append(v, bytes, r);
        if (v != plain)
            bad++;
    }
    CHECK(bad == 0);
    CHECK(moved == 0);
    CHECK(p.reused > 300);          /* nearly every record lies in room that was used before */
    CHECK(p.bytes <= p.max_bytes && p.kept.size() <= p.max_n);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "control"))
    {
        reuse(false);
        if (failures == 0)
        {
            printf("mh2gs_pool_test control: FAIL: the pool off passed the checks of the pool on\n");
            return 1;
        }
        printf("mh2gs_pool_test control: OK (%d checks failed with the pool off, as they must)\n", failures);
        return 0;
    }
    reuse(true);
    fit();
    limits();
    same_bytes();
    if (failures)
    {
        printf("mh2gs_pool_test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("mh2gs_pool_test: OK\n");
    return 0;
}
