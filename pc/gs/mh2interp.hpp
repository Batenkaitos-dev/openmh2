/* mh2interp: the in-between picture's vertices (the frame rate's stage 1).
 *
 * This file is part of the port (CC0 1.0). It does not use the GPU: a GIF stream parser that cuts the stream into
 * "kicks" (the vertices of one primitive group), a matcher that pairs the kicks of two steps, and the substitution
 * that writes the mean of two steps' vertices into a copy of the later step's GIF data.
 *
 * A "step" here is what the host handed the renderer between two VSyncs: the GIF data of the three paths and the
 * privileged writes, in order. The game sends one chain of kicks a step (every 2 VBlanks), so the segments that hold
 * path 1 kicks are the chains.
 *
 * Matching: a path 1 kick of the step is paired with the kick of the
 * other step that has the same key (path, PRIM, vertex count, TEX0 when the primitive is textured, ALPHA when it is
 * blended, TEST) and the same occurrence among the kicks of that key. A kick whose vertices move more than the gate
 * (64 px) between the two steps stays as the step has it; so does a kick with no pair. A step with more than a fifth
 * of its path 1 vertices gated or unpaired is a cut: no in-between picture.
 *
 * Interpolation: x and y (in 1/16 px of the window, rounded to the nearest 1/16 px), z, rgb, and ST with Q
 * (perspective-correct: S/Q and T/Q are averaged, and S = (S/Q) * Q). Nothing else changes: the 2D (path 2 and 3) is
 * the step's own. */
#ifndef MH2INTERP_HPP
#define MH2INTERP_HPP

#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace Mh2Interp
{
constexpr uint32_t NO_OFF = 0xFFFFFFFFu;

struct Vertex
{
    uint32_t x, y, z;        /* the GS fields: x and y 16 bits (1/16 px, with the primitive offset), z 24 or 32 bits */
    uint8_t r, g, b, a;
    float s, t, q;
    uint32_t xyz_off;        /* byte offsets in Step::data of the packed qwords the values come from, or NO_OFF */
    uint32_t rgb_off, st_off;
    uint8_t fmt;             /* 0: not a packed vertex (not changed), 4: packed XYZF2, 5: packed XYZ2 */
    uint8_t adc;
};

struct Kick
{
    uint8_t path;
    uint16_t prim;
    uint32_t first, count;   /* in Step::verts */
    int32_t ofx, ofy;        /* XYOFFSET of the primitive's context, 1/16 px */
    uint64_t key;
};

enum EventType : uint8_t { EV_GIF = 0, EV_PRIV = 1, EV_FIFO = 2 };

struct Event
{
    uint8_t type;
    uint8_t path;
    uint32_t off, len;       /* EV_GIF: the data in Step::data; EV_FIFO: len = quad words */
    uint32_t addr;           /* EV_PRIV */
    uint64_t value;
};

struct Step
{
    std::vector<uint8_t> data;
    std::vector<Event> events;
    std::vector<Kick> kicks;
    std::vector<Vertex> verts;
    uint32_t p1_kicks = 0;   /* path 1 kicks with vertices */
    bool flip = false;       /* a write of DISPFB1 or DISPFB2 */
    float cam[32];           /* the camera of the step's first model (view, then projection: MH2IPC_REC_CAMERA), when ncam > 0 */
    float cam_last[32];      /* the last one */
    uint32_t cam_at = 0;     /* (diagnostics) the step's data bytes before its first camera record */
    uint32_t ncam = 0;       /* the cameras the step's models were drawn with (records that differ from the one before) */
    void clear()
    {
        ncam = 0;
        cam_at = 0;
        data.clear();
        events.clear();
        kicks.clear();
        verts.clear();
        p1_kicks = 0;
        flip = false;
    }
    /* Keep only what matching needs (kicks and vertices). */
    void drop_data()
    {
        std::vector<uint8_t>().swap(data);
        std::vector<Event>().swap(events);
    }
};

/* The stream parser. gif() takes the data exactly as the renderer's gif_transfer does (whole quad words; a tag's data
 * may continue in later calls); finish() hands out the step built so far and starts the next. */
class Capture
{
public:
    Capture();
    void gif(uint32_t path, const void *data, size_t bytes);
    void priv(uint32_t addr, uint64_t value);
    void fifo(uint32_t qwords);
    void finish(Step &out);   /* out is cleared first; the GS register state the parser follows is kept */
    void camera(const float *m);   /* a camera record (32 floats) came */
    void reset();             /* forget the registers, the paths and the step in progress */

private:
    struct PathState
    {
        uint64_t regs = 0;
        uint32_t left = 0, idx = 0, nloop = 0, nreg = 0, flg = 0;
    };
    void qword(PathState &ps, uint32_t path, const uint8_t *q, uint32_t off);
    void wreg(uint32_t addr, uint64_t v);
    void vertex(uint32_t x, uint32_t y, uint32_t z, uint32_t adc, uint32_t off, uint8_t fmt);
    void close_kick();
    uint32_t ctx() const { return (prim_ >> 9) & 1; }

    Step cur_;
    PathState ps_[4];
    uint64_t reg_[0x80];
    uint64_t prim_ = 0;
    uint8_t rgba_[4];
    float s_ = 0, t_ = 0, q_ = 1;
    uint32_t rgb_off_ = NO_OFF, st_off_ = NO_OFF;
    bool open_ = false;
    uint32_t cur_path_ = 1;
    uint64_t key_head_ = 0;
    float pend_[32], pend_last_[32];   /* the camera records since the last path 1 data: the camera of the chain that data starts's NEXT frame, see camera() */
    uint32_t pend_n_ = 0;
};

/* The path 1 kicks of a step by key, in order: the index the matcher looks occurrences up in. */
struct Index
{
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_key;
    void build(const Step &s);
    void clear() { by_key.clear(); }
};

struct Result
{
    uint32_t kicks = 0, interpolated = 0, gated = 0, unmatched = 0;   /* path 1 kicks */
    uint64_t verts = 0, v_interpolated = 0, v_gated = 0, v_unmatched = 0;
    uint64_t v_on = 0, v_on_bad = 0;   /* path 1 vertices in or near the picture; of them, in gated or unpaired kicks */
    uint64_t err[6] = {0, 0, 0, 0, 0, 0}, merr[6] = {0, 0, 0, 0, 0, 0};   /* merr: of the vertices that move more than 1/2 px from the earlier step to this */
    uint32_t reproj = 0;                                        /* kicks moved by the camera (no pair, or gated) */
    uint64_t v_reproj = 0;
    double cam_eye = 0, cam_deg = 0;                            /* the camera's motion from the step before: the eye's distance, the turn */
    uint64_t rp_err[7] = {0, 0, 0, 0, 0, 0, 0};                 /* (tests) paired kicks: the camera's prediction of step a's vertices, error in px: <= 1/8, 1/4, 1/2, 1, 2, 8, more */
    uint64_t rpz_err[7] = {0, 0, 0, 0, 0, 0, 0};                /* (tests) the same with w found from z */
    uint32_t rpk_ok = 0, rpk_bad = 0;                           /* (tests) paired kicks of which 90 % of the vertices the camera puts within 1 px of step a's, and the others */
    uint64_t rpv_ok = 0, rpv_bad = 0;
    uint64_t rp_bad = 0;                                        /* (tests) vertices that could not be unprojected */
    uint64_t err_all[6] = {0, 0, 0, 0, 0, 0};   /* (tests, base = the real step) interpolated vertices in the picture by xy error: <= 1/16 px, 1/4, 1/2, 1, 2, more */
    uint32_t un_key = 0, un_cnt = 0, un_fmt = 0, un_adc = 0;   /* (diagnostics) unpaired kicks by reason: no kick of the key, a vertex count that differs, a vertex format, the ADC flags */
    uint32_t g_on = 0;                                          /* (diagnostics) gated kicks with a vertex in the picture that moves far */
    uint64_t v_un_cnt = 0, v_un_key = 0;
    bool cut = false;
};

struct Params
{
    int gate_px = 64;
    double cut_ratio = 0.20;
    bool gate_on_screen = false;   /* only a vertex in (or near) the picture can gate its kick */
    int xy_bias = 1, rgb_bias = 1;   /* added before the halving of the sum (1: round up, 0: floor) */
    uint32_t klo = 0, khi = 0xFFFFFFFFu;   /* (experiments) only the kicks klo <= index < khi are interpolated */
    bool stats = false;   /* (tests) Result::err and merr */
    int abl = 0;   /* (experiments) 1: no xy, 2: no z, 4: no rgb, 8: no st, 16: linear st, 32: no y, 64: no common position */
    int anchor_margin = 384;   /* a position within this many pixels of the picture is held by a kick that is not interpolated */
    int persp = 0;   /* (prototype) 1: x and y are the mean in clip space (Q is 1/w), about the picture's centre, where Q is known */
    int unify_px = 2;   /* vertices at one position in the step take one mean when their means are within this many pixels */
    bool same_adc = true;   /* a pair must have the same drawing-kick-disable flags (a clipped strip differs) */
    /* Where the in-between picture lies between the earlier step (0) and this one (1). mh2gs gives it the position of the
     * picture being made; interpolate() follows it (rounded to 1/256: 1/2 is the mean, bit for bit). */
    double alpha = 0.5;
    bool reproject = true;         /* a kick with no pair, or gated, goes with the camera (when both steps have one, and it did not jump) */
    double cam_cut_eye = 400.0;    /* the eye moved further than this between two steps (game units): a camera cut, no reprojection */
    double cam_cut_deg = 30.0;     /* or turned by more than this */
    int rp_z = 0;                  /* (experiments) 1: z of a reprojected vertex from the projection too */
    bool rp_single = true;         /* only when each step's models were all drawn with one camera */
    bool rp_paired = true;        /* a pair that still world geometry accounts for (the camera's motion explains it to within rp_tol) goes with the camera too */
    double rp_tol = 0.5;
    int rp_w = 0;                  /* (experiments) how the w of a vertex is found: 0 from its Q (1/w), 1 from its z */
};

struct Decision
{
    int32_t ka = -1, kb = -1;   /* the kicks of a and b, or -1 */
    uint8_t what = 0;           /* 0 unmatched, 1 interpolate, 2 gated, 3 moved by the camera (it had no pair, or was gated) */
    uint8_t held = 0;           /* a gated kick that is held at step a's position below alpha 1/2 (all its vertices: a kick of which some are
                                   at step a and some at step n would be torn across the picture) */
};

/* What is decided about a step against its neighbours, which does not depend on alpha: the pairing, the gate, the cut. */
struct Plan
{
    std::vector<Decision> dec;
    Result res;   /* the counts; res.cut: no in-between picture */
    /* what apply_interp reads (they do not depend on alpha; apply_interp does not write the plan) */
    std::vector<int32_t> bx, by;      /* each vertex's position in the step, the picture's own coordinates, 1/16 px */
    std::vector<uint32_t> slot;       /* the slot of its position (vertices at one position share one), 0xFFFFFFFF: none */
    std::vector<uint8_t> anch;        /* per slot, within the margin of the picture: 1 a kick with no pair is there (step n's position at every alpha),
                                         2 only gated kicks are there (step n-1's position below alpha 1/2, step n's from 1/2 on), 0 none */
    std::vector<int32_t> ax, ay;      /* each vertex of an interpolated or gated kick: its position in step a (the picture's own coordinates) */
    bool cam_ok = false;              /* the camera moves the kicks with what 3 */
    double cam_a[32], cam_b[32], cam_n[32];   /* the cameras of step a, of step b and of the step itself (view, projection; b is the step itself when live) */
    std::vector<float> wpt;           /* per vertex of a kick with what 3: its world point (3 floats), made with the step's camera */
    std::vector<float> whw;           /* the w the step's camera gives it (the texture's Q is 1/w times a constant: its ratio to the new w scales Q) */
    std::vector<uint8_t> wok;         /* 1: the world point is good */
    std::vector<int32_t> hx, hy;      /* per slot: the least position in step a of the held vertices there (the held position) */
};

/* What apply_interp works in (it does not depend on anything but the call): a caller that runs apply_interp on more
 * than one thread at once at one Plan gives each thread its own. */
struct Scratch
{
    std::vector<int32_t> fx, fy, cx, cy;
    std::vector<uint8_t> fix;
};

/* The first half of interpolate(): returns false for a cut (plan.res.cut). b may be &base, as in interpolate(). */
bool plan_interp(const Step &base, const Step &a, const Index &ia, const Step &b, const Index &ib, const Params &p, Plan &plan);

/* The second half, for one alpha (in 1/256: 0 is step a, 256 is step b; 128 is the mean, as interpolate() has it): writes the
 * weighted mean of the vertices of a and b into `data`, which must be a copy of base.data (one copy for each alpha). It only
 * reads everything else, so calls for different alphas may run at once if each has its own `scratch` (null: one made for the call). */
Result apply_interp(const Step &base, uint8_t *data, const Step &a, const Step &b, const Plan &plan, const Params &p, uint32_t alpha,
                    Scratch *scratch = nullptr);

/* Writes into `base.data` the mean of the vertices of a and b for each path 1 kick of base that both have (and that does not
 * move more than the gate). b may be &base (then the mean is of a and base itself: the live in-between of the steps
 * a and base). Nothing is written when the result is a cut. */
Result interpolate(Step &base, const Step &a, const Index &ia, const Step &b, const Index &ib, const Params &p);
}

#endif
