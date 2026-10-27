#ifndef CNX_XBOX_NETPLAY_H
#define CNX_XBOX_NETPLAY_H
#include "xbox_port.h"
/* Pregame host preferences: -1 Auto or 0..12 delay; 0..3 rollback.
 * Locked while connected. Guests adopt the host's settings. */
void xb_net_configure(int delay, unsigned window);
int xb_net_delay_setting(void);
unsigned xb_net_rollback_setting(void);
void xb_net_begin(int host, int online, uint32_t rom_crc, const char *passcode);
void xb_net_stop(void);
void xb_net_poll(void);
void xb_net_ui(uint8_t pad);
int xb_net_browsing(void);
int xb_net_back(void);
int xb_net_editing_code(void);
const char *xb_net_join_code(void);
unsigned xb_net_code_digit(void);
const char *xb_net_code_error(void);
int xb_net_room_locked(unsigned index);
int xb_net_can_start(void);
unsigned xb_net_room_count(void);
unsigned xb_net_room_selected(void);
uint32_t xb_net_room_id(unsigned index);
int xb_net_busy(void);
int xb_net_running(void);
int xb_net_take_reset(void);
const char *xb_net_status(void);
void xb_net_reconcile(Nes *nes);
int xb_net_advance(Nes *nes, uint8_t local);
#endif
