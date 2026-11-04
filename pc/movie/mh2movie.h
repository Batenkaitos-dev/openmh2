/* mh2movie: plays the game's movies (CRI Sofdec files: MPEG program
 * streams with MPEG-2 video and ADX audio) with FFmpeg. A shared library
 * that the port loads at run time (pc/host/movie.c), so the executable
 * runs without FFmpeg (movies are then skipped).
 *
 * The bridge is CC0; built, it links FFmpeg's LGPL libraries dynamically.
 * Not thread-safe per movie: one thread drives a given movie. */
#ifndef MH2MOVIE_H
#define MH2MOVIE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MH2MOVIE_ABI_VERSION 1

#ifdef _WIN32
#define MH2MOVIE_API __declspec(dllexport)
#else
#define MH2MOVIE_API __attribute__((visibility("default")))
#endif

typedef struct mh2movie mh2movie;

typedef struct mh2movie_info {
    int width, height;          /* video size */
    double fps;
    double duration;            /* seconds, 0 if unknown */
    int has_audio;
} mh2movie_info;

typedef int (*mh2movie_abi_fn)(void);
/* Open `size` bytes at `offset` of the file at `path`. Returns NULL on
 * error (message in err). Audio comes out as 48 kHz interleaved stereo s16. */
typedef mh2movie *(*mh2movie_open_fn)(const char *path, uint64_t offset, uint64_t size, mh2movie_info *info,
                                      char *err, int errlen);
/* Decode until the next video frame: RGBA8, width*height*4 bytes into rgba,
 * presentation time in *pts (seconds). Audio decoded meanwhile is queued.
 * Returns 1 for a frame, 0 at the end of the movie, <0 on error. */
typedef int (*mh2movie_next_frame_fn)(mh2movie *m, uint8_t *rgba, double *pts);
/* Take up to max_frames frames of queued audio (48 kHz stereo s16). */
typedef int (*mh2movie_audio_fn)(mh2movie *m, int16_t *out, int max_frames);
typedef void (*mh2movie_close_fn)(mh2movie *m);

MH2MOVIE_API int mh2movie_abi(void);
MH2MOVIE_API mh2movie *mh2movie_open(const char *path, uint64_t offset, uint64_t size, mh2movie_info *info,
                                     char *err, int errlen);
MH2MOVIE_API int mh2movie_next_frame(mh2movie *m, uint8_t *rgba, double *pts);
MH2MOVIE_API int mh2movie_audio(mh2movie *m, int16_t *out, int max_frames);
MH2MOVIE_API void mh2movie_close(mh2movie *m);

#ifdef __cplusplus
}
#endif

#endif
