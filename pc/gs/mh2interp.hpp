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
#include <array>
#include <cstdio>
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
    uint32_t uv;             /* the UV register when the vertex was made: u in the low 16 bits, v in the high 16 */
    uint8_t fmt;             /* 0: not a packed vertex (not changed), 4: packed XYZF2, 5: packed XYZ2, 6: an A+D or register-list XYZ2/XYZF2 write (2D: x and y at +0 and +2 of xyz_off) */
    uint8_t adc;
};

struct Kick
{
    uint8_t path;
    uint16_t prim;
    uint32_t first, count;   /* in Step::verts */
    int32_t ofx, ofy;        /* XYOFFSET of the primitive's context, 1/16 px */
    uint64_t key;
    int32_t model = -1;      /* in Step::models: the draw this kick came from (MH2IPC_REC_MODEL), or -1 */
    int32_t anchor = -1;     /* in Step::anchors: the 2D element placed from a 3D point this kick belongs to (MH2IPC_REC_ANCHOR), or -1 */
};

/* A model draw's label (MH2IPC_REC_MODEL) in a step: its kicks are those with Kick::model == its index. */
struct ModelDraw
{
    uint32_t handle, flags, prog;
    uint32_t site = 0;       /* (diagnostics) the return address of Model_Draw: the game's function that drew it (development builds' core) */
    uint32_t occ;            /* the draws of this handle before it in the step */
    uint32_t first_kick;     /* in Step::kicks, when the label came */
    uint32_t nkicks = 0;     /* path 1 kicks with vertices */
    float world[16];         /* the model's matrix (gMtxSlots[0]) */
    uint32_t cam = 0;        /* its camera: an index in Step::cams (the view and the projection the draw was made with) */
};

/* A 2D element placed from a 3D point (MH2IPC_REC_ANCHOR): the 2D kicks with Kick::anchor == its index are its. */
struct Anchor
{
    uint32_t id, occ;        /* the game's id (the call site, the thing it belongs to) and the elements of that id before it in the step */
    float point[3];          /* the world point */
    uint32_t cam;            /* an index in Step::cams: the view and the projection the game projected the point with */
    uint32_t nkicks = 0;
    int32_t first_kick = -1; /* in Step::kicks: its first kick with vertices */
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
    std::vector<ModelDraw> models;   /* the labelled model draws, in the stream's order */
    std::vector<Anchor> anchors;     /* the labelled 2D elements placed from a 3D point, in the stream's order */
    std::vector<std::array<float, 32>> cams;   /* the distinct cameras of the labelled draws (ModelDraw::cam indexes it) */
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
        models.clear();
        anchors.clear();
        cams.clear();
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
    void model(uint32_t handle, uint32_t flags, uint32_t prog, const float *world, const float *cam, uint32_t site);   /* a model label came (handle 0: the end of a labelled run) */
    void anchor(uint32_t id, const float *point, const float *cam);   /* an anchor label came (id 0: the end of its packet) */
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
    uint32_t uv_ = 0;
    uint32_t cur_off_ = NO_OFF;   /* the data qword (or its half) of the A+D or register-list write being taken */
    bool open_ = false;
    uint32_t cur_path_ = 1;
    uint64_t key_head_ = 0;
    float pend_[32], pend_last_[32];   /* the camera records since the last path 1 data: the camera of the chain that data starts's NEXT frame, see camera() */
    uint32_t pend_n_ = 0;
    int32_t cur_model_ = -1;
    int32_t cur_anchor_ = -1;
    std::unordered_map<uint32_t, uint32_t> occ_, occa_;
};

/* The path 1 kicks of a step by key, in order: the index the matcher looks occurrences up in. */
struct Index
{
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_key;       /* every path 1 kick, by key */
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_key_unl;   /* the kicks no model draw labels, by key */
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_mkey;      /* the labelled kicks by (model draw, key), in order */
    std::unordered_map<uint64_t, int32_t> by_model;                   /* (handle, occurrence) -> the draw's index in the step */
    std::unordered_map<uint64_t, int32_t> by_anchor;                  /* (id, occurrence) -> the anchor's index in the step */
    void build(const Step &s);
    void clear()
    {
        by_key.clear();
        by_key_unl.clear();
        by_mkey.clear();
        by_model.clear();
        by_anchor.clear();
    }
};

/* (tests) the error of the in-between positions against the real step, per model draw (stats mode, the skip test) */
struct ModelErr
{
    int32_t model = -1;                 /* in Step::models, or -1: the kicks no draw labels */
    uint32_t handle = 0, occ = 0, prog = 0, flags = 0;
    uint32_t kicks[4] = {0, 0, 0, 0};   /* by what: 0 unpaired, 1 interpolated, 2 gated, 3 moved by the camera */
    uint64_t verts = 0;                 /* in the picture */
    double sum = 0, max = 0;            /* the error in px */
    uint64_t over2 = 0, over8 = 0;
    float pos[3] = {0, 0, 0}, eye[3] = {0, 0, 0};   /* the draw model matrix translation, and the step camera eye */
    double sum_what[4] = {0, 0, 0, 0};
    uint64_t v_what[4] = {0, 0, 0, 0};
};

struct Result
{
    uint32_t kicks = 0, interpolated = 0, gated = 0, unmatched = 0;   /* path 1 kicks */
    uint64_t verts = 0, v_interpolated = 0, v_gated = 0, v_unmatched = 0;
    uint64_t v_on = 0, v_on_bad = 0;   /* path 1 vertices in or near the picture; of them, in gated or unpaired kicks */
    uint64_t err[6] = {0, 0, 0, 0, 0, 0}, merr[6] = {0, 0, 0, 0, 0, 0};   /* merr: of the vertices that move more than 1/2 px from the earlier step to this */
    uint32_t born_kicks = 0;                                    /* kicks of no pair in step a that the camera moves (a birth: Params::birth) */
    uint32_t att_kicks = 0;                                     /* kicks left as the step has them because their draw is fixed to the camera (Params::attach) */
    uint32_t twod = 0;                                          /* 2D kicks placed between their positions in a and b (Params::twod_px) */
    uint32_t anch = 0, anch_kicks = 0;                          /* anchors moved with their 3D point (Params::anchors), and their 2D kicks */
    uint32_t far_int = 0;                                       /* (diagnostics) gated kicks interpolated by Params::far_coh */
    uint32_t reproj = 0;                                        /* kicks moved by the camera (no pair, or gated) */
    uint64_t v_reproj = 0;
    uint32_t k_c3[6] = {0, 0, 0, 0, 0, 0};                         /* (diagnostics) of them, by why: no pair, count differs, format differs, ADC differs, gated, still */
    uint64_t v_c3[6] = {0, 0, 0, 0, 0, 0};
    double cam_eye = 0, cam_deg = 0;                            /* the camera's motion from the step before: the eye's distance, the turn */
    uint64_t rp_err[7] = {0, 0, 0, 0, 0, 0, 0};                 /* (tests) paired kicks: the camera's prediction of step a's vertices, error in px: <= 1/8, 1/4, 1/2, 1, 2, 8, more */
    uint64_t rpz_err[7] = {0, 0, 0, 0, 0, 0, 0};                /* (tests) the same with w found from z */
    uint32_t rpk_ok = 0, rpk_bad = 0;                           /* (tests) paired kicks of which 90 % of the vertices the camera puts within 1 px of step a's, and the others */
    uint64_t rpv_ok = 0, rpv_bad = 0;
    uint64_t rp_bad = 0;                                        /* (tests) vertices that could not be unprojected */
    uint64_t err_all[6] = {0, 0, 0, 0, 0, 0};   /* (tests, base = the real step) interpolated vertices in the picture by xy error: <= 1/16 px, 1/4, 1/2, 1, 2, more */
    std::vector<ModelErr> model_err;                           /* (tests) per model draw, with stats */
    uint32_t lb_kicks = 0, lb_unl = 0;                         /* (diagnostics) path 1 kicks of a labelled draw, and not */
    uint32_t lb_diff = 0, lb_nodraw = 0;                        /* of the labelled: those the labels pair other than key and occurrence would (a or b), and those whose draw is not in a or b */
    uint64_t v_lb_diff = 0;
    uint32_t un_key = 0, un_cnt = 0, un_fmt = 0, un_adc = 0;   /* (diagnostics) unpaired kicks by reason: no kick of the key, a vertex count that differs, a vertex format, the ADC flags */
    uint32_t g_on = 0;                                          /* (diagnostics) gated kicks with a vertex in the picture that moves far */
    uint64_t v_un_cnt = 0, v_un_key = 0;
    bool cut = false;
};

struct Params
{
    int gate_px = 64;
    double twod_px = 0;   /* > 0: a 2D kick (path 2 or 3) that a and b both have, with the same texture coordinates, moving no more than this many px, is placed between its positions in a and b */
    double twod_min = 1.0;   /* the 2D kick moves at least this many px (its largest vertex move a to b) to be placed: less is the step's own */
    double far_coh = 0;   /* (tests) > 0: a gated kick that the camera does not account for, whose vertices all move alike (within this many px of the mean displacement), is interpolated */
    double cut_ratio = 0.20;
    bool gate_on_screen = false;   /* only a vertex in (or near) the picture can gate its kick */
    int xy_bias = 1, rgb_bias = 1;   /* added before the halving of the sum (1: round up, 0: floor) */
    FILE *c3log = nullptr;   /* (experiments) one line for each kick that goes with the camera (see plan_interp) */
    uint32_t c3lo = 0, c3hi = 0xFFFFFFFFu;   /* (experiments) only the kicks c3lo <= index < c3hi may go with the camera (what 3) */
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
    bool stats_labels = false;      /* (diagnostics) Result::lb_diff and lb_nodraw (the legacy pairing is found beside the labels') */
    double cam_max_px = 0;         /* a kick the camera moves (what 3) any vertex of which would move further than this from the step (px; 0: no limit) is not moved: the near plane clips the ground at the feet, and a vertex that was on it goes anywhere */
    double attach = 0.0;           /* (off: the honest skip test loses up to 0.23 dB with 0.1; MH2GS_ATTACH=0.1 turns it on) a labelled draw whose matrix in the camera's space (model matrix times view) stays within this share of what the camera's move alone would have changed
                                      is fixed to the camera (a light shaft, a veil in front of it): the camera does not move its kicks (0: off) */
    int attach_fade = 0;           /* such a kick that has no pair is drawn with its vertex alpha times alpha / 256 (the step's own at alpha 1): a kick that comes and goes between steps does not show in full early */
    double attach_min = 10.0;      /* ... and the camera's move alone must have moved the draw by at least this much in the camera's space (its place plus its turn times its distance, in the game's units: below it the test cannot tell a draw fixed to the camera from one that is still) */
    int birth = 0;                 /* a kick with no pair in step a that the camera moves (a birth) is, in the in-between pictures: 0 drawn as the step has it, moved by the camera; 1 faded in
                                      (its vertex alpha times alpha / 256); 2 left out until the step's own picture */
    bool world_blend = true;       /* a paired kick (moving or still) goes between its two world points (each step unprojected with its own camera) seen by the camera at alpha, when the camera is good */
    double hyst = 1.0;             /* (tests) a kick the camera moved last step is kept with it up to rp_tol * hyst, a kick that was interpolated goes to the camera only within rp_tol / hyst (1: no memory) */
    bool mat_class = false;        /* (tests) a kick of a model draw whose matrix changed from step a to step b is interpolated, never taken for still */
    bool still_all = false;        /* (tests) the still test looks at every vertex of the kick and wants all of them within the tolerance (default: a sample of twelve, nine in ten) */
    bool no_still = false;         /* (tests) no kick takes the still shortcut: every paired kick is interpolated (with the world blend when it is on) */
    std::unordered_map<uint64_t, uint8_t> *hmap = nullptr;   /* (tests) per kick of a draw (handle, occurrence, key, occurrence in the draw): what 1 or 3 it had last step; the owner's memory, set with hyst */
    double anchor_tol = 2.0;       /* an anchor is followed only when its first 2D kick's vertex moved from a to b as the point's screen place did, to within this many px
                                      (a sprite that does not follow its point: a size that toggles, a jump to another point, a clamp, is the step's own) */
    double anchor_min = 3.0;       /* a move of less than this many px (the shift from the step's own place) is not made: the game's own place of a sprite is a whole pixel off the projection by up to a pixel, and the skip test (which has the step's own place right) loses windows when smaller moves are made */
    bool anchor_round = true;      /* the anchors' move is in whole pixels (the game's 2D is on the pixel grid) */
    bool anchors = true;           /* the 2D kicks of an anchor that a and b both have (by id and occurrence) move with the anchor's point on the screen: its world point (a to b)
                                      seen by the camera at alpha, against where the step has it; the other 2D is the step's own */
    bool labels = true;            /* the model draws' labels pair the kicks (by draw, then by key within the draw); 0: by key and occurrence in the whole step */
};

struct Decision
{
    int32_t ka = -1, kb = -1;   /* the kicks of a and b, or -1 */
    uint8_t what = 0;           /* 0 unmatched, 1 interpolate, 2 gated, 3 moved by the camera (it had no pair, or was gated) */
    uint8_t why = 0;            /* (diagnostics) what 3: why (Result::k_c3: 0 no pair, 1 count, 2 format, 3 ADC, 4 gated, 5 still) */
    uint8_t att = 0;            /* a kick of a draw fixed to the camera, left as the step has it (Params::attach) */
    uint8_t born = 0;           /* a kick with no pair in step a that goes with the camera (Params::birth) */
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
    std::vector<float> wa3, wb3;      /* per vertex of an interpolated kick (what 1): its world point from step a and from step b (3 floats each), with their cameras */
    std::vector<uint8_t> wbok;        /* 1: both are good */
    std::vector<int32_t> hx, hy;      /* per slot: the least position in step a of the held vertices there (the held position) */
    /* per anchor of the step (Params::anchors): its world points in a and b, the cameras, and where the step has it on the screen */
    struct AnchorPlan
    {
        bool ok = false;
        double wa[3], wb[3];
        double cam_a[32], cam_b[32];
        double sx0 = 0, sy0 = 0;      /* the point as the step has it: the step's world point seen by the step's own anchor camera (GS pixels) */
        double pa_x = 0, pa_y = 0, pb_x = 0, pb_y = 0;   /* (diagnostics) a's and b's projections */
    };
    std::vector<AnchorPlan> anc;
};

/* What apply_interp works in (it does not depend on anything but the call): a caller that runs apply_interp on more
 * than one thread at once at one Plan gives each thread its own. */
struct Scratch
{
    std::vector<int32_t> fx, fy, cx, cy;
    std::vector<uint8_t> fix;
};

/* The first half of interpolate(): returns false for a cut (plan.res.cut). b may be &base, as in interpolate(). */
bool diag_world(const Plan &plan, double t, double sx, double sy, double w, double out[3]);
bool diag_world_c(const double *ca, const double *cb, double t, double sx, double sy, double w, double out[3]);   /* (diagnostics) the same with the cameras of a draw */
double diag_scale_c(const double *cb, const double W[3]);   /* (diagnostics) a screen vertex's world point by the camera at t */
double diag_scale(const Plan &plan, const double W[3]);   /* (diagnostics) screen pixels a world unit moves a point at W by step b's camera */
bool plan_interp(const Step &base, const Step &a, const Index &ia, const Step &b, const Index &ib, const Params &p, Plan &plan);

/* Several moments a step (the pass's records): a moment above 0 is taken only when the streams of the moments after the first exist, which the first record of
 * the step, moment 0, makes (nm - 1 of them): XS is how many there are. A moment 0 that did not come first (a window of the pass thread that was late, a stream
 * that began in the middle of a step) leaves none, and a stream indexed then would be indexed in an empty vector. */
inline bool moment_ok(uint32_t mo, uint32_t nm, size_t xs)
{
    return mo == 0 || (nm > 1 && mo < nm && xs == size_t(nm - 1));
}

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
