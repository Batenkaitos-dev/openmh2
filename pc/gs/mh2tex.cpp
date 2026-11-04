/* mh2tex: texture dumping and replacement (see mh2tex.hpp). Part of the port (CC0 1.0); built with
 * paraLLEl-GS (LGPL-3.0-or-later) into the mh2gs library. The PNG reader and writer are stb_image and
 * stb_image_write (public domain / MIT, third_party/stb). */
#include "mh2tex.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

#include "gs_util.hpp"

using namespace ParallelGS;

namespace Mh2Tex
{
namespace
{
/* ---------------------------------------------------------------- files (UTF-8 paths) */

#ifdef _WIN32
std::wstring wide(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

bool file_exists(const std::string &p)
{
#ifdef _WIN32
    return GetFileAttributesW(wide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

bool make_dir(const std::string &p)
{
#ifdef _WIN32
    return CreateDirectoryW(wide(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(p.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

/* Makes every folder of the path that is missing. The parts that cannot be made (a drive, a network share's name)
 * are not an error if the whole path is there in the end. */
bool make_dirs(const std::string &p)
{
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/' || p[i] == '\\')
        {
            std::string part = p.substr(0, i);
            if (part.size() > 1 && part.back() != ':' && !file_exists(part))
                make_dir(part);
        }
    return file_exists(p);
}

bool rename_file(const std::string &from, const std::string &to)
{
#ifdef _WIN32
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), 0) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

void remove_file(const std::string &p)
{
#ifdef _WIN32
    DeleteFileW(wide(p).c_str());
#else
    unlink(p.c_str());
#endif
}

/* All files below dir, as full paths. */
void list_files(const std::string &dir, std::vector<std::string> &out)
{
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        std::string name = narrow(fd.cFileName);
        if (name == "." || name == "..")
            continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            list_files(full, out);
        else
            out.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d))
    {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            list_files(full, out);
        else
            out.push_back(full);
    }
    closedir(d);
#endif
}

/* ---------------------------------------------------------------- hashing */

inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t finish64(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

uint64_t hash_words(const uint32_t *p, size_t n, uint64_t seed)
{
    uint64_t h = seed ^ (uint64_t(n) * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 1 < n; i += 2)
    {
        uint64_t w = uint64_t(p[i]) | (uint64_t(p[i + 1]) << 32);
        h ^= w * 0xC2B2AE3D27D4EB4Full;
        h = rotl64(h, 31) * 0x9E3779B185EBCA87ull;
    }
    if (i < n)
    {
        h ^= uint64_t(p[i]) * 0xC2B2AE3D27D4EB4Full;
        h = rotl64(h, 31) * 0x9E3779B185EBCA87ull;
    }
    return finish64(h);
}

inline uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return finish64(h);
}

/* ---------------------------------------------------------------- formats */

const char *format_tag(uint32_t psm)
{
    switch (psm)
    {
    case PSMCT32: return "C32";
    case PSMCT24: return "C24";
    case PSMCT16:
    case PSMCT16S: return "C16";
    case PSMT8: return "T8";
    case PSMT4: return "T4";
    case PSMT8H: return "T8H";
    case PSMT4HL: return "T4HL";
    case PSMT4HH: return "T4HH";
    default: return nullptr;
    }
}

template <uint32_t PSM>
void read_texels(const uint8_t *vram, uint32_t base, uint32_t tbw, uint32_t w, uint32_t h, uint32_t mask, uint32_t *out)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t a = swizzle_PS2(x, y, base, tbw, PSM, mask);
            uint32_t v;
            switch (PSM)
            {
            case PSMCT32: v = reinterpret_cast<const uint32_t *>(vram)[a]; break;
            case PSMCT24: v = reinterpret_cast<const uint32_t *>(vram)[a] & 0xffffffu; break;
            case PSMCT16:
            case PSMCT16S: v = reinterpret_cast<const uint16_t *>(vram)[a]; break;
            case PSMT8: v = vram[a]; break;
            case PSMT8H: v = vram[4 * a + 3]; break;
            case PSMT4: v = (vram[a >> 1] >> ((a & 1) * 4)) & 0xfu; break;
            case PSMT4HL: v = (reinterpret_cast<const uint32_t *>(vram)[a] >> 24) & 0xfu; break;
            default: v = (reinterpret_cast<const uint32_t *>(vram)[a] >> 28) & 0xfu; break;
            }
            out[y * w + x] = v;
        }
}

bool read_region(const uint8_t *vram, uint32_t psm, uint32_t base, uint32_t tbw, uint32_t w, uint32_t h,
                 uint32_t mask, uint32_t *out)
{
    switch (psm)
    {
#define CASE(p) case p: read_texels<p>(vram, base, tbw, w, h, mask, out); return true
        CASE(PSMCT32);
        CASE(PSMCT24);
        CASE(PSMCT16);
        CASE(PSMCT16S);
        CASE(PSMT8);
        CASE(PSMT4);
        CASE(PSMT8H);
        CASE(PSMT4HL);
        CASE(PSMT4HH);
#undef CASE
    default: return false;
    }
}

uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t rgba16_to_rgba32(uint32_t c, uint32_t aem, uint32_t ta0, uint32_t ta1)
{
    uint32_t a = (aem && c == 0) ? 0 : ((c >> 15) & 1 ? ta1 : ta0);
    return pack_rgba((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, a);
}

uint32_t rgb24_to_rgba32(uint32_t c, uint32_t aem, uint32_t ta0)
{
    return pack_rgba(c & 255, (c >> 8) & 255, (c >> 16) & 255, (aem && (c & 0xffffff) == 0) ? 0 : ta0);
}

/* The GS's alpha 128 is a full one; in a PNG 255 is. */
inline uint8_t alpha_to_png(uint32_t a) { return a >= 128 ? 255 : uint8_t((a * 255 + 64) / 128); }
inline uint8_t alpha_from_png(uint32_t a) { return uint8_t((a * 128 + 127) / 255); }

/* FMT_WxH_16 hex digits[_16 hex digits], lower case (the name of a texture's key). */
bool valid_key_name(const std::string &n)
{
    static const char *const tags[] = {"c32", "c24", "c16", "t8", "t4", "t8h", "t4hl", "t4hh"};
    size_t us = n.find('_');
    if (us == std::string::npos)
        return false;
    bool tag = false;
    for (auto t : tags)
        tag = tag || n.compare(0, us, t) == 0;
    if (!tag)
        return false;
    size_t x = n.find('x', us + 1), us2 = n.find('_', us + 1);
    if (x == std::string::npos || us2 == std::string::npos || x > us2 || x == us + 1 || us2 == x + 1)
        return false;
    for (size_t i = us + 1; i < us2; i++)
        if (i != x && (n[i] < '0' || n[i] > '9'))
            return false;
    size_t rest = n.size() - us2 - 1;
    if (rest != 16 && rest != 33)
        return false;
    for (size_t i = us2 + 1; i < n.size(); i++)
    {
        bool hex = (n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f');
        if (i == us2 + 17 ? n[i] != '_' : !hex)
            return false;
    }
    return true;
}

/* ---------------------------------------------------------------- the dump worker */

struct DumpJob
{
    std::string path;
    uint32_t w = 0, h = 0, psm = 0, texa = 0;   /* texa: AEM | TA0 << 8 | TA1 << 16 */
    std::vector<uint32_t> texels;
    std::vector<uint32_t> palette;              /* RGBA, for the palettised formats */
    size_t bytes() const { return (texels.size() + palette.size()) * 4 + path.size(); }
};

/* ---------------------------------------------------------------- the pack */

enum FileState { UNLOADED = 0, LOADING = 1, READY = 2, FAILED = 3 };

struct PackFile
{
    std::string path;
    std::atomic<int> state{UNLOADED};
    std::vector<uint8_t> rgba;     /* READY: w x h, alpha in the GS's scale */
    uint32_t w = 0, h = 0;
    std::string error;             /* FAILED */
    bool failure_logged = false;
    bool bad_shape = false;        /* its size is not a whole multiple of its texture's (a key fixes the texture's size) */
    uint64_t last_use = 0;
};

struct Active
{
    PackFile *file = nullptr;
    uint32_t W = 0, H = 0;               /* the original texture */
    uint32_t rx = 0, ry = 0, rw = 0, rh = 0;   /* the rectangle the cached image covers */
    std::string key;
    Vulkan::ImageHandle image;
    bool bad = false;
};
}

/* ---------------------------------------------------------------- Impl */

struct Textures::Impl
{
    Vulkan::Device *device;
    uint32_t vram_size, num_pages, page_mask;
    Options opt;
    Counters c;

    /* the local copy of what the game uploaded */
    std::vector<uint8_t> shadow;
    std::vector<uint8_t> page_state;    /* 0 never written, 1 uploaded, 2 written by rendering or a copy since */
    std::vector<uint32_t> page_gen;
    uint32_t gen_counter = 0;
    uint16_t clut[512] = {};
    uint8_t clut_ok[512] = {};

    struct SigEntry { uint64_t gens, texels; };
    std::unordered_map<uint64_t, SigEntry> sigcache;
    std::vector<uint32_t> scratch;

    /* messages from the workers, logged on the renderer's thread */
    std::mutex msg_mutex;
    std::vector<std::string> messages;

    /* dumping */
    bool dumping = false;
    std::unordered_set<std::string> dumped;
    std::unordered_map<uint64_t, uint32_t> variants;   /* texture slot -> keys dumped for it */
    std::mutex dump_mutex;
    std::condition_variable dump_cv, dump_idle_cv;
    std::deque<DumpJob> dump_queue;
    size_t dump_queue_bytes = 0;
    bool dump_busy = false;
    bool dump_stop = false;
    std::thread dump_thread;
    bool limit_logged = false;

    /* the pack */
    bool packing = false;
    std::unordered_map<std::string, std::unique_ptr<PackFile>> files;
    std::unordered_map<uint64_t, Active> active;
    std::mutex load_mutex;
    std::condition_variable load_cv;
    std::deque<PackFile *> load_queue;
    bool load_stop = false;
    std::thread load_thread;
    std::atomic<uint64_t> loaded_bytes{0};
    uint64_t use_clock = 0;
    /* The images made from the pack's files, kept by file and cut-out: the GS makes a texture's cached image again
     * every time the game uploads it again (many times a frame for some), and a replacement is not uploaded again. */
    struct ImageEntry { Vulkan::ImageHandle image; uint64_t bytes = 0, last_use = 0; };
    std::unordered_map<uint64_t, ImageEntry> images;
    uint64_t image_bytes = 0;

    Impl(Vulkan::Device *dev, uint32_t vram, const Options &o)
        : device(dev), vram_size(vram), num_pages(vram / 8192), page_mask(vram / 8192 - 1), opt(o),
          shadow(vram, 0), page_state(vram / 8192, 0), page_gen(vram / 8192, 0)
    {
    }

    void log(const char *fmt, ...)
    {
        char buf[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (opt.log)
            opt.log(buf, opt.log_user);
        else
            fprintf(stderr, "%s\n", buf);
    }

    void post(const std::string &m)
    {
        std::lock_guard<std::mutex> l(msg_mutex);
        if (messages.size() < 64)
            messages.push_back(m);
    }

    void drain_messages()
    {
        std::vector<std::string> m;
        {
            std::lock_guard<std::mutex> l(msg_mutex);
            m.swap(messages);
        }
        for (auto &s : m)
            log("%s", s.c_str());
    }

    struct SeenUpload { uint64_t what, gens; };
    std::unordered_map<uint64_t, SeenUpload> uploads_seen;

    uint64_t pages_signature(const PageRect &r)
    {
        uint64_t g = 0x51ED270B;
        for_pages(r, [&](uint32_t page) { g = mix(g, page_gen[page]); });
        return g;
    }

    template <class F>
    void for_pages(const PageRect &r, F f)
    {
        for (uint32_t y = 0; y < r.page_height; y++)
            for (uint32_t x = 0; x < r.page_width; x++)
                f((r.base_page + y * r.page_stride + x) & page_mask);
    }

    /* ----- local copy */

    void upload(const CopyDescriptor &copy)
    {
        auto &b = copy.bitbltbuf.desc;
        auto &p = copy.trxpos.desc;
        auto &r = copy.trxreg.desc;
        uint32_t psm = uint32_t(b.DPSM), dbp = uint32_t(b.DBP), dbw = uint32_t(b.DBW);
        uint32_t x0 = uint32_t(p.DSAX), y0 = uint32_t(p.DSAY), w = uint32_t(r.RRW), h = uint32_t(r.RRH);

        /* The game sends the same texture again and again (it has little local memory). When the same data goes to the
         * same place and nothing has touched those pages since, the copy already holds it: nothing changes. */
        PageRect pr = compute_page_rect(dbp, x0, y0, w, h, dbw, psm);
        uint64_t where = mix(mix(mix(psm, dbp), mix(dbw, x0)), mix(mix(y0, w), h));
        uint64_t what = hash_words(static_cast<const uint32_t *>(copy.host_data), copy.host_data_size_required / 4, where);
        uint64_t gens = pages_signature(pr);
        auto seen = uploads_seen.find(where);
        if (seen != uploads_seen.end() && seen->second.what == what && seen->second.gens == gens)
        {
            c.uploads_skipped++;
            return;
        }

        bool known = true;
        switch (psm)
        {
#define CASE(P) case P: vram_upload<P>(shadow.data(), copy.host_data, dbp, dbw, x0, y0, w, h, vram_size - 1); break
            CASE(PSMT4); CASE(PSMT4HL); CASE(PSMT4HH); CASE(PSMT8); CASE(PSMT8H);
            CASE(PSMCT16); CASE(PSMCT16S); CASE(PSMZ16); CASE(PSMZ16S);
            CASE(PSMCT32); CASE(PSMZ32); CASE(PSMCT24); CASE(PSMZ24);
#undef CASE
        default: known = false; break;
        }
        if (!known)
        {
            damage(pr);
            return;
        }
        auto layout = get_data_structure(psm);
        uint32_t fx = x0 >> layout.page_width_log2, fy = y0 >> layout.page_height_log2;
        bool aligned = (dbp % 32) == 0;
        for (uint32_t iy = 0; iy < pr.page_height; iy++)
            for (uint32_t ix = 0; ix < pr.page_width; ix++)
            {
                uint32_t page = (pr.base_page + iy * pr.page_stride + ix) & page_mask;
                uint32_t px0 = (fx + ix) * layout.page_width, py0 = (fy + iy) * layout.page_height;
                bool covers = aligned && px0 >= x0 && px0 + layout.page_width <= x0 + w &&
                              py0 >= y0 && py0 + layout.page_height <= y0 + h;
                if (page_state[page] != 2 || covers)
                    page_state[page] = 1;
                page_gen[page] = ++gen_counter;
            }
        if (uploads_seen.size() > 20000)
            uploads_seen.clear();
        uploads_seen[where] = {what, pages_signature(pr)};
    }

    void damage(const PageRect &r)
    {
        for_pages(r, [this](uint32_t page) {
            if (page_state[page] == 1)
                page_state[page] = 2;
            page_gen[page] = ++gen_counter;
        });
    }

    void clut_load(const TEX0Bits &t, const TEXCLUTBits &tc)
    {
        uint32_t psm = uint32_t(t.PSM);
        if (!is_palette_format(psm))
            return;
        uint32_t cpsm = uint32_t(t.CPSM);
        if (cpsm == PSMCT24)
            cpsm = PSMCT32;
        bool ct32 = cpsm == PSMCT32;
        uint32_t csa = uint32_t(t.CSA);
        if (ct32)
            csa &= 15;
        bool index4 = psm == PSMT4 || psm == PSMT4HH || psm == PSMT4HL;
        bool csm2 = uint32_t(t.CSM) != 0;
        uint32_t cbp = uint32_t(t.CBP);
        uint32_t cbw = csm2 ? uint32_t(tc.CBW) : 0;
        uint32_t cou = csm2 ? uint32_t(tc.COU) : 0, cov = csm2 ? uint32_t(tc.COV) : 0;

        /* the pages the palette is read from must hold uploaded data */
        uint32_t pw, ph;
        if (psm == PSMT8 || psm == PSMT8H)
        { pw = csm2 ? 256 : 16; ph = csm2 ? 1 : 16; }
        else
        { pw = csm2 ? 16 : 8; ph = csm2 ? 1 : 4; }
        PageRect pr = compute_page_rect(cbp, csm2 ? cou * 16 : 0, csm2 ? cov : 0, pw, ph, cbw, cpsm);
        bool src_ok = true;
        for_pages(pr, [&](uint32_t page) { if (page_state[page] != 1) src_ok = false; });

        uint32_t entries = index4 ? 16 : 256;
        uint32_t mask = vram_size - 1;
        const uint32_t *v32 = reinterpret_cast<const uint32_t *>(shadow.data());
        const uint16_t *v16 = reinterpret_cast<const uint16_t *>(shadow.data());
        for (uint32_t entry = 0; entry < entries; entry++)
        {
            uint32_t write = entry + csa * 16, read = entry;
            if (!index4 && !csm2)
            {
                uint32_t csa_count = ct32 ? 16 : 32;
                if (entry >= (csa_count - csa) * 16)
                    continue;
                read += csa * 16;
            }
            uint32_t x, y;
            if (csm2)
            {
                x = read + cou * 16;
                y = cov;
            }
            else
            {
                x = (read & 7) | (((read >> 4) & 1) << 3);
                y = ((read >> 3) & 1) | (((read >> 5) & 7) << 1);
            }
            if (write >= 512)
                continue;
            if (ct32)
            {
                uint32_t a = swizzle_PS2(x, y, cbp, cbw, PSMCT32, mask);
                uint32_t v = v32[a];
                clut[write] = uint16_t(v);
                clut_ok[write] = src_ok;
                if (write + 256 < 512)
                {
                    clut[write + 256] = uint16_t(v >> 16);
                    clut_ok[write + 256] = src_ok;
                }
            }
            else
            {
                uint32_t a = swizzle_PS2(x, y, cbp, cbw, cpsm, mask);
                clut[write] = v16[a];
                clut_ok[write] = src_ok;
            }
        }
    }

    /* ----- keys and dumps */

    struct Key
    {
        std::string base, exact;
        bool palettised = false;
        uint32_t W = 0, H = 0, psm = 0, texa = 0;
        uint64_t slot = 0;
        std::vector<uint32_t> palette;
    };

    bool make_key(const TextureDescriptor &desc, Key &k)
    {
        uint32_t psm = uint32_t(desc.tex0.desc.PSM);
        const char *tag = format_tag(psm);
        if (!tag)
            return false;
        uint32_t W = 1u << uint32_t(desc.tex0.desc.TW), H = 1u << uint32_t(desc.tex0.desc.TH);
        uint32_t tbp = uint32_t(desc.tex0.desc.TBP0), tbw = uint32_t(desc.tex0.desc.TBW);
        PageRect pr = compute_page_rect(tbp, 0, 0, W, H, tbw, psm);
        bool ok = true;
        uint64_t gens = 0x51ED270B;
        for_pages(pr, [&](uint32_t page) {
            if (page_state[page] != 1)
                ok = false;
            gens = mix(gens, page_gen[page]);
        });
        if (!ok)
            return false;

        uint64_t slot = mix(mix(mix(psm, tbp), mix(tbw, uint64_t(desc.tex0.desc.TW))), uint64_t(desc.tex0.desc.TH));
        uint32_t texa = 0;
        bool pal = is_palette_format(psm);
        if (!pal && (psm == PSMCT16 || psm == PSMCT16S || psm == PSMCT24))
            texa = uint32_t(desc.texa.desc.AEM) | (uint32_t(desc.texa.desc.TA0) << 8) | (uint32_t(desc.texa.desc.TA1) << 16);
        slot = mix(slot, texa);
        uint64_t texels;
        auto it = sigcache.find(slot);
        if (it != sigcache.end() && it->second.gens == gens)
            texels = it->second.texels;
        else
        {
            scratch.resize(size_t(W) * H);
            if (!read_region(shadow.data(), psm, tbp, tbw, W, H, vram_size - 1, scratch.data()))
                return false;
            texels = hash_words(scratch.data(), scratch.size(), mix(mix(psm, W), mix(H, texa)));
            sigcache[slot] = {gens, texels};
            if (sigcache.size() > 20000)
                sigcache.clear();
            c.hashed++;
        }

        k.W = W;
        k.H = H;
        k.psm = psm;
        k.texa = texa;
        k.slot = slot;
        k.palettised = pal;
        char buf[96];
        snprintf(buf, sizeof(buf), "%s_%ux%u_%016llx", tag, W, H, (unsigned long long)texels);
        k.base = buf;
        k.exact = buf;
        if (pal)
        {
            uint32_t n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
            uint32_t cpsm = uint32_t(desc.tex0.desc.CPSM);
            bool ct32 = cpsm == PSMCT32 || cpsm == PSMCT24;
            uint32_t csa = uint32_t(desc.tex0.desc.CSA) & (ct32 ? 15u : 31u);
            uint32_t aem = uint32_t(desc.texa.desc.AEM), ta0 = uint32_t(desc.texa.desc.TA0), ta1 = uint32_t(desc.texa.desc.TA1);
            k.palette.resize(n);
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t idx = csa * 16 + i;
                if (n == 256)
                {
                    uint32_t bank = std::min<uint32_t>(idx >> 4, ct32 ? 15u : 31u);
                    idx = bank * 16 + (idx & 15);
                }
                if (idx >= 512 || !clut_ok[idx] || (ct32 && (idx >= 256 || !clut_ok[idx + 256])))
                    return false;
                k.palette[i] = ct32 ? (uint32_t(clut[idx]) | (uint32_t(clut[idx + 256]) << 16))
                                    : rgba16_to_rgba32(clut[idx], aem, ta0, ta1);
            }
            uint64_t ch = hash_words(k.palette.data(), k.palette.size(), 0xC1075EED);
            snprintf(buf, sizeof(buf), "_%016llx", (unsigned long long)ch);
            k.exact += buf;
        }
        return true;
    }

    void dump(const TextureDescriptor &desc, const Key &k)
    {
        if (dumped.count(k.exact))
            return;
        if (dumped.size() >= opt.dump_limit)
        {
            if (!limit_logged)
            {
                limit_logged = true;
                log("texture dump: the limit of %u files is reached; no more are written", opt.dump_limit);
            }
            return;
        }
        uint32_t &n = variants[k.slot];
        if (n >= 8)
            return;   /* a texture slot that changes all the time (a movie, an animation): its first few keys only */
        DumpJob job;
        job.path = opt.dump_dir + "/" + k.exact + ".png";
        job.w = k.W;
        job.h = k.H;
        job.psm = k.psm;
        job.texa = k.texa;
        job.palette = k.palette;
        job.texels.resize(size_t(k.W) * k.H);
        if (!read_region(shadow.data(), k.psm, uint32_t(desc.tex0.desc.TBP0), uint32_t(desc.tex0.desc.TBW), k.W, k.H,
                         vram_size - 1, job.texels.data()))
            return;
        {
            std::lock_guard<std::mutex> l(dump_mutex);
            if (dump_queue.size() >= 128 || dump_queue_bytes > (256u << 20))
            {
                c.dump_dropped++;
                return;   /* asked again at the next draw */
            }
            dump_queue_bytes += job.bytes();
            dump_queue.push_back(std::move(job));
        }
        dump_cv.notify_one();
        dumped.insert(k.exact);
        n++;
    }

    void dump_worker()
    {
        std::vector<uint8_t> rgba;
        for (;;)
        {
            DumpJob job;
            {
                std::unique_lock<std::mutex> l(dump_mutex);
                dump_busy = false;
                dump_idle_cv.notify_all();
                dump_cv.wait(l, [this] { return dump_stop || !dump_queue.empty(); });
                if (dump_queue.empty())
                    return;
                job = std::move(dump_queue.front());
                dump_queue.pop_front();
                dump_queue_bytes -= job.bytes();
                dump_busy = true;
            }
            if (file_exists(job.path))
            {
                c_dump_existing++;
                continue;
            }
            rgba.resize(job.texels.size() * 4);
            uint32_t aem = job.texa & 1, ta0 = (job.texa >> 8) & 255, ta1 = (job.texa >> 16) & 255;
            bool pal = is_palette_format(job.psm);
            for (size_t i = 0; i < job.texels.size(); i++)
            {
                uint32_t v = job.texels[i], px;
                if (pal)
                    px = job.palette[std::min<size_t>(v, job.palette.size() - 1)];
                else if (job.psm == PSMCT32)
                    px = v;
                else if (job.psm == PSMCT24)
                    px = rgb24_to_rgba32(v, aem, ta0);
                else
                    px = rgba16_to_rgba32(v, aem, ta0, ta1);
                rgba[i * 4 + 0] = uint8_t(px);
                rgba[i * 4 + 1] = uint8_t(px >> 8);
                rgba[i * 4 + 2] = uint8_t(px >> 16);
                rgba[i * 4 + 3] = alpha_to_png(px >> 24);
            }
            std::string tmp = job.path + ".tmp";
            if (stbi_write_png(tmp.c_str(), int(job.w), int(job.h), 4, rgba.data(), int(job.w * 4)) && rename_file(tmp, job.path))
                c_dumped++;
            else
            {
                remove_file(tmp);
                if (c_dump_errors++ < 3)
                    post("texture dump: cannot write " + job.path);
            }
        }
    }
    std::atomic<uint64_t> c_dumped{0}, c_dump_existing{0}, c_dump_errors{0};

    /* ----- the pack */

    void index_pack()
    {
        std::vector<std::string> all;
        list_files(opt.pack_dir, all);
        size_t dup = 0, misnamed = 0;
        for (auto &p : all)
        {
            size_t slash = p.find_last_of("/\\");
            std::string name = p.substr(slash == std::string::npos ? 0 : slash + 1);
            if (name.size() < 5)
                continue;
            std::string ext = name.substr(name.size() - 4);
            for (auto &ch : ext)
                ch = char(tolower((unsigned char)ch));
            if (ext != ".png")
                continue;
            std::string stem = name.substr(0, name.size() - 4);
            for (auto &ch : stem)
                ch = char(tolower((unsigned char)ch));
            if (!valid_key_name(stem))
            {
                if (misnamed++ < 5)
                    log("texture pack: %s is not named like a texture's key (FMT_WxH_texels[_palette]); skipped", p.c_str());
                continue;
            }
            std::unique_ptr<PackFile> f(new PackFile);
            f->path = p;
            if (!files.emplace(stem, std::move(f)).second)
                dup++;
        }
        c.pack_files = files.size();
        c.pack_misnamed = misnamed;
        if (misnamed > 5)
            log("texture pack: %zu more files are not named like a texture's key; skipped", misnamed - 5);
        log("texture pack: %zu files in %s%s", files.size(), opt.pack_dir.c_str(),
            dup ? " (a name found twice uses the first file found)" : "");
    }

    void load_worker()
    {
        for (;;)
        {
            PackFile *f;
            {
                std::unique_lock<std::mutex> l(load_mutex);
                load_cv.wait(l, [this] { return load_stop || !load_queue.empty(); });
                if (load_stop)
                    return;
                f = load_queue.front();
                load_queue.pop_front();
            }
            int w = 0, h = 0, n = 0;
            uint8_t *px = stbi_load(f->path.c_str(), &w, &h, &n, 4);
            if (!px)
            {
                const char *why = stbi_failure_reason();
                f->error = why ? why : "unreadable";
                f->state.store(FAILED, std::memory_order_release);
                continue;
            }
            f->rgba.assign(px, px + size_t(w) * h * 4);
            stbi_image_free(px);
            for (size_t i = 3; i < f->rgba.size(); i += 4)
                f->rgba[i] = alpha_from_png(f->rgba[i]);
            f->w = uint32_t(w);
            f->h = uint32_t(h);
            loaded_bytes += f->rgba.size();
            f->state.store(READY, std::memory_order_release);
        }
    }

    void queue_load(PackFile *f)
    {
        f->state.store(LOADING, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> l(load_mutex);
            load_queue.push_back(f);
        }
        load_cv.notify_one();
    }

    void evict()
    {
        uint64_t budget = uint64_t(opt.cache_mb) << 20;
        while (loaded_bytes.load() > budget)
        {
            PackFile *oldest = nullptr;
            for (auto &kv : files)
            {
                PackFile *f = kv.second.get();
                if (f->state.load(std::memory_order_acquire) == READY && !f->rgba.empty() &&
                    (!oldest || f->last_use < oldest->last_use))
                    oldest = f;
            }
            if (!oldest)
                return;
            loaded_bytes -= oldest->rgba.size();
            std::vector<uint8_t>().swap(oldest->rgba);
            oldest->state.store(UNLOADED, std::memory_order_relaxed);
        }
    }

    PackFile *find_file(const std::string &name)
    {
        std::string low = name;
        for (auto &ch : low)
            ch = char(tolower((unsigned char)ch));
        auto it = files.find(low);
        return it == files.end() ? nullptr : it->second.get();
    }

    Vulkan::ImageHandle make_image(Active &a)
    {
        PackFile *f = a.file;
        f->last_use = ++use_clock;
        if (f->bad_shape)
        {
            a.bad = true;
            return {};
        }
        if (f->w % a.W || f->h % a.H || f->w / a.W != f->h / a.H || f->w / a.W == 0 || f->w > 16384 || f->h > 16384 ||
            a.rx + a.rw > a.W || a.ry + a.rh > a.H)
        {
            a.bad = true;
            f->bad_shape = true;
            if (!f->failure_logged)
            {
                f->failure_logged = true;
                c.pack_bad++;
                log("texture pack: %s is %ux%u, but the texture is %ux%u: the size of a replacement is a whole "
                    "multiple (1x, 2x, 4x ...) of the original's; skipped",
                    f->path.c_str(), f->w, f->h, a.W, a.H);
            }
            return {};
        }
        uint32_t k = f->w / a.W;
        uint64_t ikey = mix(mix(mix(uint64_t(reinterpret_cast<uintptr_t>(f)), a.rx), mix(a.ry, a.rw)), a.rh);
        auto found = images.find(ikey);
        if (found != images.end())
        {
            found->second.last_use = use_clock;
            return found->second.image;
        }
        Vulkan::ImageCreateInfo info = Vulkan::ImageCreateInfo::immutable_2d_image(a.rw * k, a.rh * k, VK_FORMAT_R8G8B8A8_UNORM);
        Vulkan::ImageInitialData init = {};
        init.data = f->rgba.data() + (size_t(a.ry * k) * f->w + size_t(a.rx * k)) * 4;
        init.row_length = f->w;
        init.image_height = a.rh * k;
        Vulkan::ImageHandle img = device->create_image(info, &init);
        if (!img)
        {
            a.bad = true;
            log("texture pack: cannot make the image for %s", f->path.c_str());
            return {};
        }
        c.replaced_images++;
        ImageEntry &e = images[ikey];
        e.image = img;
        e.bytes = uint64_t(info.width) * info.height * 4;
        e.last_use = use_clock;
        image_bytes += e.bytes;
        uint64_t budget = uint64_t(opt.cache_mb) << 20;
        while (image_bytes > budget && images.size() > 1)
        {
            auto oldest = images.end();
            for (auto it = images.begin(); it != images.end(); ++it)
                if (it->first != ikey && (oldest == images.end() || it->second.last_use < oldest->second.last_use))
                    oldest = it;
            if (oldest == images.end())
                break;
            image_bytes -= oldest->second.bytes;
            images.erase(oldest);   /* a render pass that still samples it holds it */
        }
        return img;
    }
};

/* ---------------------------------------------------------------- Textures */

namespace
{
using Clock = std::chrono::steady_clock;
inline uint64_t ns_since(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }
}

Textures::Textures(Vulkan::Device *device, uint32_t vram_size, const Options &options)
    : impl(new Impl(device, vram_size, options))
{
    Impl &m = *impl;
    if (!m.opt.dump_dir.empty())
    {
        if (make_dirs(m.opt.dump_dir))
        {
            m.dumping = true;
            m.dump_thread = std::thread([&m] { m.dump_worker(); });
            m.log("texture dump: writing to %s", m.opt.dump_dir.c_str());
        }
        else
            m.log("texture dump: cannot make the folder %s; no dump", m.opt.dump_dir.c_str());
    }
    if (!m.opt.pack_dir.empty())
    {
        if (file_exists(m.opt.pack_dir))
        {
            m.index_pack();
            m.packing = !m.files.empty();
            if (m.packing)
                m.load_thread = std::thread([&m] { m.load_worker(); });
        }
        else
            m.log("texture pack: the folder %s does not exist; nothing is replaced", m.opt.pack_dir.c_str());
    }
}

Textures::~Textures()
{
    Impl &m = *impl;
    if (m.dump_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(m.dump_mutex);
            m.dump_stop = true;
        }
        m.dump_cv.notify_all();
        m.dump_thread.join();   /* it writes what is queued first */
    }
    if (m.load_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(m.load_mutex);
            m.load_stop = true;
        }
        m.load_cv.notify_all();
        m.load_thread.join();
    }
    m.drain_messages();
    Counters c = counters();
    if (m.dumping || m.packing)
    {
        m.log("textures: %llu looked at, %llu keyed, %llu not (data not uploaded by the game, or drawn by the GS), "
              "%llu regions hashed",
              (unsigned long long)c.textures, (unsigned long long)c.keyed, (unsigned long long)c.unkeyed,
              (unsigned long long)c.hashed);
        m.log("textures: %.1f ms in %llu uploads (longest %.2f ms), %.1f ms in new textures (longest %.2f ms), "
              "%.1f ms in %llu lookups (longest %.2f ms)",
              c.upload_ns / 1e6, (unsigned long long)c.uploads, c.upload_max_ns / 1e6, c.texture_ns / 1e6,
              c.texture_max_ns / 1e6, c.lookup_ns / 1e6, (unsigned long long)c.lookups, c.lookup_max_ns / 1e6);
    }
    if (m.dumping)
        m.log("texture dump: %llu files written, %llu there already, %llu dropped for a full queue, %llu errors",
              (unsigned long long)c.dumped, (unsigned long long)c.dump_existing, (unsigned long long)c.dump_dropped,
              (unsigned long long)c.dump_errors);
    if (m.packing)
        m.log("texture pack: a pack file was used %llu times, %llu images made, %llu files could not be used",
              (unsigned long long)c.pack_hits, (unsigned long long)c.replaced_images, (unsigned long long)c.pack_bad);
    m.active.clear();
    m.images.clear();
    delete impl;
}

size_t Textures::state_bytes(uint32_t vram_size)
{
    return vram_size / 8192 + sizeof(Impl::clut) + sizeof(Impl::clut_ok);
}

void Textures::state_get(std::vector<uint8_t> &out) const
{
    const Impl &m = *impl;
    size_t at = out.size();
    out.resize(at + state_bytes(m.vram_size));
    memcpy(out.data() + at, m.page_state.data(), m.num_pages);
    memcpy(out.data() + at + m.num_pages, m.clut, sizeof(m.clut));
    memcpy(out.data() + at + m.num_pages + sizeof(m.clut), m.clut_ok, sizeof(m.clut_ok));
}

void Textures::state_set(const uint8_t *state, const uint8_t *vram)
{
    Impl &m = *impl;
    for (uint32_t page = 0; page < m.num_pages; page++)
    {
        uint8_t st = state ? state[page] : 2;
        if (st > 2)
            st = 2;
        /* pages that no rendering or copy has written hold, in local memory, what the uploads put there */
        if (st != 2)
            memcpy(m.shadow.data() + size_t(page) * 8192, vram + size_t(page) * 8192, 8192);
        m.page_state[page] = st;
        m.page_gen[page] = ++m.gen_counter;   /* every cache keyed by the pages' generations is stale now */
    }
    if (state)
    {
        memcpy(m.clut, state + m.num_pages, sizeof(m.clut));
        memcpy(m.clut_ok, state + m.num_pages + sizeof(m.clut), sizeof(m.clut_ok));
    }
    else
    {
        memset(m.clut_ok, 0, sizeof(m.clut_ok));
    }
    m.sigcache.clear();
    m.uploads_seen.clear();
}

void Textures::on_host_upload(const CopyDescriptor &copy)
{
    if (copy.host_data_size < copy.host_data_size_required)
        return;
    auto t0 = Clock::now();
    impl->upload(copy);
    impl->c.uploads++;
    uint64_t ns = ns_since(t0);
    impl->c.upload_ns += ns;
    impl->c.upload_max_ns = std::max(impl->c.upload_max_ns, ns);
}

void Textures::on_vram_damage(const PageRect &rect)
{
    impl->damage(rect);
}

void Textures::on_clut_load(const TEX0Bits &tex0, const TEXCLUTBits &texclut)
{
    impl->clut_load(tex0, texclut);
}

void Textures::on_new_texture(const TextureDescriptor &desc)
{
    Impl &m = *impl;
    auto t0 = Clock::now();
    m.c.textures++;
    m.drain_messages();
    Impl::Key k;
    uint64_t hash = desc.hash;
    m.active.erase(hash);
    if (!m.make_key(desc, k))
    {
        m.c.unkeyed++;
        m.c.texture_ns += ns_since(t0);
        return;
    }
    m.c.keyed++;
    if (m.dumping)
        m.dump(desc, k);
    if (m.packing)
    {
        PackFile *f = m.find_file(k.exact);
        if (!f && k.palettised)
            f = m.find_file(k.base);
        /* a region-repeat texture is built by the GS from the region (masked and or-ed coordinates), not cut from the
         * texture: its image is not a crop of the original's, so a replacement is not made for it */
        if (f && !desc.clamp.desc.has_region_repeat())
        {
            m.c.pack_hits++;
            if (m.active.size() > 16384)
                m.active.clear();
            Active &a = m.active[hash];
            a.file = f;
            a.W = k.W;
            a.H = k.H;
            a.rx = desc.rect.x;
            a.ry = desc.rect.y;
            a.rw = desc.rect.width;
            a.rh = desc.rect.height;
            a.key = k.exact;
            if (f->state.load(std::memory_order_acquire) == UNLOADED)
                m.queue_load(f);
        }
    }
    uint64_t ns = ns_since(t0);
    m.c.texture_ns += ns;
    m.c.texture_max_ns = std::max(m.c.texture_max_ns, ns);
}

void Textures::on_texture_dead(Util::Hash hash)
{
    if (impl->packing)
        impl->active.erase(hash);
}

Vulkan::ImageHandle Textures::replacement_for(Util::Hash hash)
{
    Impl &m = *impl;
    if (!m.packing || !m.device || m.active.empty())
        return {};
    auto t0 = Clock::now();
    m.c.lookups++;
    Vulkan::ImageHandle result;
    auto it = m.active.find(hash);
    if (it != m.active.end() && !it->second.bad)
    {
        Active &a = it->second;
        if (a.image)
            result = a.image;
        else
        {
            PackFile *f = a.file;
            int st = f->state.load(std::memory_order_acquire);
            if (st == READY)
            {
                a.image = m.make_image(a);
                result = a.image;
                if (a.image)
                {
                    m.c.pack_loaded++;
                    m.evict();
                }
            }
            else if (st == FAILED)
            {
                a.bad = true;
                if (!f->failure_logged)
                {
                    f->failure_logged = true;
                    m.c.pack_bad++;
                    m.log("texture pack: cannot read %s (%s); skipped", f->path.c_str(), f->error.c_str());
                }
            }
            else if (st == UNLOADED)
                m.queue_load(f);
        }
    }
    uint64_t ns = ns_since(t0);
    m.c.lookup_ns += ns;
    m.c.lookup_max_ns = std::max(m.c.lookup_max_ns, ns);
    return result;
}

void Textures::flush_dumps()
{
    Impl &m = *impl;
    std::unique_lock<std::mutex> l(m.dump_mutex);
    m.dump_idle_cv.wait(l, [&m] { return m.dump_queue.empty() && !m.dump_busy; });
}

Counters Textures::counters() const
{
    Counters c = impl->c;
    c.dumped = impl->c_dumped.load();
    c.dump_existing = impl->c_dump_existing.load();
    c.dump_errors = impl->c_dump_errors.load();
    return c;
}
}
