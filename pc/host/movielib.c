#include "movielib.h"

#ifdef _WIN32
#define MH2MOVIE_LIB "mh2movie.dll"
#else
#define MH2MOVIE_LIB "mh2movie.so"
#endif

int movielib_load(MovieLib *lib, const char *folder, char *path, size_t pcap, char *err, size_t ecap)
{
    memset(lib, 0, sizeof(*lib));
    if (err && ecap)
        err[0] = 0;
    const char *base = folder ? folder : SDL_GetBasePath();
    size_t n = base ? strlen(base) : 0;
    int slash = n && (base[n - 1] == '/' || base[n - 1] == '\\');
    SDL_snprintf(path, pcap, "%s%s%s", base ? base : "", slash || !n ? "" : "/", MH2MOVIE_LIB);
    lib->so = SDL_LoadObject(path);
    if (!lib->so) {
        if (err)
            SDL_snprintf(err, ecap, "%s", SDL_GetError());
        return MOVIELIB_NOLOAD;
    }
    mh2movie_abi_fn abi = (mh2movie_abi_fn)SDL_LoadFunction(lib->so, "mh2movie_abi");
    lib->open = (mh2movie_open_fn)SDL_LoadFunction(lib->so, "mh2movie_open");
    lib->next_frame = (mh2movie_next_frame_fn)SDL_LoadFunction(lib->so, "mh2movie_next_frame");
    lib->audio = (mh2movie_audio_fn)SDL_LoadFunction(lib->so, "mh2movie_audio");
    lib->close = (mh2movie_close_fn)SDL_LoadFunction(lib->so, "mh2movie_close");
    if (!abi || abi() != MH2MOVIE_ABI_VERSION || !lib->open || !lib->next_frame || !lib->audio || !lib->close) {
        SDL_UnloadObject(lib->so);
        memset(lib, 0, sizeof(*lib));
        return MOVIELIB_OTHER;
    }
    return MOVIELIB_OK;
}
