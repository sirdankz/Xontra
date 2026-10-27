/* osd.h -- the port's own messages over the picture ("STATE SAVED"): a
 * line of capitals and digits in a small font, on a dark band, for a few
 * seconds. Drawn into the frame after the NES has made it. */
#ifndef CNX_OSD_H
#define CNX_OSD_H

#include <stdint.h>

typedef struct Osd
{
    char text[40];
    int frames;                 /* left to show */
} Osd;

/* Draw text at a chosen position, with clipped pixels (menu and errors). */
void osd_text(uint32_t *fb, int w, int h, int x, int y,
              const char *text, uint32_t colour, int scale);

/* show `text` (A-Z, 0-9, space and . - : ! /) for `frames` frames */
void osd_show(Osd *o, const char *text, int frames);
/* draw it into `fb` (w x h, 0xAARRGGBB), near the bottom, and count down */
void osd_draw(Osd *o, uint32_t *fb, int w, int h);

#endif /* CNX_OSD_H */
