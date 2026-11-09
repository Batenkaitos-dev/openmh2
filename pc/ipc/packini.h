/*
 * A text pack's pack.ini, read the same way by the core (which opens the pack) and by mh2 (which checks the chosen pack
 * before it starts the game).
 *
 * The file is UTF-8 (a leading byte order mark is skipped), one setting a line: `key = value`, with blanks around both
 * trimmed. A blank line, and a line whose first non-blank character is # or ;, is a comment. The keys, lower case:
 *   name      required: what the launcher shows (1 to 64 bytes)
 *   language  optional: a tag for the player, such as en or pt-BR (up to 16 bytes)
 *   version   optional: up to 32 bytes
 *   charmap   optional: the name of the pack's character map file in the pack's folder (default charmap.txt)
 * A key twice, a key not listed, a line with no `=`, an empty value, a control character, a byte sequence that is not UTF-8
 * and a file name with a separator in it are errors, each reported with its line.
 */
#ifndef MH2_PACKINI_H
#define MH2_PACKINI_H

#include <stdio.h>
#include <string.h>

#define PACKINI_NAME 65
#define PACKINI_LANG 17
#define PACKINI_VERSION 33
#define PACKINI_FILE 65

typedef struct PackIni {
    char name[PACKINI_NAME];
    char language[PACKINI_LANG];
    char version[PACKINI_VERSION];
    char charmap[PACKINI_FILE];   /* "charmap.txt" when the file does not name one */
    int hasCharmap;               /* the file named one */
} PackIni;

/* 1 when the n bytes are well-formed UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF). */
static inline int PackIni_Utf8(const unsigned char *s, unsigned long n)
{
    unsigned long i = 0;
    while (i < n) {
        unsigned c = s[i];
        unsigned long len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!len || i + len > n)
            return 0;
        for (unsigned long k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        if (c == 0xE0 && s[i + 1] < 0xA0)
            return 0;
        if (c == 0xED && s[i + 1] > 0x9F)
            return 0;
        if (c == 0xF0 && s[i + 1] < 0x90)
            return 0;
        if (c == 0xF4 && s[i + 1] > 0x8F)
            return 0;
        i += len;
    }
    return 1;
}

/* Reads the text (n bytes). 0 when it is well-formed; otherwise -1 and a sentence in why (cap bytes), which names the
 * line. out is filled as far as it was read. */
static inline int PackIni_Parse(const char *text, unsigned long n, PackIni *out, char *why, unsigned cap)
{
    unsigned long pos = 0;
    unsigned line = 0;
    int seen[4] = {0, 0, 0, 0};
    memset(out, 0, sizeof(*out));
    strcpy(out->charmap, "charmap.txt");
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        pos = 3;
    while (pos < n) {
        unsigned long e = pos, b, t;
        char key[16];
        unsigned long klen, vlen;
        const char *v;
        int which;
        line++;
        while (e < n && text[e] != '\n')
            e++;
        b = pos;
        t = e;
        pos = e < n ? e + 1 : e;
        if (t > b && text[t - 1] == '\r')
            t--;
        for (unsigned long k = b; k < t; k++)
            if (text[k] == 0) {
                snprintf(why, cap, "pack.ini, line %u: a NUL byte", line);
                return -1;
            }
        while (b < t && (text[b] == ' ' || text[b] == '\t'))
            b++;
        while (t > b && (text[t - 1] == ' ' || text[t - 1] == '\t'))
            t--;
        if (b == t || text[b] == '#' || text[b] == ';')
            continue;
        klen = 0;
        while (b + klen < t && text[b + klen] != '=')
            klen++;
        if (b + klen == t) {
            snprintf(why, cap, "pack.ini, line %u: no = (a setting is key = value)", line);
            return -1;
        }
        v = text + b + klen + 1;
        vlen = t - (b + klen + 1);
        while (vlen && (*v == ' ' || *v == '\t')) {
            v++;
            vlen--;
        }
        while (klen && (text[b + klen - 1] == ' ' || text[b + klen - 1] == '\t'))
            klen--;
        if (!klen || klen >= sizeof(key)) {
            snprintf(why, cap, "pack.ini, line %u: the key is empty or too long", line);
            return -1;
        }
        memcpy(key, text + b, klen);
        key[klen] = 0;
        which = !strcmp(key, "name") ? 0 : !strcmp(key, "language") ? 1 : !strcmp(key, "version") ? 2 : !strcmp(key, "charmap") ? 3 : -1;
        if (which < 0) {
            snprintf(why, cap, "pack.ini, line %u: %s is not a key of a pack.ini (name, language, version, charmap)", line, key);
            return -1;
        }
        if (seen[which]) {
            snprintf(why, cap, "pack.ini, line %u: %s is given twice", line, key);
            return -1;
        }
        seen[which] = 1;
        if (!vlen) {
            snprintf(why, cap, "pack.ini, line %u: %s has no value", line, key);
            return -1;
        }
        {
            unsigned long max = which == 0 ? PACKINI_NAME - 1 : which == 1 ? PACKINI_LANG - 1 : which == 2 ? PACKINI_VERSION - 1 : PACKINI_FILE - 1;
            char *dst = which == 0 ? out->name : which == 1 ? out->language : which == 2 ? out->version : out->charmap;
            if (vlen > max) {
                snprintf(why, cap, "pack.ini, line %u: %s is longer than %u bytes", line, key, (unsigned)max);
                return -1;
            }
            for (unsigned long k = 0; k < vlen; k++)
                if ((unsigned char)v[k] < 0x20 && v[k] != '\t') {
                    snprintf(why, cap, "pack.ini, line %u: %s holds a control character", line, key);
                    return -1;
                }
            if (!PackIni_Utf8((const unsigned char *)v, vlen)) {
                snprintf(why, cap, "pack.ini, line %u: %s is not UTF-8", line, key);
                return -1;
            }
            if (which == 3) {
                if (v[0] == '.' || memchr(v, '/', vlen) || memchr(v, '\\', vlen) || memchr(v, ':', vlen)) {
                    snprintf(why, cap, "pack.ini, line %u: charmap is a file name in the pack's folder (no folder, no leading dot)", line);
                    return -1;
                }
                out->hasCharmap = 1;
            }
            memcpy(dst, v, vlen);
            dst[vlen] = 0;
        }
    }
    if (!seen[0]) {
        snprintf(why, cap, "pack.ini has no name");
        return -1;
    }
    return 0;
}

#endif
