#ifndef CNX_XBOX_MENU_H
#define CNX_XBOX_MENU_H
#include <stdint.h>
typedef struct XbMenu {
    int page, selected, protected_room;
    unsigned digit;
    uint8_t previous;
    char passcode[5];
} XbMenu;
enum { XB_MENU_NONE, XB_MENU_LOCAL, XB_MENU_ONLINE_HOST, XB_MENU_ONLINE_JOIN,
       XB_MENU_LAN_HOST, XB_MENU_LAN_JOIN, XB_MENU_QUIT };
void xb_menu_init(XbMenu *menu, uint8_t held);
int xb_menu_poll(XbMenu *menu, uint8_t pad);
const char *xb_menu_passcode(const XbMenu *menu);
void xb_menu_draw(const XbMenu *menu, uint32_t *fb);
void xb_menu_lobby(uint32_t *fb, int online, int host);
void xb_menu_error(uint32_t *fb, int problem);
#endif
