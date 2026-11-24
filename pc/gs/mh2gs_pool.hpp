/* The room of the moments' records (development builds; MH2_PASS_RXPOOL=1). Part of the port (CC0 1.0).
 *
 * The host's reader takes each moment's record off the second ring into a byte vector of its own. A fresh vector grows as
 * the record's pieces come: every growth copies what it holds into new memory, and new memory is paid for page by page
 * when it is first written. This pool gives a moment's vector its whole room at once, when the record's first piece says
 * how long the record is, and keeps the room of the moments that are over for the moments to come, so that a record is
 * copied once, into memory that has been written before.
 *
 * Only where the bytes lie changes: what a vector holds, and so every picture, is the same with the pool on and off.
 * The caller holds the lock that guards the vectors (the pool has none of its own). */
#ifndef MH2GS_POOL_HPP
#define MH2GS_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

struct Mh2PassPool
{
    bool on = false;
    size_t max_n = 48;                       /* the vectors kept at most */
    size_t max_bytes = size_t(96) << 20;     /* and their room in all */
    size_t keep_cap = size_t(16) << 20;      /* a vector with more room than this is not kept */
    std::vector<std::vector<uint8_t>> kept;
    size_t bytes = 0;                        /* the room of the kept vectors */
    uint64_t taken = 0, reused = 0, fresh = 0, grown = 0, given = 0, let_go = 0;

    /* A record of `need` bytes begins in `v`, which is empty: v gets room for all of it. A kept vector's room is used
     * when there is one: the smallest that holds the record, else the largest (which then grows once). */
    void take(std::vector<uint8_t> &v, size_t need)
    {
        if (!on || !v.empty())
            return;
        taken++;
        if (v.capacity() < need && !kept.empty())
        {
            size_t best = kept.size(), big = 0;
            for (size_t i = 0; i < kept.size(); i++)
            {
                const size_t c = kept[i].capacity();
                if (c >= need && (best == kept.size() || c < kept[best].capacity()))
                    best = i;
                if (c > kept[big].capacity())
                    big = i;
            }
            const size_t k = best != kept.size() ? best : big;
            if (kept[k].capacity() > v.capacity())
            {
                bytes -= kept[k].capacity();
                v.swap(kept[k]);
                kept[k].swap(kept.back());
                kept.pop_back();
                reused++;
            }
        }
        if (v.capacity() < need)
        {
            if (v.capacity())
                grown++;
            else
                fresh++;
            v.reserve(need);
        }
    }

    /* A moment is over: the room of its vector is kept for a later record, unless the pool is full. The vector is left
     * empty either way. */
    void give(std::vector<uint8_t> &v)
    {
        const size_t c = v.capacity();
        if (!on || !c)
            return;
        v.clear();
        if (kept.size() >= max_n || c > keep_cap || bytes + c > max_bytes)
        {
            let_go++;
            std::vector<uint8_t>().swap(v);
            return;
        }
        kept.emplace_back();
        kept.back().swap(v);
        bytes += c;
        given++;
    }
};

#endif
