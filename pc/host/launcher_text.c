/* The launcher's strings (launcher_text.h). */
#include "launcher_text.h"

const char *const g_text[T_COUNT] = {
#define X(id, s) [id] = s,
    LTEXTS(X)
#undef X
};
