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
                   REG_TEX0_1 = 0x06;
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

void Capture::close_kick()
{
    if (!open_)
        return;
    open_ = false;
    Kick &k = cur_.kicks.back();
    k.count = uint32_t(cur_.verts.size()) - k.first;
    k.key = fnv(key_head_, k.count);
    if (k.path == 1 && k.count)
        cur_.p1_kicks++;
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
        h = fnv(h, (prim_ & 0x40) ? reg_[REG_ALPHA_1 + c] : 0);
        h = fnv(h, reg_[REG_TEST_1 + c]);
        key_head_ = h;
        k.key = 0;
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
    case REG_FOG:
        break;
    case REG_XYZF2:
    case REG_XYZF3:
        vertex(uint32_t(v & 0xFFFF), uint32_t((v >> 16) & 0xFFFF), uint32_t((v >> 32) & 0xFFFFFF), 0, NO_OFF, 0);
        break;
    case REG_XYZ2:
    case REG_XYZ3:
        vertex(uint32_t(v & 0xFFFF), uint32_t((v >> 16) & 0xFFFF), uint32_t(v >> 32), 0, NO_OFF, 0);
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
            wreg(q[8], ld64(q));
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
                wreg(r, v);
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
    out.p1_kicks = cur_.p1_kicks;
    out.flip = cur_.flip;
    memcpy(out.cam, cur_.cam, sizeof(out.cam));
    memcpy(out.cam_last, cur_.cam_last, sizeof(out.cam_last));
    out.ncam = cur_.ncam;
    out.cam_at = cur_.cam_at;
    cur_.clear();
    /* a vertex's offsets into the data point into the step that is gone: the next vertex written by an old
     * RGBAQ or ST register must not write there */
    rgb_off_ = st_off_ = NO_OFF;
}

void Index::build(const Step &s)
{
    by_key.clear();
    for (uint32_t i = 0; i < s.kicks.size(); i++)
        if (s.kicks[i].path == 1 && s.kicks[i].count)
            by_key[s.kicks[i].key].push_back(i);
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
    std::unordered_map<uint64_t, uint32_t> occ;
    const bool live = (&b == &base);
    const double gate2 = double(p.gate_px) * 16.0 * double(p.gate_px) * 16.0;
    /* The camera: a kick with no pair, or gated, goes with it (what 3) when both steps have one and it did not jump (a camera cut). */
    plan.cam_ok = false;
    plan.wpt.clear();
    plan.whw.clear();
    plan.wok.clear();
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
            }
        }
    }
    /* does the camera account for the kick's motion between steps a and b (still world geometry)? Its vertices of step b, unprojected with b's
     * camera and projected with a's, land within p.rp_tol px of step a's, for nine in ten of a sample of them */
    double Aa_still[16];
    if (plan.cam_ok)
        cam_screen(plan.cam_a, Aa_still);
    auto still = [&](uint32_t i, const Kick &ka, const Kick &kb) -> bool {
        const Kick &k = base.kicks[i];
        const uint32_t step = k.count > 12 ? k.count / 12 : 1;
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
            good += std::sqrt(ex * ex + ey * ey) <= p.rp_tol;
        }
        return n >= 2 && good * 10 >= n * 9;
    };
    /* what 3: the kick's world points, from the step's camera; false (the kick stays as it was decided) when the camera is not good or a vertex is not a packed one */
    auto try_cam = [&](uint32_t i) -> bool {
        if (!plan.cam_ok)
            return false;
        const Kick &k = base.kicks[i];
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
        dec[i].what = 3;
        res.reproj++;
        res.v_reproj += k.count;
        return true;
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
        auto fa = ia.by_key.find(k.key);
        if (fa == ia.by_key.end() || o >= fa->second.size())
        {
            if (try_cam(i))
                continue;
            res.unmatched++;
            res.un_key++;
            res.v_un_key += k.count;
            res.v_unmatched += k.count;
            res.v_on_bad += on;
            continue;
        }
        d.ka = int32_t(fa->second[o]);
        if (live)
            d.kb = int32_t(i);
        else
        {
            auto fb = ib.by_key.find(k.key);
            if (fb == ib.by_key.end() || o >= fb->second.size())
            {
                if (try_cam(i))
                    continue;
                res.unmatched++;
                res.v_unmatched += k.count;
                res.v_on_bad += on;
                continue;
            }
            d.kb = int32_t(fb->second[o]);
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
        else if (far && try_cam(i))
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
        else if (p.rp_paired && plan.cam_ok && !live_cam_skip && still(i, ka, kb) && try_cam(i))
            ;
        else
        {
            d.what = 1;
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
            if (d.held && early)
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
                const double pt[3] = {plan.wpt[idx * 3], plan.wpt[idx * 3 + 1], plan.wpt[idx * 3 + 2]};
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
        if (d.what == 3)
        {
            /* moved by the camera: x and y from the projection; the texture's S, T and Q are the step's, scaled by the ratio of the w the step's
             * camera gave the vertex to the w of the camera at alpha (Q is 1/w times a constant; S and T go with Q); z, colour as the step has them */
            const Kick &k = base.kicks[i];
            for (uint32_t j = 0; j < k.count; j++)
            {
                const uint32_t idx = k.first + j;
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
                    const double pt[3] = {plan.wpt[idx * 3], plan.wpt[idx * 3 + 1], plan.wpt[idx * 3 + 2]};
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
                    const double pt[3] = {plan.wpt[idx * 3], plan.wpt[idx * 3 + 1], plan.wpt[idx * 3 + 2]};
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
