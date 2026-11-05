#ifndef MH2_MOVIETABLE_H
#define MH2_MOVIETABLE_H

/*
 * The movies' timing table (mh2-movies.txt, beside the disc archive), written as tools/pc/movie_table.py writes it, line for
 * line (that tool stays the build's, and the oracle). The core keeps each movie's timing without a decoder (pc/core/ipc/
 * ipc_movie.c): the original game reports a movie finished at the first VBlank end where its clock has passed both the last
 * frame's time and the sound's length, and both depend only on the stream. This measures them with the movie library
 * (movielib.h), driven as the decoder thread drives it: a frame, then the sound decoded meanwhile.
 *
 * Per movie (an entry of AFS02.AFS) a line "I OFFSET SIZE FRAMES SHOWN SOUND PTS": the frames the library decodes (-1: it
 * cannot open the movie, which the original reports finished at once); the smallest clock (48 kHz frames) at which every frame
 * is shown; the sound's length in 48 kHz frames (-1: no sound); the largest presentation time of frames 1 to FRAMES-1.
 *
 * Without the library (or with another version of it) there is no table: the archive is whole, and the launcher says the
 * movies are not timed, with the library's own words.
 */
#include <stddef.h>
#include <stdint.h>

enum { MT_OK = 0, MT_CANCELLED, MT_E_ARCHIVE, MT_E_AFS, MT_E_NOLIB, MT_E_WRITE };

/* Called before each movie (IN_MOVIE 0: movie I of N is next, counted from 1) and after each of its frames (IN_MOVIE 1).
 * Nonzero stops. */
typedef int (*MovieProgress)(void *ud, int i, int n, int in_movie);
/* Called for what went wrong in one movie (it is timed as unopenable, as in the original). */
typedef void (*MovieNote)(void *ud, int i, const char *text);

/* The table for ARCHIVE (the file the maker wrote, or any of this version), into the file "mh2-movies.txt" beside it. LIBDIR:
 * the folder with the movie library (NULL: the folder mh2 is in). TEXT gets the reason for a failure. */
int movietable_make(const char *archive, const char *libdir, MovieProgress progress, MovieNote note, void *ud, char *text, size_t cap);

/* Python's repr of a double: the shortest digits that read back as the same number (exposed for the test). */
void movietable_repr(double x, char *out, size_t cap);

/* The smallest clock C >= 0 with (double)C / 48000.0 >= PTS, as the original compares (exposed for the test). */
long long movietable_shown(double pts);

#endif
