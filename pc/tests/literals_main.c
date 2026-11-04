/* The game's string literals stay apart (tools/pc/literals.py), as MWCC and the PS2's linker keep
 * them:
 * - an equal text in two units is two objects: the linker merges neither whole strings nor tails;
 * - within one unit an equal text is one object (MWCC pools a file's literals, and clang does);
 * - between `#pragma dont_reuse_strings on` and its reset, a literal is an object of its own.
 * literals_a.c and literals_b.c are compiled as the game's files; main is cdecl for the C runtime. */
#include <stdio.h>

/* (string.h's functions would take the game's calling convention here: the C runtime's are cdecl) */
static int Cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a != *b;
}

static unsigned Len(const char *a)
{
    unsigned n = 0;
    while (a[n])
        n++;
    return n;
}

const char *Lit_A1(void);
const char *Lit_A2(void);
const char *Lit_A3(void);
const char *Lit_ATail(void);
const char *Lit_ANoReuse(void);
const char *Lit_B1(void);
const char *Lit_BTail(void);
const char *Lit_AEmpty(void);
const char *Lit_BEmpty(void);

static int s_bad;

static void Expect(const char *what, int ok)
{
    if (!ok) {
        printf("literals: FAILED: %s\n", what);
        s_bad++;
    }
}

__attribute__((cdecl)) int main(void)
{
    const char *a1 = Lit_A1(), *a2 = Lit_A2(), *a3 = Lit_A3(), *nr = Lit_ANoReuse(), *b1 = Lit_B1();
    const char *at = Lit_ATail(), *bt = Lit_BTail();
    Expect("the texts", !Cmp(a1, "the same text") && !Cmp(b1, a1) && !Cmp(nr, a1) && !Cmp(bt, "text"));
    Expect("one unit's equal texts are one literal", a1 == a2 && a2 == a3);
    Expect("two units' equal texts are two literals", a1 != b1);
    Expect("two units' empty texts are two literals", Lit_AEmpty() != Lit_BEmpty() && !*Lit_AEmpty() && !*Lit_BEmpty());
    Expect("a literal under dont_reuse_strings is its own", nr != a1 && nr != b1);
    Expect("no tail merging across units", !(bt > at && bt < at + Len(at)) && !(bt > a1 && bt < a1 + Len(a1)));
    if (s_bad)
        return 1;
    printf("literals: OK (%p %p %p)\n", (const void *)a1, (const void *)b1, (const void *)nr);
    return 0;
}
