/*
 * The text pack's text table (platform layer, host part): replacement strings for the text the program draws, keyed by the
 * original string. Host_TextLookup is what the text layer's entry points ask (PC_TEXT, include/pal.h): the replacement for a
 * string, or the string itself.
 *
 * The pack's text.tsv (UTF-8, one entry a line): KEY, a tab, SIG, a tab, TEXT. A line that starts with '#', and a blank line, is
 * a comment. KEY is 16 lower-case hex digits: the first 8 bytes of the SHA-256 of the original string's bytes (without the NUL)
 * as the text layer is given it, a printf format string before its arguments are inserted. SIG is the original's % conversions
 * written one after the other (%d%-8s), or - for none; the replacement must have exactly those, in that order (the game passes
 * the arguments of the original). TEXT is the replacement: ASCII, the characters of the pack's character map (charmap.txt: one for
 * each of the bytes 0xA1 to 0xDF, the first line is 0xA1), and \xNN for any byte as it is; \\ is a backslash, \n a line feed, \t, \r.
 * A pack that keeps Japanese characters writes their Shift-JIS bytes as \xNN. Each entry is checked at load: the conversions
 * against SIG, the ~ codes well-formed (~~, ~%, ~Cnn, ~Ann, ~Bnn), no NUL, and a length that fits the text buffer (TEXT_BUF_SIZE,
 * 1,024 bytes with the NUL). A table that fails a check is refused as a whole, with its line.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "host_file.h"
#include "io/host_sha256.h"
#include "io/io_host.h"

#define TEXT_LIMIT 1024   /* TEXT_BUF_SIZE (include/text.h) */

typedef struct Entry {
    uint64_t key;     /* the first 8 bytes of the SHA-256, big-endian */
    char *text;       /* NULL: free */
} Entry;

static Entry *g_tab;
static unsigned int g_cap, g_count;
static char g_why[600];

static uint64_t KeyOf(const char *s)
{
    HostSha256 h;
    char hex[65];
    uint64_t k = 0;
    HostSha256_Init(&h);
    HostSha256_Update(&h, s, strlen(s));
    HostSha256_Final(&h, hex);
    for (int i = 0; i < 16; i++)
        k = k << 4 | (uint64_t)(hex[i] <= '9' ? hex[i] - '0' : hex[i] - 'a' + 10);
    return k;
}

static Entry *Find(uint64_t key)
{
    if (!g_cap)
        return NULL;
    unsigned int i = (unsigned int)(key ^ key >> 29) & (g_cap - 1);
    while (g_tab[i].text) {
        if (g_tab[i].key == key)
            return &g_tab[i];
        i = (i + 1) & (g_cap - 1);
    }
    return NULL;
}

static int Insert(uint64_t key, char *text)
{
    if ((g_count + 1) * 2 > g_cap) {
        unsigned int ncap = g_cap ? g_cap * 2 : 1024;
        Entry *nt = calloc(ncap, sizeof(Entry));
        if (!nt)
            return -1;
        for (unsigned int i = 0; i < g_cap; i++)
            if (g_tab[i].text) {
                unsigned int j = (unsigned int)(g_tab[i].key ^ g_tab[i].key >> 29) & (ncap - 1);
                while (nt[j].text)
                    j = (j + 1) & (ncap - 1);
                nt[j] = g_tab[i];
            }
        free(g_tab);
        g_tab = nt;
        g_cap = ncap;
    }
    unsigned int i = (unsigned int)(key ^ key >> 29) & (g_cap - 1);
    while (g_tab[i].text)
        i = (i + 1) & (g_cap - 1);
    g_tab[i].key = key;
    g_tab[i].text = text;
    g_count++;
    return 0;
}

HOST void Host_TextClose(void)
{
    for (unsigned int i = 0; i < g_cap; i++)
        free(g_tab[i].text);
    free(g_tab);
    g_tab = NULL;
    g_cap = g_count = 0;
}

HOST const char *Host_TextWhy(void)
{
    return g_why;
}

HOST unsigned int Host_TextCount(void)
{
    return g_count;
}

/* ---- the character map ---- */

static int g_map[256];   /* code point for a byte 0xA1 to 0xDF, 0: none */

static int Utf8One(const unsigned char *s, int n, int *cp)
{
    if (n >= 1 && s[0] < 0x80) { *cp = s[0]; return 1; }
    if (n >= 2 && s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) { *cp = (s[0] & 0x1F) << 6 | (s[1] & 0x3F); return 2; }
    if (n >= 3 && s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = (s[0] & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        if (*cp < 0x800 || (*cp >= 0xD800 && *cp <= 0xDFFF))
            return 0;
        return 3;
    }
    if (n >= 4 && s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = (s[0] & 0x07) << 18 | (s[1] & 0x3F) << 12 | (s[2] & 0x3F) << 6 | (s[3] & 0x3F);
        return *cp >= 0x10000 && *cp <= 0x10FFFF ? 4 : 0;
    }
    return 0;
}

static int LoadMap(const char *path)
{
    memset(g_map, 0, sizeof(g_map));
    if (!path)
        return 0;
    FILE *f = HostFile_Open(path, "rb");
    if (!f) {
        snprintf(g_why, sizeof(g_why), "the character map %s can't be read", path);
        return -1;
    }
    static unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == sizeof(buf)) {
        snprintf(g_why, sizeof(g_why), "the character map %s is over 8 KB", path);
        return -1;
    }
    size_t p = 0;
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        p = 3;
    int line = 0;
    while (p < n) {
        size_t e = p;
        while (e < n && buf[e] != '\n')
            e++;
        size_t t = e;
        if (t > p && buf[t - 1] == '\r')
            t--;
        line++;
        if (line > 63) {
            snprintf(g_why, sizeof(g_why), "the character map %s has more than 63 lines (the bytes 0xA1 to 0xDF)", path);
            return -1;
        }
        if (t > p) {
            int cp = 0, len = Utf8One(buf + p, (int)(t - p), &cp);
            if (!len || (size_t)len != t - p) {
                snprintf(g_why, sizeof(g_why), "the character map %s, line %d: one UTF-8 character a line", path, line);
                return -1;
            }
            for (int b = 0xA1; b < 0xA1 + line - 1; b++)
                if (g_map[b] == cp) {
                    snprintf(g_why, sizeof(g_why), "the character map %s, line %d: the character is already on line %d", path, line, b - 0xA0);
                    return -1;
                }
            g_map[0xA1 + line - 1] = cp;
        }
        p = e + 1;
    }
    return 0;
}

/* ---- one entry ---- */

static int HexVal(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The replacement's text (UTF-8 with escapes) as the game's bytes; NULL with the reason in why. */
static char *Convert(const char *t, size_t n, char *why, size_t whyCap)
{
    char *out = malloc(n + 1);
    size_t o = 0;
    if (!out)
        return NULL;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\\') {
            if (i + 1 >= n) { snprintf(why, whyCap, "a backslash ends the text"); goto bad; }
            char e = t[i + 1];
            if (e == '\\') { out[o++] = '\\'; i += 2; }
            else if (e == 'n') { out[o++] = '\n'; i += 2; }
            else if (e == 't') { out[o++] = '\t'; i += 2; }
            else if (e == 'r') { out[o++] = '\r'; i += 2; }
            else if (e == 'x' && i + 3 < n && HexVal(t[i + 2]) >= 0 && HexVal(t[i + 3]) >= 0) {
                int v = HexVal(t[i + 2]) << 4 | HexVal(t[i + 3]);
                if (v == 0) { snprintf(why, whyCap, "\\x00 would end the string"); goto bad; }
                out[o++] = (char)v;
                i += 4;
            } else { snprintf(why, whyCap, "an escape that is not \\\\, \\n, \\t, \\r or \\xNN (two lower-case hex digits)"); goto bad; }
            continue;
        }
        if (c < 0x80) {
            if (c < 0x20 && c != '\t') { snprintf(why, whyCap, "a control character (write \\n or \\xNN)"); goto bad; }
            out[o++] = (char)c;
            i++;
            continue;
        }
        int cp = 0, len = Utf8One((const unsigned char *)t + i, (int)(n - i), &cp);
        if (!len) { snprintf(why, whyCap, "bytes that are not UTF-8"); goto bad; }
        int b;
        for (b = 0xA1; b <= 0xDF; b++)
            if (g_map[b] == cp)
                break;
        if (b > 0xDF) {
            snprintf(why, whyCap, "the character U+%04X is not in the character map (a Japanese character is written as its Shift-JIS bytes, \\xNN)", cp);
            goto bad;
        }
        out[o++] = (char)b;
        i += (size_t)len;
    }
    out[o] = 0;
    return out;
bad:
    free(out);
    return NULL;
}

/* The ~ codes and % conversions of a converted string. ~~ ~% ~Cnn ~Ann ~Bnn; a conversion is % flags width .precision
 * length letter, or %%. Returns NULL when well-formed, or a reason. */
static const char *CheckCodes(const char *s, char *conv, size_t convCap)
{
    size_t cl = 0;
    conv[0] = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x81 && *p <= 0x9F && p[1]) {   /* the first byte of a two-byte character: skip its second */
            p++;
            continue;
        }
        if (*p >= 0xE0 && p[1]) {
            p++;
            continue;
        }
        if (*p == '~') {
            if (p[1] == '~' || p[1] == '%') {
                p++;
            } else if ((p[1] == 'A' || p[1] == 'B' || p[1] == 'C') && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
                p += 3;
            } else {
                return "a ~ that is not ~~, ~%, ~Cnn, ~Ann or ~Bnn (nn two digits)";
            }
        } else if (*p == '%') {
            const unsigned char *spec = p;   /* the '%' */
            p++;
            if (*p == '%')
                continue;
            while (*p && strchr("-+ #0", *p))
                p++;
            if (*p == '*')
                p++;
            else
                while (*p >= '0' && *p <= '9')
                    p++;
            if (*p == '.') {
                p++;
                if (*p == '*')
                    p++;
                else
                    while (*p >= '0' && *p <= '9')
                        p++;
            }
            while (*p && strchr("hlLzjt", *p))
                p++;
            if (!*p || !strchr("diouxXcsp", *p))
                return "a % that is not a conversion (diouxXcsp, or %%)";
            if (cl + (size_t)(p - spec) + 2 >= convCap)
                return "too many conversions";
            memcpy(conv + cl, spec, (size_t)(p - spec) + 1);
            cl += (size_t)(p - spec) + 1;
            conv[cl] = 0;
        }
    }
    return NULL;
}

HOST int Host_TextLoad(const char *dir, const char *charmap)
{
    char path[4096 + 100];
    g_why[0] = 0;
    Host_TextClose();
    snprintf(path, sizeof(path), "%s/text.tsv", dir);
    FILE *f = HostFile_Open(path, "rb");
    if (!f)
        return 0;   /* a pack with no text table: its files only */
    char cm[4096 + 100];
    if (charmap) {
        snprintf(cm, sizeof(cm), "%s/%s", dir, charmap);
        if (LoadMap(cm) != 0) {
            fclose(f);
            return -1;
        }
    } else {
        memset(g_map, 0, sizeof(g_map));
    }
    char *data = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 65536 > cap) {
            cap = cap ? cap * 2 : 1 << 20;
            char *nd = realloc(data, cap);
            if (!nd) { free(data); fclose(f); snprintf(g_why, sizeof(g_why), "out of memory"); return -1; }
            data = nd;
        }
        size_t got = fread(data + n, 1, cap - n, f);
        if (!got)
            break;
        n += got;
    }
    fclose(f);
    size_t p = 0;
    if (n >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        p = 3;
    unsigned int line = 0;
    int rc = 0;
    while (p < n && rc == 0) {
        size_t e = p;
        while (e < n && data[e] != '\n')
            e++;
        size_t t = e;
        if (t > p && data[t - 1] == '\r')
            t--;
        line++;
        const char *l = data + p;
        size_t len = t - p;
        p = e + 1;
        if (len == 0 || l[0] == '#')
            continue;
        char why[400];
        why[0] = 0;
        const char *tab2 = len > 17 && l[16] == '\t' ? memchr(l + 17, '\t', len - 17) : NULL;
        if (!tab2) {
            snprintf(why, sizeof(why), "the line is not KEY (16 hex digits), a tab, SIG, a tab, and the text");
        } else {
            uint64_t key = 0;
            int ok = 1;
            for (int i = 0; i < 16; i++) {
                int v = HexVal(l[i]);
                if (v < 0) { ok = 0; break; }
                key = key << 4 | (uint64_t)v;
            }
            if (!ok) {
                snprintf(why, sizeof(why), "the key is not 16 lower-case hex digits");
            } else if (Find(key)) {
                snprintf(why, sizeof(why), "the key is given twice");
            } else {
                char *txt = Convert(tab2 + 1, len - (size_t)(tab2 + 1 - l), why, sizeof(why));
                if (txt) {
                    char conv[200];
                    const char *bad = CheckCodes(txt, conv, sizeof(conv));
                    size_t sl = (size_t)(tab2 - (l + 17));
                    int sigOk = sl == 1 && l[17] == '-' ? conv[0] == 0 : (strlen(conv) == sl && memcmp(conv, l + 17, sl) == 0);
                    if (bad)
                        snprintf(why, sizeof(why), "%s", bad);
                    else if (!sigOk)
                        snprintf(why, sizeof(why), "the replacement's conversions are \"%s\", and the original's (SIG) are not the same", conv);
                    else if (strlen(txt) >= TEXT_LIMIT)
                        snprintf(why, sizeof(why), "the text is %u bytes; the game's text buffer holds %d", (unsigned)strlen(txt), TEXT_LIMIT - 1);
                    else if (Insert(key, txt) != 0)
                        snprintf(why, sizeof(why), "out of memory");
                    else
                        why[0] = 0;
                    if (why[0])
                        free(txt);
                }
            }
        }
        if (why[0]) {
            snprintf(g_why, sizeof(g_why), "text.tsv, line %u: %s", line, why);
            rc = -1;
        }
    }
    free(data);
    if (rc != 0)
        Host_TextClose();
    return rc;
}

/* ---- the lookup ---- */


HOST const char *Host_TextLookup(const char *s)
{
    if (!g_count
    )
        return s;
    if (!s || !*s)
        return s;
    uint64_t key = KeyOf(s);
    Entry *e = Find(key);
    return e ? e->text : s;
}
