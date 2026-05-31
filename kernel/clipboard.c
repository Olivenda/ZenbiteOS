/* System clipboard: shared single-buffer between apps (Editor, Notes,
 * Web URL bar, Shell). Plain bytes, no MIME type, no history. */

#include "../include/kernel.h"

#define CLIP_MAX 2048
static char clip_buf[CLIP_MAX];
static int  clip_len;

void clipboard_set(const char *buf, int n) {
    if (n < 0) n = 0;
    if (n > CLIP_MAX) n = CLIP_MAX;
    for (int i = 0; i < n; i++) clip_buf[i] = buf[i];
    clip_len = n;
}

int clipboard_get(char *out, int max) {
    int n = clip_len < max ? clip_len : max;
    for (int i = 0; i < n; i++) out[i] = clip_buf[i];
    return n;
}

int clipboard_len(void) { return clip_len; }
