#ifndef MH2_MOVIELIB_H
#define MH2_MOVIELIB_H

/*
 * The movie library (mh2movie, pc/movie), loaded at run time from the folder mh2 is in. movie.c plays the movies with
 * it; movietable.c (the archive maker's movie table) measures them with it, driven as the decoder thread drives it.
 */
#include <stddef.h>

#include <SDL3/SDL.h>

#include "../../pc/movie/mh2movie.h"

typedef struct MovieLib {
    SDL_SharedObject *so;
    mh2movie_open_fn open;
    mh2movie_next_frame_fn next_frame;
    mh2movie_audio_fn audio;
    mh2movie_close_fn close;
} MovieLib;

enum { MOVIELIB_OK = 0, MOVIELIB_NOLOAD = -1, MOVIELIB_OTHER = -2 };

/* Loads the library from FOLDER (NULL: the folder mh2 is in). PATH gets the file's path (CAP bytes); on MOVIELIB_NOLOAD ERR
 * holds the system's reason. MOVIELIB_OTHER: it loads, but is another ABI version (it is unloaded). */
int movielib_load(MovieLib *lib, const char *folder, char *path, size_t pcap, char *err, size_t ecap);

#endif
