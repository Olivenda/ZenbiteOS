#ifndef ZENBITE_TUI_H
#define ZENBITE_TUI_H

#include "types.h"

/* TUI primitives for full-screen wizards. CP437 box-drawing characters. */

#define TUI_BG       1   /* VGA_BLUE              */
#define TUI_FG       15  /* VGA_WHITE             */
#define TUI_TITLE_FG 14  /* VGA_YELLOW            */
#define TUI_TITLE_BG 4   /* VGA_RED               */
#define TUI_STATUS_FG 0  /* VGA_BLACK             */
#define TUI_STATUS_BG 7  /* VGA_LIGHT_GREY        */
#define TUI_BOX_FG   15
#define TUI_HIGH_FG  0
#define TUI_HIGH_BG  7

void tui_init(void);                 /* clear screen, blue bg, hide cursor */
void tui_end(void);                  /* restore normal text-mode shell    */

void tui_clear(void);
void tui_box(int row, int col, int w, int h);             /* single line */
void tui_dbl_box(int row, int col, int w, int h);         /* double line */
void tui_title(const char *text);
void tui_status(const char *text);
void tui_print(int row, int col, const char *text);
void tui_print_color(int row, int col, const char *text, u8 fg, u8 bg);
void tui_highlight(int row, int col, int w);              /* invert a span */

/* Reads a line into buf, returns when ENTER pressed. */
int  tui_input(int row, int col, int w, char *buf, int max);

/* Confirm box: shows message + (Yes/No) selector. Returns 1/0. */
int  tui_confirm(const char *msg);

/* Centered modal info box: shows msg, waits for any key. */
void tui_alert(const char *msg);

#endif
