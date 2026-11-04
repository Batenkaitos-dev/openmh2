#ifndef CONV_H
#define CONV_H

#include "common.h"

/*
 * The kana-kanji converter's shared records: its dictionary matches, the
 * nodes of a converted text, and the per-position records.
 */

/* A dictionary word: its reading and its conversion (Dict_AddWord, Dict_DeleteWord,
 * UserDict_AddWord; Conv_NewWord fills gNewWord). */
typedef struct Word {
    /* 0x0 */ u8 *reading;          /* 1-byte codes (gNewWordReading) */
    /* 0x4 */ s16 len;              /* the reading's length */
    /* 0x6 */ u8 unk6;
    /* 0x7 */ u8 kind;              /* the word class (UserDict_WordClass; 25 from Conv_NewWord) */
    /* 0x8 */ u8 unk8;
    /* 0x9 */ u8 unk9[3];
    /* 0xC */ u8 *text;             /* Shift-JIS */
} Word;
typedef char Word_size_check[(sizeof(Word) == 0x10) ? 1 : -1];

/* A dictionary match for an input position (24 bytes, from the converter's node pool); a record
 * keeps a list of them. */
typedef struct ConvSeg {
    /* 0x00 */ s16 len;
    /* 0x02 */ u8 kind;
    /* 0x03 */ u8 sub;
    /* 0x04 */ s32 unk4;
    /* 0x08 */ u64 entry;           /* dictionary entry, -1 for none */
    /* 0x10 */ u16 unk10;           /* the sense code (MatchItem +4), Cand_Prio code */
    /* 0x14 */ struct ConvSeg *next;
} ConvSeg;
typedef char ConvSeg_size_check[(sizeof(ConvSeg) == 0x18) ? 1 : -1];

/* A conversion candidate (16 bytes); a record keeps them sorted by len. */
typedef struct ConvCand {
    /* 0x0 */ s16 len;
    /* 0x2 */ u8 kind;              /* 40 for plain text, 255 for none */
    /* 0x3 */ u8 sub;
    /* 0x4 */ ConvSeg *seg;
    /* 0x8 */ u16 prio;             /* also the best cost (Cand_BestCost) */
    /* 0xA */ u16 extra;            /* extra cost (Cand_SetNextCosts, ConvRec_InitExtraCosts) */
    /* 0xC */ struct ConvCand *next;
} ConvCand;
typedef char ConvCand_size_check[(sizeof(ConvCand) == 0x10) ? 1 : -1];

/* A node of a converted text (20 bytes in a 24-byte node of the converter's pool). A text is a chain: the
 * first node holds 4 bytes of it, the next ones (flags 2) 14 bytes each, all but the last flagged
 * 1. A raw node (0x80) stands for input left as it is. */
typedef struct ConvOut {
    /* 0x00 */ u8 flags;            /* 1 = continued in next, 0x80 = raw */
    /* 0x01 */ u8 text[5];
    /* 0x06 */ u8 len;              /* input characters converted */
    /* 0x07 */ u8 kind;
    /* 0x08 */ ConvSeg *seg;
    /* 0x0C */ u16 unkC;
    /* 0x0E */ u16 unkE;            /* Cand_Prio code */
    /* 0x10 */ struct ConvOut *next;
} ConvOut;
typedef char ConvOut_size_check[(sizeof(ConvOut) == 0x14) ? 1 : -1];

/* One of the 80 per-position records at gConvRecs (28 bytes). */
typedef struct ConvRec {
    /* 0x00 */ u8 *tableEntry;      /* built-in table entry (ConvTable_Lookup), -1 before the lookup */
    /* 0x04 */ ConvSeg *segs;       /* -1 when there is none */
    /* 0x08 */ ConvCand *cands;
    /* 0x0C */ ConvOut *outs;       /* the texts of the segment starting here */
    /* 0x10 */ s32 unk10;
    /* 0x14 */ s8 fixed;
    /* 0x15 */ s8 len;              /* length of the segment starting here */
    /* 0x16 */ s8 fixedLen;          /* len when first fixed (ConvRec_NoteFixedLen); a changed one is learned */
    /* 0x17 */ s8 unk17;
    /* 0x18 */ s8 tableLen;         /* length matched in the built-in table */
    /* 0x19 */ s8 unk19;
    /* 0x1A */ u8 pad1A[2];
} ConvRec;
typedef char ConvRec_size_check[(sizeof(ConvRec) == 0x1C) ? 1 : -1];

#endif /* CONV_H */
