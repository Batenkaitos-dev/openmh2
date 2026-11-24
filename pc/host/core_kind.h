#ifndef MH2_CORE_KIND_H
#define MH2_CORE_KIND_H

/*
 * The content core (the one that plays backported content; pc/core/content) holds a mark in its own file. The renderer's memory depends
 * on the kind: 8 MB for the content core, whose textures reach blocks above 16383, and the GS's 4 MB for the base core. The renderer is
 * made before the core runs, so the host reads the mark from the file, whichever way the core was named (the launcher's Play, a --core
 * option, a development option). The core tells the host its kind again as it attaches (core_kind in the shared header), and the host
 * checks that the two agree.
 *
 * core_is_content: 1 when the file at PATH holds the mark, 0 when it does not or cannot be read. About 25 MB are read in 1 MB chunks.
 */
int core_is_content(const char *path);

#endif
