/*
 * Included first into the files of the host seam and of the development layer (the build adds it with -include):
 * their writable data goes into the sections "hostnb" (zero) and "hostdat" (initialised), which a save state
 * does not hold (state.h). Their data is the process's, not the game's.
 */
#pragma clang section bss = "hostnb" data = "hostdat"
