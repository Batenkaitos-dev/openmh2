/* The game's string literals (tools/pc/literals.py): one unit of two, compiled as the game's files are
 * (ps2fp/ps2fpcc.py, the game's flags). Read by literals_main.c. */
const char *Lit_A1(void)
{
    return "the same text";
}

const char *Lit_A2(void)
{
    return "the same text";
}

const char *Lit_ATail(void)
{
    return "a longer text";
}

#pragma dont_reuse_strings on
const char *Lit_ANoReuse(void)
{
    return "the same text";
}
#pragma dont_reuse_strings reset

const char *Lit_A3(void)
{
    return "the same text";
}

const char *Lit_AEmpty(void)
{
    return "";
}
