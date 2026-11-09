/* The float model in force (ps2fp.h): 0 legacy, 1 ps2. Set once before the game's first instruction
 * (pc/core/pal_fpmodel.c); the tests of the crt set it themselves. */
int ps2fp_model;
