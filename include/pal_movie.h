#ifndef PAL_MOVIE_H
#define PAL_MOVIE_H

#include "platform.h"

/*
 * The port's movie player (PLATFORM_PC only; pc/core/ipc/ipc_movie.c), in place
 * of CRI's Sofdec player under the game's four wrappers (MovieSfd_Start,
 * MovieSfd_RequestStop, MovieSfd_Stop, MovieSfd_GetStatus in
 * src/sub_main/movie.c), as the original's HLE replaces them
 *
 */
#if PLATFORM_PC

/* Plays entry `entry` of AFS02.AFS in display mode `mode` (MovieDesc +0xE)
 * at `volume` (0.1 dB, at least -960). */
void Pal_MoviePlay(int entry, int mode, int volume);
/* Stops the movie (the stop request and the stop alike). */
void Pal_MovieStop(void);
/* MOVIE_STATUS_STOPPED, PREPARING, PLAYING or FINISHED. */
int Pal_MovieStatus(void);

#endif /* PLATFORM_PC */

#endif /* PAL_MOVIE_H */
