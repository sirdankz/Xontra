/* xbox_port.h -- the Xbox side of Contra NES-X: what xbox_main.c drives.
 *
 *   xbox_video.c   Direct3D: the NES picture, scaled up whole, in its border
 *   xbox_audio.c   DirectSound: the APU's sound, streamed
 *   xbox_input.c   the controllers, as the NES's two pads
 *   xbox_save.c    the save state, as a save game on the hard disk */
#ifndef CNX_XBOX_PORT_H
#define CNX_XBOX_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "../core/nes.h"

#define XB_AUDIO_RATE 48000

void xb_log(const char *fmt, ...);

/* video: 0 when up; the refresh rate it runs at (50 or 60) */
int xb_video_init(int *refresh_hz);
/* the NES's 256 x 240 frame, `top`..`top + lines` of it shown, around it `border` */
void xb_video_present(const uint32_t *fb, int top, int lines, uint32_t border);
void xb_video_toggle_smooth(void);

int xb_audio_init(void);
/* mono samples at XB_AUDIO_RATE, in time order */
void xb_audio_push(const int16_t *s, int n);
void xb_audio_pause(int paused);

void xb_input_init(void);
/* the two NES pads this frame (PAD_*), and the port's own commands */
#define XB_CMD_QUIT       0x01
#define XB_CMD_SMOOTH     0x02
#define XB_CMD_SPRITES    0x04
#define XB_CMD_SAVE       0x08
#define XB_CMD_LOAD       0x10
#define XB_CMD_NET_HOST   0x20
#define XB_CMD_NET_JOIN   0x40
#define XB_CMD_NET_EXIT   0x80
#define XB_CMD_NET_ONLINE 0x100
#define XB_CMD_MENU       0x200
unsigned xb_input_poll(uint8_t pads[2]);

/* the save state: loaded on demand (NULL when there is none), stored on
 * demand (nonzero when it is on the disk) */
uint8_t *xb_save_load(size_t *len);
int xb_save_store(const uint8_t *data, size_t len);

#endif /* CNX_XBOX_PORT_H */
