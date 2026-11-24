/* mh2interp: see mh2interp.hpp. Part of the port (CC0 1.0). */
#include "mh2interp.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <utility>

namespace Mh2Interp
{
namespace
{
inline uint64_t ld64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
inline uint32_t ld32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline float ldf(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}
inline float u2f(uint32_t u)
{
    float v;
    memcpy(&v, &u, 4);
    return v;
}
inline void st16(uint8_t *p, uint32_t v)
{
    uint16_t w = uint16_t(v);
    memcpy(p, &w, 2);
}
inline void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
inline void stf(uint8_t *p, float v) { memcpy(p, &v, 4); }

constexpr uint64_t FNV_BASIS = 1469598103934665603ull;
inline uint64_t fnv(uint64_t h, uint64_t w)
{
    for (int i = 0; i < 8; i++)
    {
        h = (h ^ (w & 0xFF)) * 1099511628211ull;
        w >>= 8;
    }
    return h;
}

constexpr uint32_t REG_PRIM = 0, REG_RGBAQ = 1, REG_ST = 2, REG_UV = 3, REG_XYZF2 = 4, REG_XYZ2 = 5, REG_FOG = 0xA,
                   REG_XYZF3 = 0xC, REG_XYZ3 = 0xD, REG_XYOFFSET_1 = 0x18, REG_ALPHA_1 = 0x42, REG_TEST_1 = 0x47,
                   REG_TEX0_1 = 0x06, REG_TEX1_1 = 0x14;
constexpr uint32_t PRIV_DISPFB1 = 0x12000070, PRIV_DISPFB2 = 0x12000090;
}

Capture::Capture() { reset(); }

void Capture::reset()
{
    cur_.clear();
    pend_n_ = 0;
    for (auto &p : ps_)
        p = PathState();
    memset(reg_, 0, sizeof(reg_));
    prim_ = 0;
    memset(rgba_, 0, sizeof(rgba_));
    rgba_[3] = 0x80;
    s_ = t_ = 0;
    q_ = 1;
    rgb_off_ = st_off_ = NO_OFF;
    open_ = false;
    cur_path_ = 1;
    cur_model_ = -1;
    cur_anchor_ = -1;
    occ_.clear();
    occa_.clear();
}

/* The game builds a frame's draw lists (Model_Draw: the camera records) while the chain of the frame before is still to go, and the chain's
 * data comes at the start of the next interval, so the camera of the chain that arrives next is the first one received since the
 * path 1 data before it. A record that comes is held until the next path 1 data, which takes it (a step takes the first it finds). */
void Capture::camera(const float *m)
{
    if (pend_n_ == 0)
    {
        memcpy(pend_, m, sizeof(pend_));
        memcpy(pend_last_, m, sizeof(pend_last_));
        pend_n_ = 1;
        return;
    }
    if (memcmp(pend_last_, m, sizeof(pend_last_)))
    {
        memcpy(pend_last_, m, sizeof(pend_last_));
        pend_n_++;
    }
}

void Capture::anchor(uint32_t id, const float *point, const float *cam)
{
    close_kick();
    if (!id)
    {
        cur_anchor_ = -1;
        return;
    }
    /* the packets that follow one another with one id and one point are one element's (a text is many packets) */
    if (!cur_.anchors.empty() && cur_.anchors.back().id == id && !memcmp(cur_.anchors.back().point, point, sizeof(float) * 3))
    {
        cur_anchor_ = int32_t(cur_.anchors.size() - 1);
        return;
    }
    Anchor a;
    a.id = id;
    a.occ = occa_[id]++;
    memcpy(a.point, point, sizeof(a.point));
    std::array<float, 32> c;
    memcpy(c.data(), cam, sizeof(float) * 32);
    size_t i = cur_.cams.size();
    while (i > 0 && memcmp(cur_.cams[i - 1].data(), c.data(), sizeof(float) * 32))
        i--;
    if (i == 0)
    {
        cur_.cams.push_back(c);
        i = cur_.cams.size();
    }
    a.cam = uint32_t(i - 1);
    cur_anchor_ = int32_t(cur_.anchors.size());
    cur_.anchors.push_back(a);
}

void Capture::model(uint32_t handle, uint32_t flags, uint32_t prog, const float *world, const float *cam, uint32_t site)
{
    close_kick();
    if (!handle)
    {
        cur_model_ = -1;
        return;
    }
    ModelDraw m;
    m.handle = handle;
    m.flags = flags;
    m.prog = prog;
    m.site = site;
    m.occ = occ_[handle]++;
    m.first_kick = uint32_t(cur_.kicks.size());
    memcpy(m.world, world, sizeof(m.world));
    /* its camera: the one of the draw before when it is the same, else the same as an earlier draw's, else a new one */
    {
        std::array<float, 32> c;
        memcpy(c.data(), cam, sizeof(float) * 32);
        size_t i = cur_.cams.size();
        while (i > 0 && memcmp(cur_.cams[i - 1].data(), c.data(), sizeof(float) * 32))
            i--;
        if (i == 0)
        {
            cur_.cams.push_back(c);
            i = cur_.cams.size();
        }
        m.cam = uint32_t(i - 1);
    }
    cur_model_ = int32_t(cur_.models.size());
    cur_.models.push_back(m);
}

void Capture::close_kick()
{
    if (!open_)
        return;
    open_ = false;
    Kick &k = cur_.kicks.back();
    k.count = uint32_t(cur_.verts.size()) - k.first;
    k.key = fnv(key_head_, k.count);
    if (k.path == 1 && k.count)
    {
        cur_.p1_kicks++;
        if (k.model >= 0)
            cur_.models[k.model].nkicks++;
    }
    if (k.anchor >= 0 && k.count)
    {
        Anchor &an = cur_.anchors[k.anchor];
        an.nkicks++;
        if (an.first_kick < 0)
            an.first_kick = int32_t(cur_.kicks.size() - 1);
    }
}

void Capture::vertex(uint32_t x, uint32_t y, uint32_t z, uint32_t adc, uint32_t off, uint8_t fmt)
{
    if (!open_)
    {
        Kick k;
        k.path = uint8_t(cur_path_);
        k.prim = uint16_t(prim_ & 0x7FF);
        k.first = uint32_t(cur_.verts.size());
        k.count = 0;
        uint32_t c = ctx();
        uint64_t xyo = reg_[REG_XYOFFSET_1 + c];
        k.ofx = int32_t(xyo & 0xFFFF);
        k.ofy = int32_t((xyo >> 32) & 0xFFFF);
        uint64_t h = fnv(FNV_BASIS, (uint64_t(k.path) << 16) | k.prim);
        h = fnv(h, (prim_ & 0x10) ? reg_[REG_TEX0_1 + c] : 0);
        /* the block pointers' high bits ride in TEX1 (bits 48-55): two draws that differ only there differ */
        h = fnv(h, (prim_ & 0x10) ? (reg_[REG_TEX1_1 + c] >> 48) & 0xFF : 0);
        h = fnv(h, (prim_ & 0x40) ? reg_[REG_ALPHA_1 + c] : 0);
        h = fnv(h, reg_[REG_TEST_1 + c]);
        key_head_ = h;
        k.key = 0;
        k.model = cur_model_;
        k.anchor = cur_anchor_;
        cur_.kicks.push_back(k);
        open_ = true;
    }
    Vertex v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.r = rgba_[0];
    v.g = rgba_[1];
    v.b = rgba_[2];
    v.a = rgba_[3];
    v.s = s_;
    v.t = t_;
    v.q = q_;
    v.xyz_off = off;
    v.rgb_off = rgb_off_;
    v.st_off = st_off_;
    v.uv = uv_;
    v.fmt = fmt;
    v.adc = uint8_t(adc);
    cur_.verts.push_back(v);
}

void Capture::wreg(uint32_t addr, uint64_t v)
{
    switch (addr)
    {
    case REG_PRIM:
        close_kick();
        prim_ = v & 0x7FF;
        break;
    case REG_RGBAQ:
        rgba_[0] = uint8_t(v);
        rgba_[1] = uint8_t(v >> 8);
        rgba_[2] = uint8_t(v >> 16);
        rgba_[3] = uint8_t(v >> 24);
        q_ = u2f(uint32_t(v >> 32));
        rgb_off_ = NO_OFF;
        break;
    case REG_ST:
        s_ = u2f(uint32_t(v));
        t_ = u2f(uint32_t(v >> 32));
        st_off_ = NO_OFF;
        break;
    case REG_UV:
        uv_ = uint32_t(v & 0x3FFF) | (uint32_t((v >> 16) & 0x3FFF) << 16);
        break;
    case REG_FOG:
        break;
    case REG_XYZF2:
    case REG_XYZF3:
        vertex(uint32_t(v & 0xFFFF), uint32_t((v >> 16) & 0xFFFF), uint32_t((v >> 32) & 0xFFFFFF), 0, cur_off_, cur_off_ != NO_OFF ? 6 : 0);
        break;
    case REG_XYZ2:
    case REG_XYZ3:
        vertex(uint32_t(v & 0xFFFF), uint32_t((v >> 16) & 0xFFFF), uint32_t(v >> 32), 0, cur_off_, cur_off_ != NO_OFF ? 6 : 0);
        break;
    default:
        close_kick();
        if (addr < 0x80)
            reg_[addr] = v;
        break;
    }
}

void Capture::qword(PathState &ps, uint32_t path, const uint8_t *q, uint32_t off)
{
    cur_path_ = path;
    if (ps.left == 0)
    {
        uint64_t t = ld64(q);
        ps.regs = ld64(q + 8);
        ps.nloop = uint32_t(t & 0x7FFF);
        bool pre = (t >> 46) & 1;
        uint64_t prim = (t >> 47) & 0x7FF;
        ps.flg = uint32_t((t >> 58) & 3);
        ps.nreg = uint32_t((t >> 60) & 15);
        if (!ps.nreg)
            ps.nreg = 16;
        ps.idx = 0;
        if (ps.flg == 0)
            ps.left = ps.nloop * ps.nreg;
        else if (ps.flg == 1)
            ps.left = (ps.nloop * ps.nreg + 1) / 2;
        else
            ps.left = ps.nloop;
        if (pre && ps.flg < 2)
            wreg(REG_PRIM, prim);
        return;
    }
    if (ps.flg == 0)
    {
        uint32_t r = uint32_t((ps.regs >> (4 * (ps.idx % ps.nreg))) & 15);
        uint32_t w0 = ld32(q), w1 = ld32(q + 4), w2 = ld32(q + 8), w3 = ld32(q + 12);
        switch (r)
        {
        case 0:
            wreg(REG_PRIM, ld64(q));
            break;
        case 1:
            rgba_[0] = uint8_t(w0);
            rgba_[1] = uint8_t(w1);
            rgba_[2] = uint8_t(w2);
            rgba_[3] = uint8_t(w3);
            rgb_off_ = off;
            break;
        case 2:
            s_ = u2f(w0);
            t_ = u2f(w1);
            q_ = u2f(w2);
            st_off_ = off;
            break;
        case 3:
            uv_ = (w0 & 0x3FFF) | ((w1 & 0x3FFF) << 16);
            break;
        case 4:
        case 0xC:
            vertex(w0 & 0xFFFF, w1 & 0xFFFF, (w2 >> 4) & 0xFFFFFF, (w3 >> 15) & 1, off, 4);
            break;
        case 5:
        case 0xD:
            vertex(w0 & 0xFFFF, w1 & 0xFFFF, w2, (w3 >> 15) & 1, off, 5);
            break;
        case 6:
        case 7:
        case 8:
        case 9:
            wreg(r, ld64(q));
            break;
        case 0xA:
            break;
        case 0xE:
            cur_off_ = off;   /* an A+D write's data: a vertex it makes can be moved there (fmt 6: x and y are the 16 bits at +0 and +2) */
            wreg(q[8], ld64(q));
            cur_off_ = NO_OFF;
            break;
        default:
            break;
        }
    }
    else if (ps.flg == 1)
    {
        uint32_t items = ps.nloop * ps.nreg;
        for (uint32_t k = 0; k < 2; k++)
        {
            uint32_t item = ps.idx * 2 + k;
            if (item >= items)
                break;
            uint32_t r = uint32_t((ps.regs >> (4 * (item % ps.nreg))) & 15);
            uint64_t v = ld64(q + 8 * k);
            switch (r)
            {
            case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 0xA: case 0xC: case 0xD:
                cur_off_ = off + 8 * k;
                wreg(r, v);
                cur_off_ = NO_OFF;
                break;
            default:
                break;
            }
        }
    }
    ps.idx++;
    ps.left--;
}

void Capture::gif(uint32_t path, const void *data, size_t bytes)
{
    if (path < 1 || path > 3 || !bytes)
        return;
    bytes &= ~size_t(15);
    if (path == 1 && pend_n_)
    {
        if (cur_.ncam == 0)
        {
            memcpy(cur_.cam, pend_, sizeof(cur_.cam));
            memcpy(cur_.cam_last, pend_last_, sizeof(cur_.cam_last));
            cur_.ncam = pend_n_;
        }
        pend_n_ = 0;
    }
    uint32_t off = uint32_t(cur_.data.size());
    const uint8_t *p = static_cast<const uint8_t *>(data);
    cur_.data.insert(cur_.data.end(), p, p + bytes);
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EV_GIF;
    e.path = uint8_t(path);
    e.off = off;
    e.len = uint32_t(bytes);
    cur_.events.push_back(e);
    PathState &ps = ps_[path];
    for (size_t i = 0; i < bytes; i += 16)
        qword(ps, path, cur_.data.data() + off + i, off + uint32_t(i));
}

void Capture::priv(uint32_t addr, uint64_t value)
{
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EV_PRIV;
    e.addr = addr;
    e.value = value;
    cur_.events.push_back(e);
    if (addr == PRIV_DISPFB1 || addr == PRIV_DISPFB2)
        cur_.flip = true;
}

void Capture::fifo(uint32_t qwords)
{
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EV_FIFO;
    e.len = qwords;
    cur_.events.push_back(e);
}

void Capture::finish(Step &out)
{
    close_kick();
    out.clear();
    std::swap(out.data, cur_.data);
    std::swap(out.events, cur_.events);
    std::swap(out.kicks, cur_.kicks);
    std::swap(out.verts, cur_.verts);
    std::swap(out.models, cur_.models);
    std::swap(out.anchors, cur_.anchors);
    std::swap(out.cams, cur_.cams);
    out.p1_kicks = cur_.p1_kicks;
    out.flip = cur_.flip;
    memcpy(out.cam, cur_.cam, sizeof(out.cam));
    memcpy(out.cam_last, cur_.cam_last, sizeof(out.cam_last));
    out.ncam = cur_.ncam;
    out.cam_at = cur_.cam_at;
    cur_.clear();
    cur_model_ = -1;
    cur_anchor_ = -1;
    occ_.clear();
    occa_.clear();
    /* a vertex's offsets into the data point into the step that is gone: the next vertex written by an old
     * RGBAQ or ST register must not write there */
    rgb_off_ = st_off_ = NO_OFF;
}

static uint64_t mkey(int32_t model, uint64_t key) { return fnv(fnv(FNV_BASIS, uint64_t(uint32_t(model))), key); }

void Index::build(const Step &s)
{
    clear();
    for (uint32_t i = 0; i < s.models.size(); i++)
        by_model[(uint64_t(s.models[i].handle) << 32) | s.models[i].occ] = int32_t(i);
    for (uint32_t i = 0; i < s.anchors.size(); i++)
        by_anchor[(uint64_t(s.anchors[i].id) << 32) | s.anchors[i].occ] = int32_t(i);
    for (uint32_t i = 0; i < s.kicks.size(); i++)
    {
        const Kick &k = s.kicks[i];
        if (k.path != 1 || !k.count)
            continue;
        by_key[k.key].push_back(i);
        if (k.model >= 0)
            by_mkey[mkey(k.model, k.key)].push_back(i);
        else
            by_key_unl[k.key].push_back(i);
    }
}

namespace
{
bool packed(const Vertex &v) { return (v.fmt == 4 || v.fmt == 5) && v.xyz_off != NO_OFF; }

inline int32_t rel(uint32_t v, int32_t of) { return int32_t(v & 0xFFFF) - of; }

/* ---- the camera (MH2IPC_REC_CAMERA: view and projection, row-major, a row vector times the matrix) ---- */

void mmul(const double *a, const double *b, double *o)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

/* the camera's world-to-screen matrix, view then projection */
void cam_screen(const double *c, double *o) { mmul(c, c + 16, o); }

/* the eye, and the rotation (rows: where the world's axes go) of a rigid view matrix; false when it is not one */
bool view_split(const double *v, double eye[3], double r[9])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = v[i * 4 + j];
    /* orthonormal rows */
    for (int i = 0; i < 3; i++)
        for (int k = i; k < 3; k++)
        {
            double d = r[i * 3] * r[k * 3] + r[i * 3 + 1] * r[k * 3 + 1] + r[i * 3 + 2] * r[k * 3 + 2];
            if (std::fabs(d - (i == k ? 1.0 : 0.0)) > 1e-3)
                return false;
        }
    if (std::fabs(v[3]) > 1e-6 || std::fabs(v[7]) > 1e-6 || std::fabs(v[11]) > 1e-6 || std::fabs(v[15] - 1.0) > 1e-6)
        return false;
    /* p_view = p R + t, so the eye (p_view = 0) is -t R^T */
    for (int j = 0; j < 3; j++)
        eye[j] = -(v[12] * r[j * 3] + v[13] * r[j * 3 + 1] + v[14] * r[j * 3 + 2]);
    return true;
}

/* the rotation as a quaternion (x y z w) of the column convention (the transpose of the rows') */
void rot_quat(const double *r, double q[4])
{
    const double m00 = r[0], m11 = r[4], m22 = r[8];
    /* the column convention's matrix is r transposed: M(i,j) = r[j*3+i] */
    auto M = [&](int i, int j) { return r[j * 3 + i]; };
    double tr = m00 + m11 + m22;
    if (tr > 0)
    {
        double s = std::sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s;
        q[0] = (M(2, 1) - M(1, 2)) / s;
        q[1] = (M(0, 2) - M(2, 0)) / s;
        q[2] = (M(1, 0) - M(0, 1)) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q[3] = (M(2, 1) - M(1, 2)) / s;
        q[0] = 0.25 * s;
        q[1] = (M(0, 1) + M(1, 0)) / s;
        q[2] = (M(0, 2) + M(2, 0)) / s;
    }
    else if (m11 > m22)
    {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q[3] = (M(0, 2) - M(2, 0)) / s;
        q[0] = (M(0, 1) + M(1, 0)) / s;
        q[1] = 0.25 * s;
        q[2] = (M(1, 2) + M(2, 1)) / s;
    }
    else
    {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q[3] = (M(1, 0) - M(0, 1)) / s;
        q[0] = (M(0, 2) + M(2, 0)) / s;
        q[1] = (M(1, 2) + M(2, 1)) / s;
        q[2] = 0.25 * s;
    }
}

void quat_rot(const double *q, double *r)
{
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    /* the column convention's matrix, stored as the rows of the row convention (transposed) */
    double M[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                   2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                   2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[j * 3 + i] = M[i * 3 + j];
}

/* The angle (degrees) between two quaternions' rotations. */
double quat_deg(const double *a, const double *b)
{
    double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    d = d > 1 ? 1 : d;
    return 2.0 * std::acos(d) * 180.0 / 3.14159265358979323846;
}

/* The camera of two steps, t of the way from a to b: the eye moves in a line, the rotation by slerp, the projection entry by entry
 * (the field of view changes). A view that is not rigid is blended entry by entry. */
void cam_blend(const double *a, const double *b, double t, double *o)
{
    for (int i = 16; i < 32; i++)
        o[i] = a[i] * (1 - t) + b[i] * t;
    double ea[3], eb[3], ra[9], rb[9];
    if (!view_split(a, ea, ra) || !view_split(b, eb, rb))
    {
        for (int i = 0; i < 16; i++)
            o[i] = a[i] * (1 - t) + b[i] * t;
        return;
    }
    double qa[4], qb[4], q[4];
    rot_quat(ra, qa);
    rot_quat(rb, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0)
        for (int i = 0; i < 4; i++)
            qb[i] = -qb[i], d = -d;
    double wa = 1 - t, wb = t;
    if (d < 0.9995)
    {
        double th = std::acos(d), s = std::sin(th);
        wa = std::sin((1 - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    double n = 0;
    for (int i = 0; i < 4; i++)
    {
        q[i] = wa * qa[i] + wb * qb[i];
        n += q[i] * q[i];
    }
    n = 1.0 / std::sqrt(n);
    for (int i = 0; i < 4; i++)
        q[i] *= n;
    double r[9], eye[3];
    quat_rot(q, r);
    for (int j = 0; j < 3; j++)
        eye[j] = ea[j] * (1 - t) + eb[j] * t;
    for (int i = 0; i < 16; i++)
        o[i] = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            o[i * 4 + j] = r[i * 3 + j];
    o[15] = 1;
    /* t = -eye R */
    for (int j = 0; j < 3; j++)
        o[12 + j] = -(eye[0] * r[j] + eye[1] * r[3 + j] + eye[2] * r[6 + j]);
}

/* A screen vertex (x, y in the GS's window units, w from its Q or its z) to the world point whose projection by A it is: three linear equations in the
 * point. false: a degenerate system. */
bool unproject(const double *A, double sx, double sy, double sz, double w, bool by_z, double out[3])
{
    double m[3][4];
    for (int i = 0; i < 3; i++)
    {
        m[0][i] = A[i * 4 + 0] - sx * A[i * 4 + 3];
        m[1][i] = A[i * 4 + 1] - sy * A[i * 4 + 3];
        m[2][i] = by_z ? A[i * 4 + 2] - sz * A[i * 4 + 3] : A[i * 4 + 3];
    }
    m[0][3] = -(A[12] - sx * A[15]);
    m[1][3] = -(A[13] - sy * A[15]);
    m[2][3] = by_z ? -(A[14] - sz * A[15]) : w - A[15];
    auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
        return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    };
    double D = det3(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
    if (!(std::fabs(D) > 1e-18))
        return false;
    out[0] = det3(m[0][3], m[0][1], m[0][2], m[1][3], m[1][1], m[1][2], m[2][3], m[2][1], m[2][2]) / D;
    out[1] = det3(m[0][0], m[0][3], m[0][2], m[1][0], m[1][3], m[1][2], m[2][0], m[2][3], m[2][2]) / D;
    out[2] = det3(m[0][0], m[0][1], m[0][3], m[1][0], m[1][1], m[1][3], m[2][0], m[2][1], m[2][3]) / D;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

/* the world point through the matrix A: window x, y, z and w; false behind the eye */
bool project(const double *A, const double p[3], double *sx, double *sy, double *sz, double *w)
{
    double hx = p[0] * A[0] + p[1] * A[4] + p[2] * A[8] + A[12];
    double hy = p[0] * A[1] + p[1] * A[5] + p[2] * A[9] + A[13];
    double hz = p[0] * A[2] + p[1] * A[6] + p[2] * A[10] + A[14];
    double hw = p[0] * A[3] + p[1] * A[7] + p[2] * A[11] + A[15];
    if (!(hw > 1e-9))
        return false;
    *sx = hx / hw;
    *sy = hy / hw;
    *sz = hz / hw;
    *w = hw;
    return true;
}
}

bool plan_interp(const Step &base, const Step &a, const Index &ia, const Step &b, const Index &ib, const Params &p, Plan &plan)
{
    Result &res = plan.res;
    res = Result();
    std::vector<Decision> &dec = plan.dec;
    dec.assign(base.kicks.size(), Decision());
    std::unordered_map<uint64_t, uint32_t> occ, occm, occu;
    const bool live = (&b == &base);
    const double gate2 = double(p.gate_px) * 16.0 * double(p.gate_px) * 16.0;
    /* The camera: a kick with no pair, or gated, goes with it (what 3) when both steps have one and it did not jump (a camera cut). */
    plan.cam_ok = false;
    plan.wpt.clear();
    plan.whw.clear();
    plan.wok.clear();
    plan.wa3.clear();
    plan.wb3.clear();
    plan.wbok.clear();
    const bool live_cam_skip = false;
    double An[16];
    if (p.reproject && a.ncam > 0 && b.ncam > 0 && base.ncam > 0 && (!p.rp_single || (a.ncam == 1 && b.ncam == 1 && base.ncam == 1)))
    {
        double ea[3], eb[3], ra[9], rb[9];
        double ca[32], cb[32];
        for (int i = 0; i < 32; i++)
        {
            ca[i] = a.cam[i];
            cb[i] = b.cam[i];
            plan.cam_a[i] = ca[i];
            plan.cam_b[i] = cb[i];
            plan.cam_n[i] = base.cam[i];
        }
        if (view_split(ca, ea, ra) && view_split(cb, eb, rb))
        {
            double qa[4], qb[4];
            rot_quat(ra, qa);
            rot_quat(rb, qb);
            res.cam_eye = std::sqrt((ea[0] - eb[0]) * (ea[0] - eb[0]) + (ea[1] - eb[1]) * (ea[1] - eb[1]) + (ea[2] - eb[2]) * (ea[2] - eb[2]));
            res.cam_deg = quat_deg(qa, qb);
            if (res.cam_eye <= p.cam_cut_eye && res.cam_deg <= p.cam_cut_deg)
            {
                cam_screen(plan.cam_n, An);
                plan.cam_ok = true;
                plan.wpt.assign(base.verts.size() * 3, 0.0f);
                plan.whw.assign(base.verts.size(), 0.0f);
                plan.wok.assign(base.verts.size(), 0);
                if (p.world_blend)
                {
                    plan.wa3.assign(base.verts.size() * 3, 0.0f);
                    plan.wb3.assign(base.verts.size() * 3, 0.0f);
                    plan.wbok.assign(base.verts.size(), 0);
                }
            }
        }
    }
    /* does the camera account for the kick's motion between steps a and b (still world geometry)? Its vertices of step b, unprojected with b's
     * camera and projected with a's, land within p.rp_tol px of step a's, for nine in ten of a sample of them */
    double Aa_still[16];
    if (plan.cam_ok)
        cam_screen(plan.cam_a, Aa_still);
    auto still = [&](uint32_t i, const Kick &ka, const Kick &kb, double tol) -> bool {
        const Kick &k = base.kicks[i];
        const uint32_t step = (k.count > 12 && !p.still_all) ? k.count / 12 : 1;
        uint32_t n = 0, good = 0;
        double Ab[16];
        cam_screen(plan.cam_b, Ab);
        for (uint32_t j = 0; j < k.count; j += step)
        {
            const Vertex &vb = b.verts[kb.first + j], &va = a.verts[ka.first + j];
            if (vb.st_off == NO_OFF || !(vb.q > 1e-12f) || !std::isfinite(vb.q))
                continue;
            double pt[3], sx, sy, sz, w;
            n++;
            if (!unproject(Ab, double(vb.x & 0xFFFF) / 16.0, double(vb.y & 0xFFFF) / 16.0, double(vb.z), 1.0 / double(vb.q), false, pt) || !project(Aa_still, pt, &sx, &sy, &sz, &w))
                continue;
            const double ex = sx - double(va.x & 0xFFFF) / 16.0, ey = sy - double(va.y & 0xFFFF) / 16.0;
            good += std::sqrt(ex * ex + ey * ey) <= tol;
        }
        return n >= 2 && (p.still_all ? good == n : good * 10 >= n * 9);
    };
    /* what 3: the kick's world points, from the step's camera; false (the kick stays as it was decided) when the camera is not good or a vertex is not a packed one */
    auto md_dbg = [&](const Kick &k) { return k.model >= 0; };
    /* a labelled draw that is fixed to the camera: its matrix times the view (the draw in the camera's space) is the same in a and b to within a share of the change
     * the camera's move alone would have made (the draw's matrix of a with the view of b). Then its screen place does not follow the camera, and the camera must not move it. */
    std::vector<int8_t> att_of(base.models.size(), -1);
    std::vector<std::array<double, 3>> att_dbg(base.models.size(), std::array<double, 3>{{-1, -1, -1}});
    auto attached = [&](int32_t mi) -> bool {
        if (p.attach <= 0 || mi < 0)
            return false;
        int8_t &r = att_of[size_t(mi)];
        if (r >= 0)
            return r != 0;
        r = 0;
        const ModelDraw &md = base.models[size_t(mi)];
        const uint64_t mk = (uint64_t(md.handle) << 32) | md.occ;
        auto fa = ia.by_model.find(mk);
        auto fb2 = live ? ia.by_model.end() : ib.by_model.find(mk);
        if (fa == ia.by_model.end() || (!live && fb2 == ib.by_model.end()))
            return false;
        const ModelDraw &ma = a.models[size_t(fa->second)];
        const ModelDraw &mb = live ? md : b.models[size_t(fb2->second)];
        const Step &sb = live ? base : b;
        if (ma.cam >= a.cams.size() || mb.cam >= sb.cams.size())
            return false;
        double Ma[16], Va[16], Mb[16], Vb[16], MVa[16], MVb[16], MVf[16];
        for (int q = 0; q < 16; q++)
        {
            Ma[q] = ma.world[q];
            Mb[q] = mb.world[q];
            Va[q] = a.cams[ma.cam][size_t(q)];
            Vb[q] = sb.cams[mb.cam][size_t(q)];
        }
        mmul(Ma, Va, MVa);
        mmul(Mb, Vb, MVb);
        mmul(Ma, Vb, MVf);
        /* where the draw's origin is in the camera's space (the matrix's last row), and how far its axes turned (the 3 x 3 part): what the camera's move alone would have changed
         * (the draw's matrix of a with the view of b) against what changed; a turn counts as the distance of the draw times the angle */
        double dist = std::sqrt(MVa[12] * MVa[12] + MVa[13] * MVa[13] + MVa[14] * MVa[14]);
        double sc = std::sqrt(MVa[0] * MVa[0] + MVa[1] * MVa[1] + MVa[2] * MVa[2]);   /* the draw's scale in the matrix: a turn of the unit vectors is this big */
        if (!(sc > 1e-9))
            return false;
        double tf = 0, ta = 0, rf = 0, ra = 0;
        for (int q = 0; q < 3; q++)
        {
            tf += (MVf[12 + q] - MVa[12 + q]) * (MVf[12 + q] - MVa[12 + q]);
            ta += (MVb[12 + q] - MVa[12 + q]) * (MVb[12 + q] - MVa[12 + q]);
        }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
            {
                rf += (MVf[i * 4 + j] - MVa[i * 4 + j]) * (MVf[i * 4 + j] - MVa[i * 4 + j]);
                ra += (MVb[i * 4 + j] - MVa[i * 4 + j]) * (MVb[i * 4 + j] - MVa[i * 4 + j]);
            }
        const double fixm = std::sqrt(tf) + dist * std::sqrt(rf) / sc, actm = std::sqrt(ta) + dist * std::sqrt(ra) / sc;
        att_dbg[size_t(mi)] = {{fixm, actm, dist}};
        r = (fixm >= p.attach_min && actm <= p.attach * fixm) ? 1 : 0;
        if (p.c3log)
            fprintf(p.c3log, "attached %d h %u occ %u site %x fix %g act %g dist %g\n", int(r), md.handle, md.occ, md.site, fixm, actm, dist);
        return r != 0;
    };
    int c3why = 0;   /* (diagnostics) why the kick is being tried with the camera: 0 no pair, 1 count, 2 format, 3 ADC, 4 gated */
    auto try_cam = [&](uint32_t i) -> bool {
        if (!plan.cam_ok || i < p.c3lo || i >= p.c3hi)
            return false;
        const Kick &k = base.kicks[i];
        if (attached(k.model))
        {
            res.att_kicks++;
            dec[i].att = 1;
            return false;
        }
        for (uint32_t j = 0; j < k.count; j++)
            if (!packed(base.verts[k.first + j]))
                return false;
        uint32_t good = 0;
        for (uint32_t j = 0; j < k.count; j++)
        {
            const uint32_t idx = k.first + j;
            const Vertex &v = base.verts[idx];
            double pt[3];
            const bool has_q = v.st_off != NO_OFF && v.q > 1e-12f && std::isfinite(v.q);
            const bool by_z = p.rp_w == 1 || !has_q;
            const double sx = double(v.x & 0xFFFF) / 16.0, sy = double(v.y & 0xFFFF) / 16.0;
            if (!unproject(An, sx, sy, double(v.z), has_q ? 1.0 / double(v.q) : 0.0, by_z, pt))
                continue;
            const double hw = pt[0] * An[3] + pt[1] * An[7] + pt[2] * An[11] + An[15];
            if (!(hw > 1e-9))
                continue;
            plan.wpt[idx * 3] = float(pt[0]);
            plan.wpt[idx * 3 + 1] = float(pt[1]);
            plan.wpt[idx * 3 + 2] = float(pt[2]);
            plan.whw[idx] = float(hw);
            plan.wok[idx] = 1;
            good++;
        }
        (void)good;
        if (p.c3log)
        {
            const Vertex &v0 = base.verts[k.first];
            const ModelDraw *md = k.model >= 0 ? &base.models[size_t(k.model)] : nullptr;
            fprintf(p.c3log, "c3 kick %u count %u prim %x key %llx why %d good %u model h %u occ %u site %x flags %x | v0 x %d y %d z %u rgba %u %u %u %u stq %g %g %g fmt %u adc %u | vN x %d y %d z %u\n", i, k.count,
                    unsigned(k.prim), (unsigned long long)k.key, c3why, good, md ? md->handle : 0, md ? md->occ : 0, md ? md->site : 0, md ? md->flags : 0, rel(v0.x, k.ofx) / 16, rel(v0.y, k.ofy) / 16, v0.z,
                    v0.r, v0.g, v0.b, v0.a, double(v0.s), double(v0.t), double(v0.q), v0.fmt, v0.adc, rel(base.verts[k.first + k.count - 1].x, k.ofx) / 16, rel(base.verts[k.first + k.count - 1].y, k.ofy) / 16,
                    base.verts[k.first + k.count - 1].z);
        }
        if (p.c3log && md_dbg(k))
        {
            const ModelDraw &md = base.models[size_t(k.model)];
            auto fa = ia.by_model.find((uint64_t(md.handle) << 32) | md.occ);
            auto fb2 = live ? ia.by_model.end() : ib.by_model.find((uint64_t(md.handle) << 32) | md.occ);
            if (fa != ia.by_model.end() && (live || fb2 != ib.by_model.end()))
            {
                const ModelDraw &ma = a.models[size_t(fa->second)];
                const ModelDraw &mb = live ? md : b.models[size_t(fb2->second)];
                double Ma[16], Va[16], Mb[16], Vb[16], MVa[16], MVb[16];
                for (int q = 0; q < 16; q++)
                {
                    Ma[q] = ma.world[q];
                    Mb[q] = mb.world[q];
                    Va[q] = a.cams[ma.cam][size_t(q)];
                    Vb[q] = (live ? base : b).cams[mb.cam][size_t(q)];
                }
                mmul(Ma, Va, MVa);
                mmul(Mb, Vb, MVb);
                double dM = 0, dV = 0, dMV = 0;
                for (int q = 0; q < 16; q++)
                {
                    dM = std::max(dM, std::fabs(Ma[q] - Mb[q]));
                    dV = std::max(dV, std::fabs(Va[q] - Vb[q]));
                    dMV = std::max(dMV, std::fabs(MVa[q] - MVb[q]));
                }
                fprintf(p.c3log, "   attach k %u: fix %g act %g dist %g | dM %g dV %g dMV %g | M a trans %g %g %g b %g %g %g | MV a trans %g %g %g b %g %g %g\n", i, att_dbg[size_t(k.model)][0], att_dbg[size_t(k.model)][1], att_dbg[size_t(k.model)][2], dM, dV, dMV, Ma[12], Ma[13], Ma[14], Mb[12], Mb[13], Mb[14],
                        MVa[12], MVa[13], MVa[14], MVb[12], MVb[13], MVb[14]);
            }
        }
        dec[i].what = 3;
        dec[i].why = uint8_t(c3why);
        res.k_c3[c3why]++;
        res.v_c3[c3why] += k.count;
        res.reproj++;
        res.v_reproj += k.count;
        return true;
    };
    /* what 1 with the camera: the kick's vertices as world points, each step unprojected with its own camera (the two steps' pair, vertex by vertex) */
    double Ab_w[16];
    if (plan.cam_ok && p.world_blend)
        cam_screen(plan.cam_b, Ab_w);
    auto world_pair = [&](uint32_t i, const Kick &ka, const Kick &kb) {
        if (!plan.cam_ok || !p.world_blend)
            return;
        const Kick &k = base.kicks[i];
        for (uint32_t j = 0; j < k.count; j++)
        {
            const Vertex &va = a.verts[ka.first + j], &vb = b.verts[kb.first + j];
            if (va.st_off == NO_OFF || vb.st_off == NO_OFF || !(va.q > 1e-12f) || !(vb.q > 1e-12f) || !std::isfinite(va.q) || !std::isfinite(vb.q))
                continue;
            double pa[3], pb[3];
            if (!unproject(Aa_still, double(va.x & 0xFFFF) / 16.0, double(va.y & 0xFFFF) / 16.0, double(va.z), 1.0 / double(va.q), false, pa))
                continue;
            if (!unproject(Ab_w, double(vb.x & 0xFFFF) / 16.0, double(vb.y & 0xFFFF) / 16.0, double(vb.z), 1.0 / double(vb.q), false, pb))
                continue;
            const uint32_t idx = k.first + j;
            for (int q = 0; q < 3; q++)
            {
                plan.wa3[idx * 3 + q] = float(pa[q]);
                plan.wb3[idx * 3 + q] = float(pb[q]);
            }
            plan.wbok[idx] = 1;
        }
    };
    /* every vertex of the pair moves alike: within tol px of the mean displacement of the kick's vertices (screen, b minus a) */
    auto coherent = [&](uint32_t i, const Kick &ka, const Kick &kb, double tol) -> bool {
        const Kick &k = base.kicks[i];
        if (k.count < 3)
            return false;
        double mx = 0, my = 0;
        for (uint32_t j = 0; j < k.count; j++)
        {
            mx += double(rel(b.verts[kb.first + j].x, kb.ofx)) - double(rel(a.verts[ka.first + j].x, ka.ofx));
            my += double(rel(b.verts[kb.first + j].y, kb.ofy)) - double(rel(a.verts[ka.first + j].y, ka.ofy));
        }
        mx /= k.count * 16.0;
        my /= k.count * 16.0;
        for (uint32_t j = 0; j < k.count; j++)
        {
            const double dx = double(rel(b.verts[kb.first + j].x, kb.ofx) - rel(a.verts[ka.first + j].x, ka.ofx)) / 16.0 - mx;
            const double dy = double(rel(b.verts[kb.first + j].y, kb.ofy) - rel(a.verts[ka.first + j].y, ka.ofy)) / 16.0 - my;
            if (dx * dx + dy * dy > tol * tol)
                return false;
        }
        return true;
    };
    uint64_t hkey = 0;
    bool hkey_ok = false;
    /* the still shortcut for a paired kick: the camera accounts for its motion (to within rp_tol, or the tolerance its last class asks for) */
    auto hst_step = [&](uint32_t i, const Kick &ka, const Kick &kb, bool lbl, uint32_t om) -> bool {
        hkey_ok = false;
        if (p.no_still || !p.rp_paired || !plan.cam_ok || live_cam_skip)
            return false;
        if (p.mat_class && lbl)
        {
            /* classified by the draw's identity: a model whose matrix changed between the steps is moving, whatever 0.5 px says of this kick */
            const ModelDraw &md = base.models[base.kicks[i].model];
            auto fm = ia.by_model.find((uint64_t(md.handle) << 32) | md.occ);
            if (fm == ia.by_model.end() || memcmp(a.models[fm->second].world, md.world, sizeof(float) * 16) != 0)
                return false;
        }
        double tol = p.rp_tol;
        if (p.hmap && lbl)
        {
            const Kick &k = base.kicks[i];
            const ModelDraw &md = base.models[k.model];
            hkey = fnv(fnv(fnv(FNV_BASIS, md.handle), md.occ), fnv(k.key, om));
            hkey_ok = true;
            auto f = p.hmap->find(hkey);
            if (f != p.hmap->end())
                tol = f->second == 3 ? p.rp_tol * p.hyst : p.rp_tol / p.hyst;
        }
        const bool st = still(i, ka, kb, tol);
        if (st && hkey_ok)
            (*p.hmap)[hkey] = 3;
        return st;
    };
    for (uint32_t i = 0; i < base.kicks.size(); i++)
    {
        const Kick &k = base.kicks[i];
        if (k.path != 1 || !k.count)
            continue;
        res.kicks++;
        res.verts += k.count;
        /* the vertices in (or near) the picture: only they count for the cut, as only they are seen */
        uint32_t on = 0;
        for (uint32_t j = 0; j < k.count; j++)
        {
            const Vertex &v = base.verts[k.first + j];
            int32_t px = rel(v.x, k.ofx), py = rel(v.y, k.ofy);
            on += px >= -64 * 16 && px <= 704 * 16 && py >= -64 * 16 && py <= 512 * 16;
        }
        res.v_on += on;
        uint32_t o = occ[k.key]++;
        Decision &d = dec[i];
        /* the partners: with the labels, a labelled kick is paired inside the model draw (same handle and occurrence in the other step) by key and
         * occurrence in the draw, and a kick no draw labels by key and occurrence among those; without, by key and occurrence in the whole step */
        const bool lbl = p.labels && k.model >= 0;
        uint32_t om = 0, ou = 0;
        if (p.labels)
        {
            if (k.model >= 0)
                om = occm[mkey(k.model, k.key)]++;
            else
                ou = occu[k.key]++;
            if (k.model >= 0)
                res.lb_kicks++;
            else
                res.lb_unl++;
        }
        auto find = [&](const Step &s, const Index &ix, int32_t &out) -> bool {
            out = -1;
            if (lbl)
            {
                const ModelDraw &md = base.models[k.model];
                auto fm = ix.by_model.find((uint64_t(md.handle) << 32) | md.occ);
                if (fm == ix.by_model.end())
                    return false;
                auto fk = ix.by_mkey.find(mkey(fm->second, k.key));
                if (fk == ix.by_mkey.end() || om >= fk->second.size())
                    return false;
                out = int32_t(fk->second[om]);
                return true;
            }
            const auto &map = p.labels ? ix.by_key_unl : ix.by_key;
            auto fk = map.find(k.key);
            const uint32_t oo = p.labels ? ou : o;
            if (fk == map.end() || oo >= fk->second.size())
                return false;
            out = int32_t(fk->second[oo]);
            (void)s;
            return true;
        };
        int32_t pa = -1, pb = -1;
        bool fa_ok = find(a, ia, pa);
        bool fb_ok = live ? true : find(b, ib, pb);
        if (live)
            pb = int32_t(i);
        if (lbl && p.stats_labels)
        {
            /* (diagnostics) how the labels' partners differ from key and occurrence in the whole step */
            auto fla = ia.by_key.find(k.key);
            int32_t la = (fla != ia.by_key.end() && o < fla->second.size()) ? int32_t(fla->second[o]) : -1;
            int32_t lb_ = -1;
            if (live)
                lb_ = int32_t(i);
            else
            {
                auto flb = ib.by_key.find(k.key);
                lb_ = (flb != ib.by_key.end() && o < flb->second.size()) ? int32_t(flb->second[o]) : -1;
            }
            if (la != pa || lb_ != pb)
            {
                res.lb_diff++;
                res.v_lb_diff += k.count;
            }
            if (!fa_ok || !fb_ok)
                res.lb_nodraw++;
        }
        if (!fa_ok)
        {
            c3why = 0;
            if (try_cam(i))
            {
                d.born = 1;
                res.born_kicks++;
                continue;
            }
            res.unmatched++;
            res.un_key++;
            res.v_un_key += k.count;
            res.v_unmatched += k.count;
            res.v_on_bad += on;
            continue;
        }
        d.ka = pa;
        d.kb = pb;
        if (!fb_ok)
        {
            d.kb = -1;
            c3why = 0;
            if (try_cam(i))
                continue;
            res.unmatched++;
            res.v_unmatched += k.count;
            res.v_on_bad += on;
            continue;
        }
        const Kick &ka = a.kicks[d.ka];
        const Kick &kb = b.kicks[d.kb];
        bool ok = ka.count == k.count && kb.count == k.count;
        bool far = false, far_on = false;
        int why = ok ? 0 : 1;
        for (uint32_t j = 0; ok && j < k.count; j++)
        {
            const Vertex &v = base.verts[k.first + j];
            const Vertex &va = a.verts[ka.first + j];
            const Vertex &vb = b.verts[kb.first + j];
            if (!packed(v) || v.fmt != va.fmt || v.fmt != vb.fmt || (p.same_adc && (v.adc != va.adc || v.adc != vb.adc)))
            {
                ok = false;
                why = (v.fmt != va.fmt || v.fmt != vb.fmt) ? 2 : 3;
                break;
            }
            double dx = double(rel(vb.x, kb.ofx)) - double(rel(va.x, ka.ofx));
            double dy = double(rel(vb.y, kb.ofy)) - double(rel(va.y, ka.ofy));
            if (dx * dx + dy * dy > gate2)
            {
                int32_t px = rel(v.x, k.ofx), py = rel(v.y, k.ofy);
                bool in_picture = px >= -64 * 16 && px <= 704 * 16 && py >= -64 * 16 && py <= 512 * 16;
                far_on = far_on || in_picture;
                if (!p.gate_on_screen || in_picture)
                    far = true;
            }
        }
        c3why = why >= 1 && why <= 3 ? why : 1;
        if (!ok && try_cam(i))
            ;
        else if (!ok)
        {
            res.unmatched++;
            (why == 1 ? res.un_cnt : why == 2 ? res.un_fmt : res.un_adc)++;
            if (why == 1)
                res.v_un_cnt += k.count;
            res.v_unmatched += k.count;
            res.v_on_bad += on;
        }
        else if (far && p.far_coh > 0 && plan.cam_ok && !still(i, ka, kb, p.rp_tol) && coherent(i, ka, kb, p.far_coh))
        {
            /* a coherent move the camera does not explain: the kick's own motion, not a mispairing */
            if (hkey_ok)
                (*p.hmap)[hkey] = 1;
            d.what = 1;
            world_pair(i, ka, kb);
            res.interpolated++;
            res.v_interpolated += k.count;
            res.far_int++;
        }
        else if (far && (c3why = 4, try_cam(i)))
            ;
        else if (far)
        {
            d.what = 2;
            res.gated++;
            res.v_gated += k.count;
            /* a clipped strip has vertices that jump from step to step far outside the picture: a kick counts for the cut
             * only when a vertex in the picture is what moves far */
            if (far_on)
            {
                res.v_on_bad += on;
                res.g_on++;
            }
        }
        else if (i < p.klo || i >= p.khi)
            d.what = 2;
        else if ((c3why = 5, hst_step(i, ka, kb, lbl, om)) && try_cam(i))
            ;
        else
        {
            if (hkey_ok)
                (*p.hmap)[hkey] = 1;
            d.what = 1;
            world_pair(i, ka, kb);
            res.interpolated++;
            res.v_interpolated += k.count;
        }
    }
    if (p.stats && plan.cam_ok)
    {
        /* (tests) how well the camera predicts the vertices of the pairs: step b's vertices unprojected with its camera and projected with step a's */
        double Aa[16];
        cam_screen(plan.cam_a, Aa);
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            const Decision &d = dec[i];
            if ((d.what < 1 || d.what > 3) || d.ka < 0 || d.kb < 0)
                continue;
            const Kick &k = base.kicks[i], &ka = a.kicks[d.ka], &kb = b.kicks[d.kb];
            if (ka.count != k.count || kb.count != k.count)
                continue;
            uint32_t within = 0;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const Vertex &vb = b.verts[kb.first + j], &va = a.verts[ka.first + j];
                double pt[3], sx, sy, sz, w;
                const bool has_q = vb.st_off != NO_OFF && vb.q > 1e-12f && std::isfinite(vb.q);
                double Ab[16];
                cam_screen(plan.cam_b, Ab);
                for (int zz = 0; zz < 2; zz++)
                {
                    if (zz == 0 && !has_q)
                        continue;
                    if (!unproject(Ab, double(vb.x & 0xFFFF) / 16.0, double(vb.y & 0xFFFF) / 16.0, double(vb.z), has_q ? 1.0 / double(vb.q) : 0.0, zz == 1, pt) ||
                        !project(Aa, pt, &sx, &sy, &sz, &w))
                    {
                        res.rp_bad++;
                        continue;
                    }
                    const double ex = sx - double(va.x & 0xFFFF) / 16.0, ey = sy - double(va.y & 0xFFFF) / 16.0;
                    const double e = std::sqrt(ex * ex + ey * ey);
                    (zz ? res.rpz_err : res.rp_err)[e <= 0.125 ? 0 : e <= 0.25 ? 1 : e <= 0.5 ? 2 : e <= 1 ? 3 : e <= 2 ? 4 : e <= 8 ? 5 : 6]++;
                    within += zz == 0 && e <= 1.0;
                }
            }
            (within * 10 >= k.count * 9 ? res.rpk_ok : res.rpk_bad)++;
            (within * 10 >= k.count * 9 ? res.rpv_ok : res.rpv_bad) += k.count;
        }
    }
    /* The anchors (2D elements the game places from a 3D point): the ones that a and b both have (by id and occurrence), with the camera of each and the
     * point of each, when the camera did not jump and the point is on the screen in both and did not jump either; what alpha does with them is apply_interp's. */
    plan.anc.clear();
    if (p.anchors && !base.anchors.empty())
    {
        plan.anc.assign(base.anchors.size(), Plan::AnchorPlan());
        for (uint32_t ai = 0; ai < base.anchors.size(); ai++)
        {
            const Anchor &an = base.anchors[ai];
            const uint64_t key = (uint64_t(an.id) << 32) | an.occ;
            auto fa = ia.by_anchor.find(key);
            if (fa == ia.by_anchor.end())
                continue;
            int32_t ib_ = int32_t(ai);
            if (!live)
            {
                auto fb_ = ib.by_anchor.find(key);
                if (fb_ == ib.by_anchor.end())
                    continue;
                ib_ = fb_->second;
            }
            const Anchor &A = a.anchors[fa->second], &B = b.anchors[ib_];
            if (A.cam >= a.cams.size() || B.cam >= b.cams.size() || an.cam >= base.cams.size())
                continue;
            Plan::AnchorPlan &ap = plan.anc[ai];
            double cn[32];
            for (int i = 0; i < 32; i++)
            {
                ap.cam_a[i] = a.cams[A.cam][size_t(i)];
                ap.cam_b[i] = b.cams[B.cam][size_t(i)];
                cn[i] = base.cams[an.cam][size_t(i)];
            }
            double ea[3], eb[3], ra[9], rb[9], qa[4], qb[4];
            if (!view_split(ap.cam_a, ea, ra) || !view_split(ap.cam_b, eb, rb))
                continue;
            rot_quat(ra, qa);
            rot_quat(rb, qb);
            const double eye = std::sqrt((ea[0] - eb[0]) * (ea[0] - eb[0]) + (ea[1] - eb[1]) * (ea[1] - eb[1]) + (ea[2] - eb[2]) * (ea[2] - eb[2]));
            if (eye > p.cam_cut_eye || quat_deg(qa, qb) > p.cam_cut_deg)
                continue;
            for (int i = 0; i < 3; i++)
            {
                ap.wa[i] = A.point[i];
                ap.wb[i] = B.point[i];
            }
            double Sa[16], Sb[16], Sn[16], z, w;
            cam_screen(ap.cam_a, Sa);
            cam_screen(ap.cam_b, Sb);
            cam_screen(cn, Sn);
            const double wn[3] = {an.point[0], an.point[1], an.point[2]};
            if (!project(Sa, ap.wa, &ap.pa_x, &ap.pa_y, &z, &w) || !project(Sb, ap.wb, &ap.pb_x, &ap.pb_y, &z, &w) || !project(Sn, wn, &ap.sx0, &ap.sy0, &z, &w))
                continue;
            /* a point that jumped (the game's own jump of a name tag: a respawn, a new target) is not followed */
            if (std::hypot(ap.pb_x - ap.pa_x, ap.pb_y - ap.pa_y) > double(p.gate_px))
                continue;
            /* ...nor an element whose sprite did not follow its point from a to b: its first vertex moved as the point's place did, or it is the step's own */
            if (p.anchor_tol > 0)
            {
                if (A.first_kick < 0 || B.first_kick < 0)
                    continue;
                const Kick &ka = a.kicks[size_t(A.first_kick)], &kb = b.kicks[size_t(B.first_kick)];
                if (ka.key != kb.key || ka.count != kb.count || !ka.count)
                    continue;
                const Vertex &va = a.verts[ka.first], &vb = b.verts[kb.first];
                const double mx = double(rel(vb.x, kb.ofx) - rel(va.x, ka.ofx)) / 16.0, my = double(rel(vb.y, kb.ofy) - rel(va.y, ka.ofy)) / 16.0;
                if (std::hypot(mx - (ap.pb_x - ap.pa_x), my - (ap.pb_y - ap.pa_y)) > p.anchor_tol)
                    continue;
            }
            ap.ok = true;
        }
    }
    if (res.verts == 0 || double(res.v_on_bad) > p.cut_ratio * double(res.v_on))
    {
        res.cut = true;
        return false;
    }
    /* What does not depend on alpha: each vertex's position in the step (in the picture's own coordinates), and the slot of its position (the
     * vertices that the step has at one position share a slot), with whether a kick that is not interpolated holds it (see apply_interp). */
    const size_t nv = base.verts.size();
    const bool unify = !(p.abl & 64);
    const int32_t am = p.anchor_margin * 16;
    plan.bx.assign(nv, 0);
    plan.by.assign(nv, 0);
    plan.slot.assign(nv, 0xFFFFFFFFu);
    plan.anch.clear();
    plan.hx.clear();
    plan.hy.clear();
    plan.ax.assign(nv, 0);
    plan.ay.assign(nv, 0);
    struct HS
    {
        int32_t x, y;
        uint32_t id;
        uint8_t used;
    };
    std::vector<HS> table;
    uint32_t tmask = 0;
    if (unify)
    {
        uint32_t n = 1;
        while (n < 2 * res.verts + 16)
            n <<= 1;
        table.assign(n, HS());
        tmask = n - 1;
    }
    for (uint32_t i = 0; i < base.kicks.size(); i++)
    {
        const Kick &k = base.kicks[i];
        if (k.path != 1 || !k.count)
            continue;
        for (uint32_t j = 0; j < k.count; j++)
        {
            const uint32_t idx = k.first + j;
            const Vertex &v = base.verts[idx];
            const int32_t bx = rel(v.x, k.ofx), by = rel(v.y, k.ofy);
            plan.bx[idx] = bx;
            plan.by[idx] = by;
            if (!unify)
                continue;
            uint32_t h = (uint32_t(bx) * 73856093u) ^ (uint32_t(by) * 19349663u);
            h ^= h >> 15;
            HS *sl;
            for (;; h++)
            {
                sl = &table[h & tmask];
                if (!sl->used)
                {
                    sl->used = 1;
                    sl->x = bx;
                    sl->y = by;
                    sl->id = uint32_t(plan.anch.size());
                    plan.anch.push_back(0);
                    plan.hx.push_back(INT32_MAX);
                    plan.hy.push_back(INT32_MAX);
                    break;
                }
                if (sl->x == bx && sl->y == by)
                    break;
            }
            plan.slot[idx] = sl->id;
            const bool in_box = bx >= -am && bx <= 640 * 16 + am && by >= -am && by <= 448 * 16 + am;
            if (plan.dec[i].what == 1 || plan.dec[i].what == 2)
            {
                const Kick &ka = a.kicks[plan.dec[i].ka];
                plan.ax[idx] = rel(a.verts[ka.first + j].x, ka.ofx);
                plan.ay[idx] = rel(a.verts[ka.first + j].y, ka.ofy);
            }
            if (plan.dec[i].what == 2)
            {
                /* a gated kick is held at step a's position until alpha 1/2 (a kick that has a vertex where a kick with no pair has one is not: below) */
                plan.dec[i].held = 1;
                if (in_box && plan.anch[sl->id] == 0)
                    plan.anch[sl->id] = 2;
            }
            else if (plan.dec[i].what != 1 && plan.dec[i].what != 3 && in_box)
                plan.anch[sl->id] = 1;
        }
    }
    if (unify)
    {
        /* A held kick that shares a vertex with a kick that has no pair is not held (it would be torn: that vertex stays at step n while the
         * others go to step a, far apart as a gated kick's two positions are); its vertices then hold their places for the kicks they share
         * them with, which may unhold those: until nothing changes. */
        for (bool changed = true; changed;)
        {
            changed = false;
            for (uint32_t i = 0; i < base.kicks.size(); i++)
            {
                const Kick &k = base.kicks[i];
                if (!plan.dec[i].held)
                    continue;
                bool clash = false;
                for (uint32_t j = 0; j < k.count && !clash; j++)
                    clash = plan.anch[plan.slot[k.first + j]] == 1;
                if (!clash)
                    continue;
                plan.dec[i].held = 0;
                changed = true;
                for (uint32_t j = 0; j < k.count; j++)
                {
                    const uint32_t idx = k.first + j;
                    if (plan.bx[idx] >= -am && plan.bx[idx] <= 640 * 16 + am && plan.by[idx] >= -am && plan.by[idx] <= 448 * 16 + am)
                        plan.anch[plan.slot[idx]] = 1;
                }
            }
        }
        /* the position a slot's held vertices hold: the least of their positions in step a */
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            if (!plan.dec[i].held)
                continue;
            const Kick &k = base.kicks[i];
            for (uint32_t j = 0; j < k.count; j++)
            {
                const uint32_t idx = k.first + j, sid = plan.slot[idx];
                if (plan.ax[idx] < plan.hx[sid] || (plan.ax[idx] == plan.hx[sid] && plan.ay[idx] < plan.hy[sid]))
                {
                    plan.hx[sid] = plan.ax[idx];
                    plan.hy[sid] = plan.ay[idx];
                }
            }
        }
    }
    else
        for (uint32_t i = 0; i < base.kicks.size(); i++)
            plan.dec[i].held = 0;   /* (experiments: no common position) */
    return true;
}

Result apply_interp(const Step &base, uint8_t *data, const Step &a, const Step &b, const Plan &plan, const Params &p, uint32_t alpha,
                    Scratch *scratch)
{
    Scratch own;
    Scratch &sc = scratch ? *scratch : own;
    Result res = plan.res;
    const std::vector<Decision> &dec = plan.dec;
    const bool unify = !(p.abl & 64);
    const int64_t wb = int64_t(alpha > 256 ? 256 : alpha), wa = 256 - wb;   /* the weights of a and b, in 1/256 */
    const double fb = double(wb) / 256.0;
    const bool early = wb < 128;   /* a gated kick is held at step a's position */
    /* Where each vertex of the picture goes, in the picture's own coordinates: the mean of the two steps' positions, or where the step
     * has it for a kick that is not interpolated. Vertices that the step has at one position, in whichever kicks, must stay at one
     * position, or the edge two kicks share opens (a thin line of the background, or a doubled one): the interpolated kicks' vertices
     * there take the same mean (the least of them) when they are within unify_px; a position that a kick that is not interpolated has
     * (gated, or with no pair) is kept for all, within anchor_margin of the picture (further out there is no edge to open, and the
     * clipped vertices that pile up at one position would hold unrelated kicks). */
    const size_t nv = base.verts.size();
    std::vector<int32_t> &fx = sc.fx, &fy = sc.fy;
    std::vector<uint8_t> &fix = sc.fix;   /* 1: the vertex keeps the step's position */
    std::vector<int32_t> &cx = sc.cx, &cy = sc.cy;   /* the least mean of the interpolated vertices at a slot */
    fx.resize(nv);
    fy.resize(nv);
    fix.assign(nv, 0);
    const double gate2w = 16.0 * 16.0 * 16.0 * 16.0;   /* 16 px */
    double Bc[32], Bs[16];   /* the camera at alpha, and its world-to-screen matrix */
    if (plan.cam_ok)
    {
        cam_blend(plan.cam_a, plan.cam_b, fb, Bc);
        cam_screen(Bc, Bs);
    }
    if (unify)
    {
        cx.assign(plan.anch.size(), INT32_MAX);
        cy.assign(plan.anch.size(), INT32_MAX);
    }
    auto wpoint = [&](uint32_t, uint32_t idx, double pt[3]) {
        const double x = plan.wpt[idx * 3], y = plan.wpt[idx * 3 + 1], z = plan.wpt[idx * 3 + 2];
        pt[0] = x;
        pt[1] = y;
        pt[2] = z;
    };
    std::vector<uint8_t> badcam;   /* per kick: the camera would move a vertex too far (cam_max_px): the kick is not moved */
    if (plan.cam_ok && p.cam_max_px > 0)
    {
        badcam.assign(base.kicks.size(), 0);
        const double lim = p.cam_max_px * 16.0;
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            if (dec[i].what != 3)
                continue;
            const Kick &k = base.kicks[i];
            for (uint32_t j = 0; j < k.count && !badcam[i]; j++)
            {
                const uint32_t idx = k.first + j;
                if (!plan.wok[idx])
                    continue;
                double sx, sy, sz, w;
                double pt[3];
                wpoint(i, idx, pt);
                if (!project(Bs, pt, &sx, &sy, &sz, &w))
                {
                    badcam[i] = 1;
                    break;
                }
                const double dx = sx * 16.0 - k.ofx - plan.bx[idx], dy = sy * 16.0 - k.ofy - plan.by[idx];
                if (dx * dx + dy * dy > lim * lim)
                    badcam[i] = 1;
            }
        }
    }
    for (uint32_t i = 0; i < base.kicks.size(); i++)
    {
        const Kick &k = base.kicks[i];
        if (k.path != 1 || !k.count)
            continue;
        const Decision &d = dec[i];
        for (uint32_t j = 0; j < k.count; j++)
        {
            const uint32_t idx = k.first + j;
            const int32_t bx = plan.bx[idx], by = plan.by[idx];
            if (d.what == 3 && !badcam.empty() && badcam[i])
            {
                /* (the camera would send a vertex of this kick far: it keeps what the step has; a kick that had a pair is drawn as step b has it in the skip test with abl 128) */
                fx[idx] = bx;
                fy[idx] = by;
                fix[idx] = 1;
                if ((p.abl & 128) && d.kb >= 0 && &b != &base && b.kicks[d.kb].count == k.count)
                {
                    const Kick &kb = b.kicks[d.kb];
                    fx[idx] = rel(b.verts[kb.first + j].x, kb.ofx);
                    fy[idx] = rel(b.verts[kb.first + j].y, kb.ofy);
                    fix[idx] = 0;
                }
            }
            else if (d.held && early)
            {
                /* a held kick below alpha 1/2 is as step a has it (xy, z, colour, ST: the loop below writes them with the weights 1 and 0); the vertices
                 * that share a position in the step share the least of their positions in step a, when that is within unify_px of their own */
                const uint32_t sid = plan.slot[idx];
                fx[idx] = plan.ax[idx];
                fy[idx] = plan.ay[idx];
                if (plan.anch[sid] == 2 && std::abs(plan.ax[idx] - plan.hx[sid]) <= p.unify_px * 16 && std::abs(plan.ay[idx] - plan.hy[sid]) <= p.unify_px * 16)
                {
                    fx[idx] = plan.hx[sid];
                    fy[idx] = plan.hy[sid];
                }
            }
            else if (d.what == 3)
            {
                double sx, sy, sz, w;
                double pt[3];
                wpoint(i, idx, pt);
                if (plan.wok[idx] && project(Bs, pt, &sx, &sy, &sz, &w))
                {
                    fx[idx] = int32_t(std::floor(sx * 16.0 + 0.5)) - k.ofx;
                    fy[idx] = int32_t(std::floor(sy * 16.0 + 0.5)) - k.ofy;
                    if (unify)
                    {
                        const uint32_t sid = plan.slot[idx];
                        if (fx[idx] < cx[sid] || (fx[idx] == cx[sid] && fy[idx] < cy[sid]))
                        {
                            cx[sid] = fx[idx];
                            cy[sid] = fy[idx];
                        }
                    }
                }
                else
                {
                    fx[idx] = bx;
                    fy[idx] = by;
                    fix[idx] = 1;
                }
            }
            else if (d.what != 1)
            {
                fx[idx] = bx;
                fy[idx] = by;
                fix[idx] = 1;
                if ((p.abl & 128) && d.kb >= 0 && &b != &base && b.kicks[d.kb].count == k.count)
                {
                    /* (tests) the skip test as a live in-between has it: a kick the pairing cannot follow is drawn as step b has it, not as the step being compared */
                    const Kick &kb = b.kicks[d.kb];
                    fx[idx] = rel(b.verts[kb.first + j].x, kb.ofx);
                    fy[idx] = rel(b.verts[kb.first + j].y, kb.ofy);
                    fix[idx] = 0;
                }
            }
            else
            {
                const Kick &ka = a.kicks[d.ka];
                const Kick &kb = b.kicks[d.kb];
                fx[idx] = int32_t((wa * rel(a.verts[ka.first + j].x, ka.ofx) + wb * rel(b.verts[kb.first + j].x, kb.ofx) + 128 * p.xy_bias) >> 8);
                fy[idx] = int32_t((wa * rel(a.verts[ka.first + j].y, ka.ofy) + wb * rel(b.verts[kb.first + j].y, kb.ofy) + 128 * p.xy_bias) >> 8);
                if (plan.cam_ok && !plan.wbok.empty() && plan.wbok[idx])
                {
                    double sx, sy, sz, w;
                    const double pt[3] = {double(plan.wa3[idx * 3]) * (1.0 - fb) + double(plan.wb3[idx * 3]) * fb, double(plan.wa3[idx * 3 + 1]) * (1.0 - fb) + double(plan.wb3[idx * 3 + 1]) * fb,
                                          double(plan.wa3[idx * 3 + 2]) * (1.0 - fb) + double(plan.wb3[idx * 3 + 2]) * fb};
                    if (project(Bs, pt, &sx, &sy, &sz, &w))
                    {
                        const int32_t wx = int32_t(std::floor(sx * 16.0 + 0.5)) - k.ofx, wy = int32_t(std::floor(sy * 16.0 + 0.5)) - k.ofy;
                        /* only where the two agree about the picture (a vertex of a clipped strip goes anywhere): within the gate of the straight mean */
                        const double ddx = double(wx - fx[idx]), ddy = double(wy - fy[idx]);
                        if (ddx * ddx + ddy * ddy <= gate2w)
                        {
                            fx[idx] = wx;
                            fy[idx] = wy;
                        }
                    }
                }
                if (p.persp && wb == 128)
                {
                    const Vertex &pa = a.verts[ka.first + j], &pb = b.verts[kb.first + j];
                    if (pa.q > 1e-6f && pb.q > 1e-6f && pa.q < 1e6f && pb.q < 1e6f && !(pa.q == 1.0f && pb.q == 1.0f))
                    {
                        const double wa_ = 1.0 / pa.q, wb_ = 1.0 / pb.q, pcx = 320 * 16, pcy = 224 * 16;
                        const double W = 0.5 * (wa_ + wb_);
                        double X = 0.5 * ((rel(pa.x, ka.ofx) - pcx) * wa_ + (rel(pb.x, kb.ofx) - pcx) * wb_) / W + pcx;
                        double Y = 0.5 * ((rel(pa.y, ka.ofy) - pcy) * wa_ + (rel(pb.y, kb.ofy) - pcy) * wb_) / W + pcy;
                        fx[idx] = int32_t(std::floor(X + 0.5));
                        fy[idx] = int32_t(std::floor(Y + 0.5));
                    }
                }
                if (unify)
                {
                    const uint32_t sid = plan.slot[idx];
                    if (fx[idx] < cx[sid] || (fx[idx] == cx[sid] && fy[idx] < cy[sid]))
                    {
                        cx[sid] = fx[idx];
                        cy[sid] = fy[idx];
                    }
                }
            }
        }
    }
    if (unify)
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            const Kick &k = base.kicks[i];
            if (k.path != 1 || !k.count || (dec[i].what != 1 && dec[i].what != 3))
                continue;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const uint32_t idx = k.first + j;
                if (dec[i].what == 3 && fix[idx])
                    continue;
                const uint32_t sid = plan.slot[idx];
                if (plan.anch[sid] == 2 && early && std::abs(plan.ax[idx] - plan.hx[sid]) <= p.gate_px * 16 && std::abs(plan.ay[idx] - plan.hy[sid]) <= p.gate_px * 16)
                {
                    /* goes with the held vertices it shares the position with (when step a had it near theirs: else the kick would be stretched) */
                    fx[idx] = plan.hx[sid];
                    fy[idx] = plan.hy[sid];
                }
                else if (plan.anch[sid])
                {
                    fx[idx] = plan.bx[idx];
                    fy[idx] = plan.by[idx];
                    fix[idx] = 1;
                }
                else if (std::abs(fx[idx] - cx[sid]) <= p.unify_px * 16 && std::abs(fy[idx] - cy[sid]) <= p.unify_px * 16)
                {
                    fx[idx] = cx[sid];
                    fy[idx] = cy[sid];
                }
            }
        }
    for (uint32_t i = 0; i < base.kicks.size(); i++)
    {
        const Decision &d = dec[i];
        if (d.what == 3 && !badcam.empty() && badcam[i])
        {
            if (p.abl & 128)
            {
                const Kick &k = base.kicks[i];
                for (uint32_t j = 0; j < k.count; j++)
                {
                    const Vertex &v = base.verts[k.first + j];
                    if (!packed(v) || fix[k.first + j])
                        continue;
                    uint8_t *q = data + v.xyz_off;
                    st16(q, uint32_t(fx[k.first + j] + k.ofx));
                    st16(q + 4, uint32_t(fy[k.first + j] + k.ofy));
                }
            }
            continue;
        }
        if (d.what == 3)
        {
            /* moved by the camera: x and y from the projection; the texture's S, T and Q are the step's, scaled by the ratio of the w the step's
             * camera gave the vertex to the w of the camera at alpha (Q is 1/w times a constant; S and T go with Q); z, colour as the step has them */
            const Kick &k = base.kicks[i];
            const int32_t bx0 = fx[k.first] + k.ofx, by0 = fy[k.first] + k.ofy;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const uint32_t idx = k.first + j;
                if (d.born && p.birth == 1 && base.verts[idx].rgb_off != NO_OFF)
                {
                    uint8_t *c = data + base.verts[idx].rgb_off;
                    c[12] = uint8_t((uint32_t(base.verts[idx].a) * alpha + 128) >> 8);
                }
                if (d.born && p.birth == 2 && packed(base.verts[idx]) && plan.wok[k.first])
                {
                    uint8_t *q = data + base.verts[idx].xyz_off;
                    st16(q, uint32_t(bx0));
                    st16(q + 4, uint32_t(by0));
                    continue;
                }
                if (p.c3log && p.c3hi - p.c3lo <= 8 && i >= p.c3lo && i < p.c3hi)
                    fprintf(p.c3log, "  k %u v %u: x %.1f y %.1f z %u q %g adc %u -> x %.1f y %.1f ok %d fix %d (alpha %u)\n", i, j, rel(base.verts[idx].x, k.ofx) / 16.0, rel(base.verts[idx].y, k.ofy) / 16.0,
                            base.verts[idx].z, double(base.verts[idx].q), base.verts[idx].adc, fx[idx] / 16.0, fy[idx] / 16.0, int(plan.wok[idx]), int(fix[idx]), alpha);
                if (fix[idx] || !plan.wok[idx])
                    continue;
                const Vertex &v = base.verts[idx];
                uint8_t *q = data + v.xyz_off;
                if (!(p.abl & 1))
                {
                    st16(q, uint32_t(fx[idx] + k.ofx));
                    if (!(p.abl & 32))
                        st16(q + 4, uint32_t(fy[idx] + k.ofy));
                }
                if (p.rp_z)
                {
                    double sx, sy, sz, w;
                    double pt[3];
                wpoint(i, idx, pt);
                    if (project(Bs, pt, &sx, &sy, &sz, &w) && std::isfinite(sz))
                    {
                        const double zr = std::floor(sz + 0.5);
                        const uint32_t z = zr < 0 ? 0 : zr > 16777215.0 ? 0xFFFFFFu : uint32_t(zr);
                        if (v.fmt == 4)
                        {
                            uint32_t w2 = ld32(q + 8);
                            st32(q + 8, (w2 & 0xF000000F) | (z << 4));
                        }
                    }
                }
                if (v.st_off != NO_OFF && !(p.abl & 8))
                {
                    double sx, sy, sz, w;
                    double pt[3];
                wpoint(i, idx, pt);
                    if (project(Bs, pt, &sx, &sy, &sz, &w) && v.q > 0.0f)
                    {
                        const double r = double(plan.whw[idx]) / w;
                        uint8_t *c = data + v.st_off;
                        const double s2 = double(v.s) * r, t2 = double(v.t) * r, q2 = double(v.q) * r;
                        if (std::isfinite(s2) && std::isfinite(t2) && std::isfinite(q2))
                        {
                            stf(c, float(s2));
                            stf(c + 4, float(t2));
                            stf(c + 8, float(q2));
                        }
                    }
                }
            }
            continue;
        }
        if ((p.abl & 128) && (d.what == 0 || d.what == 2) && d.kb >= 0 && &b != &base && b.kicks[d.kb].count == base.kicks[i].count)
        {
            const Kick &k = base.kicks[i];
            for (uint32_t j = 0; j < k.count; j++)
            {
                const Vertex &v = base.verts[k.first + j];
                if (!packed(v) || fix[k.first + j])
                    continue;
                uint8_t *q = data + v.xyz_off;
                st16(q, uint32_t(fx[k.first + j] + k.ofx));
                st16(q + 4, uint32_t(fy[k.first + j] + k.ofy));
            }
            continue;
        }
        if (d.att && p.attach_fade && d.what != 1)
        {
            const Kick &k = base.kicks[i];
            for (uint32_t j = 0; j < k.count; j++)
            {
                const Vertex &v = base.verts[k.first + j];
                if (v.rgb_off != NO_OFF)
                    data[v.rgb_off + 12] = uint8_t((uint32_t(v.a) * alpha + 128) >> 8);
            }
        }
        if (d.what != 1 && !(d.held && early))
            continue;
        const Kick &k = base.kicks[i];
        const Kick &ka = a.kicks[d.ka];
        const Kick &kb = b.kicks[d.kb];
        /* the weights of a and b for this kick: a gated kick is step a's */
        const int64_t kwb = d.what == 1 ? wb : 0, kwa = 256 - kwb;   /* (a held kick: 0) */
        const double kfb = double(kwb) / 256.0, kfa = 1.0 - kfb;
        for (uint32_t j = 0; j < k.count; j++)
        {
            const Vertex &v = base.verts[k.first + j];
            const Vertex &va = a.verts[ka.first + j];
            const Vertex &vb = b.verts[kb.first + j];
            uint8_t *q = data + v.xyz_off;
            int32_t x = fx[k.first + j] + k.ofx;
            int32_t y = fy[k.first + j] + k.ofy;
            if (p.stats)
            {
                int32_t px = rel(v.x, k.ofx), py = rel(v.y, k.ofy);
                if (px >= -64 * 16 && px <= 704 * 16 && py >= -64 * 16 && py <= 512 * 16)
                {
                    double ex = double(x - int32_t(v.x & 0xFFFF)), ey = double(y - int32_t(v.y & 0xFFFF));
                    double e = std::sqrt(ex * ex + ey * ey);
                    res.err[e <= 1 ? 0 : e <= 4 ? 1 : e <= 8 ? 2 : e <= 16 ? 3 : e <= 32 ? 4 : 5]++;
                    double rx = double(rel(va.x, ka.ofx) - px), ry = double(rel(va.y, ka.ofy) - py);
                    if (rx * rx + ry * ry > 64)
                        res.merr[e <= 1 ? 0 : e <= 4 ? 1 : e <= 8 ? 2 : e <= 16 ? 3 : e <= 32 ? 4 : 5]++;
                }
            }
            if (!(p.abl & 1) && !fix[k.first + j])
            {
                st16(q, uint32_t(x));
                if (!(p.abl & 32))
                    st16(q + 4, uint32_t(y));
            }
            if (p.abl & 2)
                ;
            else if (v.fmt == 4)
            {
                uint32_t z = uint32_t((uint64_t(va.z) * uint64_t(kwa) + uint64_t(vb.z) * uint64_t(kwb)) >> 8) & 0xFFFFFF;
                uint32_t w2 = ld32(q + 8);
                st32(q + 8, (w2 & 0xF000000F) | (z << 4));
            }
            else
                st32(q + 8, uint32_t((uint64_t(va.z) * uint64_t(kwa) + uint64_t(vb.z) * uint64_t(kwb)) >> 8));
            if (v.rgb_off != NO_OFF && !(p.abl & 4))
            {
                uint8_t *c = data + v.rgb_off;
                c[0] = uint8_t((va.r * kwa + vb.r * kwb + 128 * p.rgb_bias) >> 8);
                c[4] = uint8_t((va.g * kwa + vb.g * kwb + 128 * p.rgb_bias) >> 8);
                c[8] = uint8_t((va.b * kwa + vb.b * kwb + 128 * p.rgb_bias) >> 8);
            }
            if (v.st_off != NO_OFF && !(p.abl & 8))
            {
                uint8_t *c = data + v.st_off;
                if (va.q != 0.0f && vb.q != 0.0f && !(p.abl & 16))
                {
                    double qm = double(va.q) * kfa + double(vb.q) * kfb;
                    double s = ((double(va.s) / va.q) * kfa + (double(vb.s) / vb.q) * kfb) * qm;
                    double t = ((double(va.t) / va.q) * kfa + (double(vb.t) / vb.q) * kfb) * qm;
                    if (std::isfinite(s) && std::isfinite(t) && std::isfinite(qm))
                    {
                        stf(c, float(s));
                        stf(c + 4, float(t));
                        stf(c + 8, float(qm));
                    }
                }
                else
                {
                    stf(c, float(double(va.s) * kfa + double(vb.s) * kfb));
                    stf(c + 4, float(double(va.t) * kfa + double(vb.t) * kfb));
                }
            }
        }
    }
    if (!plan.anc.empty())
    {
        /* The 2D kicks of an anchor move by the point's place on the screen: its world point (a's to b's, by alpha) seen by the camera at alpha (the cameras of a and b
         * blended as the models' are), minus where the step has the point (the step's own point and camera). x and y in 1/16 px, written into the 2D vertices'
         * place in the data. Nothing else of the 2D is touched. */
        std::vector<int32_t> adx(plan.anc.size(), 0), ady(plan.anc.size(), 0);
        for (size_t ai = 0; ai < plan.anc.size(); ai++)
        {
            const Plan::AnchorPlan &ap = plan.anc[ai];
            if (!ap.ok)
                continue;
            double c[32], S[16], pt[3], sx, sy, sz, w;
            cam_blend(ap.cam_a, ap.cam_b, fb, c);
            cam_screen(c, S);
            for (int i = 0; i < 3; i++)
                pt[i] = ap.wa[i] * (1.0 - fb) + ap.wb[i] * fb;
            if (!project(S, pt, &sx, &sy, &sz, &w))
                continue;
            /* the game places its 2D elements on whole pixels (the text, the icons: an integer position from the point's screen place), and the picture of a sprite off the
             * pixel grid is blurred by the filter: the move is whole pixels too (Params::anchor_round) */
            double mx = sx - ap.sx0, my = sy - ap.sy0;
            if (std::hypot(mx, my) < p.anchor_min)
                mx = my = 0;
            adx[ai] = p.anchor_round ? int32_t(std::floor(mx + 0.5)) * 16 : int32_t(std::floor(mx * 16.0 + 0.5));
            ady[ai] = p.anchor_round ? int32_t(std::floor(my + 0.5)) * 16 : int32_t(std::floor(my * 16.0 + 0.5));
            res.anch++;
        }
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            const Kick &k = base.kicks[i];
            if (k.path == 1 || !k.count || k.anchor < 0 || !plan.anc[size_t(k.anchor)].ok)
                continue;
            const int32_t dx = adx[size_t(k.anchor)], dy = ady[size_t(k.anchor)];
            if (!dx && !dy)
                continue;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const Vertex &v = base.verts[k.first + j];
                if (v.xyz_off == NO_OFF || v.fmt < 4 || v.fmt > 6)
                    continue;
                uint8_t *q = data + v.xyz_off;
                st16(q, uint32_t(int32_t(v.x & 0xFFFF) + dx));
                st16(q + (v.fmt == 6 ? 2 : 4), uint32_t(int32_t(v.y & 0xFFFF) + dy));
            }
            res.anch_kicks++;
        }
    }
    if (p.twod_px > 0)
    {
        /* The 2D (paths 2 and 3: sprites, text, the gauges): a kick that both steps have, paired by key and occurrence among the 2D kicks, with
         * the same texture coordinates in every vertex (the same picture, moved), and whose vertices move no more than twod_px between the steps, is
         * placed between its positions in a and b, as the models' kicks are. A kick that is not like that is the step's own, as before. */
        auto by_key = [](const Step &s, std::unordered_map<uint64_t, std::vector<uint32_t>> &m) {
            for (uint32_t i = 0; i < s.kicks.size(); i++)
                if (s.kicks[i].path != 1 && s.kicks[i].count)
                    m[s.kicks[i].key].push_back(i);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> ma, mb;
        by_key(a, ma);
        if (&b != &base)
            by_key(b, mb);
        std::unordered_map<uint64_t, uint32_t> occ2;
        const int64_t lim2 = int64_t(p.twod_px * 16.0) * int64_t(p.twod_px * 16.0);
        const int64_t min2 = int64_t(p.twod_min * 16.0) * int64_t(p.twod_min * 16.0);
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            const Kick &k = base.kicks[i];
            if (k.path == 1 || !k.count)
                continue;
            const uint32_t o = occ2[k.key]++;
            auto fa = ma.find(k.key);
            if (fa == ma.end() || o >= fa->second.size())
                continue;
            int32_t ib = int32_t(i);
            if (&b != &base)
            {
                auto fb_ = mb.find(k.key);
                if (fb_ == mb.end() || o >= fb_->second.size())
                    continue;
                ib = int32_t(fb_->second[o]);
            }
            const Kick &ka = a.kicks[fa->second[o]];
            const Kick &kb = b.kicks[ib];
            if (ka.count != k.count || kb.count != k.count)
                continue;
            bool ok = true;
            int64_t mx2 = 0;
            for (uint32_t j = 0; ok && j < k.count; j++)
            {
                const Vertex &v = base.verts[k.first + j], &va = a.verts[ka.first + j], &vb = b.verts[kb.first + j];
                if (v.xyz_off == NO_OFF || v.fmt < 4 || v.fmt > 6 || v.fmt != va.fmt || v.fmt != vb.fmt || v.adc != va.adc || v.adc != vb.adc || ((k.prim & 0x100) ? va.uv != vb.uv : (va.s != vb.s || va.t != vb.t || va.q != vb.q)))
                {
                    ok = false;
                    break;
                }
                const int64_t dx = int64_t(rel(vb.x, kb.ofx)) - rel(va.x, ka.ofx), dy = int64_t(rel(vb.y, kb.ofy)) - rel(va.y, ka.ofy);
                if (dx * dx + dy * dy > lim2)
                    ok = false;
                mx2 = std::max(mx2, dx * dx + dy * dy);
            }
            if (!ok || mx2 < min2 || !mx2)
                continue;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const Vertex &v = base.verts[k.first + j], &va = a.verts[ka.first + j], &vb = b.verts[kb.first + j];
                uint8_t *q = data + v.xyz_off;
                const int32_t x = int32_t((wa * rel(va.x, ka.ofx) + wb * rel(vb.x, kb.ofx) + 128 * p.xy_bias) >> 8);
                const int32_t y = int32_t((wa * rel(va.y, ka.ofy) + wb * rel(vb.y, kb.ofy) + 128 * p.xy_bias) >> 8);
                st16(q, uint32_t(x + k.ofx));
                st16(q + (v.fmt == 6 ? 2 : 4), uint32_t(y + k.ofy));
            }
            res.twod++;
        }
    }
    if (p.stats)
    {
        /* (tests) the error of the final positions against the real step, per model draw: a vertex that keeps the step has no error */
        std::unordered_map<int32_t, size_t> at;
        for (uint32_t i = 0; i < base.kicks.size(); i++)
        {
            const Kick &k = base.kicks[i];
            if (k.path != 1 || !k.count)
                continue;
            auto it = at.find(k.model);
            if (it == at.end())
            {
                it = at.emplace(k.model, res.model_err.size()).first;
                ModelErr e;
                e.model = k.model;
                if (k.model >= 0)
                {
                    e.handle = base.models[k.model].handle;
                    e.occ = base.models[k.model].occ;
                    e.prog = base.models[k.model].prog;
                    e.flags = base.models[k.model].flags;
                    for (int q = 0; q < 3; q++)
                        e.pos[q] = base.models[k.model].world[12 + q];
                }
                if (plan.cam_ok)
                {
                    double ee[3], rr[9];
                    if (view_split(plan.cam_n, ee, rr))
                        for (int q = 0; q < 3; q++)
                            e.eye[q] = float(ee[q]);
                }
                res.model_err.push_back(e);
            }
            ModelErr &e = res.model_err[it->second];
            const int w = dec[i].what > 3 ? 0 : dec[i].what;
            e.kicks[w]++;
            for (uint32_t j = 0; j < k.count; j++)
            {
                const uint32_t idx = k.first + j;
                const int32_t bx = plan.bx[idx], by = plan.by[idx];
                if (bx < -64 * 16 || bx > 704 * 16 || by < -64 * 16 || by > 512 * 16)
                    continue;
                double er = 0;
                if (!fix[idx] && fx[idx] != INT32_MIN)
                    er = std::sqrt(double(fx[idx] - bx) * double(fx[idx] - bx) + double(fy[idx] - by) * double(fy[idx] - by)) / 16.0;
                e.verts++;
                e.sum += er;
                e.sum_what[w] += er;
                e.v_what[w]++;
                if (er > e.max)
                    e.max = er;
                e.over2 += er > 2;
                e.over8 += er > 8;
            }
        }
    }
    return res;
}

Result interpolate(Step &base, const Step &a, const Index &ia, const Step &b, const Index &ib, const Params &p)
{
    Plan plan;
    if (!plan_interp(base, a, ia, b, ib, p, plan))
        return plan.res;
    /* p.alpha in 1/256 (0.5 is 128: the mean) */
    const double al = p.alpha < 0 ? 0 : p.alpha > 1 ? 1 : p.alpha;
    return apply_interp(base, base.data.data(), a, b, plan, p, uint32_t(al * 256.0 + 0.5));
}
}

namespace Mh2Interp
{
/* (diagnostics, the judder measure) the world point of a screen vertex (x, y in window units, w) as the camera of the picture t of the way from step a to
 * step b sees it; false when the plan has no good camera or the system is degenerate */
bool diag_world(const Plan &plan, double t, double sx, double sy, double w, double out[3])
{
    if (!plan.cam_ok)
        return false;
    double Bc[32], Bs[16];
    if (t <= 0)
        cam_screen(plan.cam_a, Bs);
    else if (t >= 1)
        cam_screen(plan.cam_b, Bs);
    else
    {
        cam_blend(plan.cam_a, plan.cam_b, t, Bc);
        cam_screen(Bc, Bs);
    }
    return unproject(Bs, sx, sy, 0.0, w, false, out);
}

/* the same with the cameras of a draw (ca of step a, cb of step b: 32 doubles each, view then projection) */
bool diag_world_c(const double *ca, const double *cb, double t, double sx, double sy, double w, double out[3])
{
    double Bc[32], Bs[16];
    if (t <= 0)
        cam_screen(ca, Bs);
    else if (t >= 1)
        cam_screen(cb, Bs);
    else
    {
        cam_blend(ca, cb, t, Bc);
        cam_screen(Bc, Bs);
    }
    return unproject(Bs, sx, sy, 0.0, w, false, out);
}

/* the screen pixels a world unit moves a point at W, by step b's camera (the root mean square over the three axes) */
double diag_scale_c(const double *cb, const double W[3])
{
    double Bs[16], sx, sy, sz, w, tx, ty;
    cam_screen(cb, Bs);
    if (!project(Bs, W, &sx, &sy, &sz, &w))
        return 0.0;
    double acc = 0;
    for (int k = 0; k < 3; k++)
    {
        double q[3] = {W[0], W[1], W[2]};
        q[k] += 1.0;
        if (!project(Bs, q, &tx, &ty, &sz, &w))
            return 0.0;
        acc += (tx - sx) * (tx - sx) + (ty - sy) * (ty - sy);
    }
    return std::sqrt(acc / 3.0);
}
}

namespace Mh2Interp
{
double diag_scale(const Plan &plan, const double W[3])
{
    return diag_scale_c(plan.cam_b, W);
}
}
