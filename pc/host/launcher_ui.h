/*
 * The launcher's UI base (the launcher design, 2 and 9): a window with SDL's renderer, a font through stb_truetype, the
 * rows of the game's Option screens (a label, a value between two arrows, the selected row larger), a help area, and the
 * input of a keyboard, a pad and a mouse as a few events. The picture is 640 x 448 in logical coordinates, letterboxed
 * into the window, which is drawn in physical pixels so that the text is sharp at any size.
 */
#ifndef MH2_LAUNCHER_UI_H
#define MH2_LAUNCHER_UI_H

#include <stdint.h>

#define UI_W 640
#define UI_H 448
#define UI_LOG_LINES 17   /* the lines of a scrolling text that fit above the help */

/* A row: a label at the left and, for a setting, its value between two arrows. */
enum {
    UR_ARROWS = 1,   /* the value has arrows (it can be changed) */
    UR_DIM = 2,      /* unavailable: half grey */
    UR_GREEN = 4,    /* the Defaults row */
    UR_GAP = 8,      /* a half row of space before it */
    UR_NOLEFT = 16,  /* the left arrow is not drawn (the first choice) */
    UR_NORIGHT = 32, /* the right arrow is not drawn (the last choice) */
    UR_WARN = 64,    /* a warning shade (the banner of a missing archive) */
    UR_EDIT = 128,   /* the value is being typed: drawn from the left, with a caret, and no arrows */
};
typedef struct UiRow {
    const char *label;
    const char *value;   /* NULL: none */
    unsigned flags;
} UiRow;

typedef enum {
    UE_NONE,
    UE_UP, UE_DOWN, UE_LEFT, UE_RIGHT, UE_PGUP, UE_PGDN,
    UE_CONFIRM, UE_CANCEL,
    UE_QUIT,    /* the window was closed */
    UE_HOVER,   /* the mouse is over row ROW */
    UE_CLICK,   /* row ROW was clicked */
    UE_ARROW,   /* an arrow of row ROW was clicked: DIR -1 or +1 */
    UE_WHEEL,   /* the wheel over row ROW: DIR */
    UE_TEXT,       /* typed letters (TEXT), while ui_text_input is on */
    UE_BACKSPACE,  /* the key that takes the last letter back */
} UiEventType;
typedef struct UiEvent {
    int type, row, dir;
    char text[40];
} UiEvent;

/* Opens the window (960 x 672, resizable, at least 640 x 448), its renderer and the font. 0, or -1 (SDL's message in
 * ui_error). A software renderer is used when MH2_LAUNCHER_SOFTWARE is set in the environment, or when there is no other. */
int ui_open(int fullscreen);
const char *ui_error(void);
/* Destroys the window and the renderer: the game's window is made after. */
void ui_close(void);
/* The next event (waiting up to WAIT_MS), as the table says (the launcher design, 9); 1 if there is one. */
int ui_poll(UiEvent *e, int wait_ms);
int ui_fullscreen(void);
const char *ui_renderer(void);   /* the name of SDL's renderer in use (software, direct3d11, opengl...) */
/* Typing: on while a row is being edited (letters come as UE_TEXT; the keys the player set for confirm and cancel are letters
 * too, so they stop meaning those; Return and Escape still do). */
void ui_text_input(int on);
/* The window (for the system's file dialogs). */
void *ui_window(void);

/* One picture: begin, the page, end (which shows it). */
void ui_begin(void);
void ui_header(const char *title);
/* The Home page: the title, and after it in smaller type what the project runs. */
void ui_header_sub(const char *title, const char *sub);
/* The rows from y = 70, 30 apart; SEL is drawn larger, in azure. Returns the first row to draw at (always 0). */
void ui_rows(const UiRow *rows, int n, int sel);
void ui_rows_top(float y);   /* the next ui_rows starts at Y instead of 70 (a page with text above its rows) */
void ui_help(const char *text);   /* the help area: two lines at the bottom */
/* Free text at x = 60, centred on Y: SHADE 0 plain, 1 dim, 2 warning. */
void ui_line(float y, int shade, const char *text);
/* A paragraph at x = 50 from Y: wrapped to the help's width, at most 4 lines. */
void ui_para(float y, const char *text);
/* A progress bar across the page at Y (FRAC 0 to 1), LABEL at its right. */
void ui_bar(float y, float frac, const char *label);
/* A scrolling text: N lines of LINES from FIRST, as many as fit; returns how many fit. */
int ui_lines(const char *const *lines, int n, int first);
/* A box over the page: TEXT and a row of BUTTONS, SEL highlighted. */
void ui_message(const char *text, const char *const *buttons, int nb, int sel);
void ui_end(void);
/* The last picture's pixels as an FNV-1a hash (and a PNG when PATH is given): the headless test's. 0, or -1. */
int ui_readback(uint64_t *hash, const char *png_path);
/* A logical point as the window's coordinates, for synthesized mouse events (the test). */
void ui_window_point(float x, float y, float *wx, float *wy);
int ui_rows_visible(void);   /* how many rows the last page drew */
void ui_set_size(int w, int h);   /* the window's size (the test) */
void ui_bounce(int dir);   /* an arrow jumps: -1 left, +1 right */

#endif
