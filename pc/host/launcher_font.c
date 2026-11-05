/*
 * The launcher's font, Noto Sans Regular (SIL Open Font License 1.1: fonts/OFL.txt, fonts/README.txt), embedded in mh2
 * with C23's #embed: this file is compiled as C23 (CMakeLists.txt).
 */
#include "launcher_font.h"

const unsigned char g_font_noto[] = {
#embed "fonts/NotoSans-Regular.ttf"
};
const unsigned long g_font_noto_size = sizeof(g_font_noto);
