/* A development log of the loads, for measuring them: one line per VBlank in which the
 * disc was read or the game's load state changed, to the host's log. MH2_LOADLOG=1 turns it on. It reads the
 * game's memory and changes nothing the game can see. */
typedef int IoLoadLogEmpty;
