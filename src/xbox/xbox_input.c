/* xbox_input.c -- the controllers as the NES's two pads.
 *
 * The first controller plugged in (by port) is player 1, the next player 2;
 * Contra merges both pads in a one-player game, so either can play it.
 * Each controller:
 *
 *   D-pad / left stick   the pad's cross: move, aim
 *   A                    A: jump
 *   B, X                 B: fire
 *   Y                    B, pressed for you fifteen times a second: rapid fire
 *   Back                 SELECT: 1 or 2 players at the title
 *   Start                START: start, pause
 *
 * The port's own, from any controller: White saves the game's state while
 * it is paused, Black loads it (while paused, or at the title); left-stick
 * click smooths the picture, right-stick click shows every sprite (no
 * flicker); Back + Start opens the front menu, and adding both triggers
 * exits to the dashboard. */
#include "xbox_xtl.h"

#include <string.h>

#include "xbox_port.h"

#define PORTS 4
#define PRESSED 30              /* an analog button counts past this */
#define STICK 13000             /* a thumbstick direction counts past this */
#define RAPID_PERIOD 4          /* frames: two pressed, two let go */

static HANDLE sPad[PORTS];
static WORD sPrevButtons;
static int sPrevWhite, sPrevBlack;
static unsigned sFrame, sPrevNet;
static int sPrevMenu;

void xb_input_init(void)
{
    XDEVICE_PREALLOC_TYPE prealloc[] = {
        { XDEVICE_TYPE_GAMEPAD, PORTS },
    };

    XInitDevices(sizeof(prealloc) / sizeof(prealloc[0]), prealloc);
}

static void refresh(void)
{
    /* Request a fresh USB report every millisecond. */
    XINPUT_POLLING_PARAMETERS polling = { 0 };
    DWORD ins = 0, rem = 0;
    int i;

    polling.fAutoPoll = TRUE;
    polling.bInputInterval = 1;
    polling.bOutputInterval = 8;
    if (!XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem))
        return;
    for (i = 0; i < PORTS; i++)
    {
        if ((rem & (1u << i)) && sPad[i] != NULL)
        {
            XInputClose(sPad[i]);
            sPad[i] = NULL;
        }
        if ((ins & (1u << i)) && sPad[i] == NULL) {
            sPad[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, (DWORD)i, XDEVICE_NO_SLOT, &polling);
            if (sPad[i] == NULL)
                sPad[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, (DWORD)i, XDEVICE_NO_SLOT, NULL);
        }
    }
}

/* one controller as an NES pad */
static uint8_t as_pad(const XINPUT_GAMEPAD *g, int rapid_on)
{
    uint8_t p = 0;
    int up = g->sThumbLY > STICK || (g->wButtons & XINPUT_GAMEPAD_DPAD_UP);
    int down = g->sThumbLY < -STICK || (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    int left = g->sThumbLX < -STICK || (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    int right = g->sThumbLX > STICK || (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);

    /* a pad's cross cannot press both ways at once, and the game is not
     * written for it */
    if (up && down)
        up = down = 0;
    if (left && right)
        left = right = 0;
    if (up)
        p |= PAD_UP;
    if (down)
        p |= PAD_DOWN;
    if (left)
        p |= PAD_LEFT;
    if (right)
        p |= PAD_RIGHT;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_A] > PRESSED)
        p |= PAD_A;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_B] > PRESSED || g->bAnalogButtons[XINPUT_GAMEPAD_X] > PRESSED)
        p |= PAD_B;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_Y] > PRESSED && rapid_on)
        p |= PAD_B;
    if (g->wButtons & XINPUT_GAMEPAD_BACK)
        p |= PAD_SELECT;
    if (g->wButtons & XINPUT_GAMEPAD_START)
        p |= PAD_START;
    return p;
}

unsigned xb_input_poll(uint8_t pads[2])
{
    WORD buttons = 0;
    int white = 0, black = 0, lt = 0, rt = 0, player = 0, i;
    int rapid_on = (sFrame++ % RAPID_PERIOD) < RAPID_PERIOD / 2;
    unsigned cmd = 0, net = 0;
    int menu = 0;

    refresh();
    pads[0] = pads[1] = 0;
    for (i = 0; i < PORTS; i++)
    {
        XINPUT_STATE s;
        const XINPUT_GAMEPAD *g;

        if (sPad[i] == NULL || XInputGetState(sPad[i], &s) != ERROR_SUCCESS)
            continue;
        g = &s.Gamepad;
        if (player < 2)
            pads[player++] = as_pad(g, rapid_on);
        if ((g->wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ==
            (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) menu = 1;
        /* Keep the old leave chord as a shortcut. Hosting/joining is now
         * selected in the front menu, so these cannot start a session. */
        if ((g->wButtons & XINPUT_GAMEPAD_BACK) &&
            g->bAnalogButtons[XINPUT_GAMEPAD_WHITE] > PRESSED &&
            g->bAnalogButtons[XINPUT_GAMEPAD_BLACK] > PRESSED)
            net |= XB_CMD_NET_EXIT;
        buttons |= g->wButtons;
        white |= g->bAnalogButtons[XINPUT_GAMEPAD_WHITE] > PRESSED;
        black |= g->bAnalogButtons[XINPUT_GAMEPAD_BLACK] > PRESSED;
        lt |= g->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] > PRESSED;
        rt |= g->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] > PRESSED;
    }

    if (white && !sPrevWhite)
        cmd |= XB_CMD_SAVE;
    if (black && !sPrevBlack)
        cmd |= XB_CMD_LOAD;
    sPrevWhite = white;
    sPrevBlack = black;
    if ((buttons & XINPUT_GAMEPAD_LEFT_THUMB) && !(sPrevButtons & XINPUT_GAMEPAD_LEFT_THUMB))
        cmd |= XB_CMD_SMOOTH;
    if ((buttons & XINPUT_GAMEPAD_RIGHT_THUMB) && !(sPrevButtons & XINPUT_GAMEPAD_RIGHT_THUMB))
        cmd |= XB_CMD_SPRITES;
    if ((buttons & XINPUT_GAMEPAD_BACK) && (buttons & XINPUT_GAMEPAD_START) && lt && rt)
        cmd |= XB_CMD_QUIT;
    if (net) {
        /* Netplay chords must not also SELECT, save or load the game. */
        pads[0] = pads[1] = 0;
        cmd &= ~(XB_CMD_SAVE | XB_CMD_LOAD);
        if (net & XB_CMD_NET_EXIT) net = XB_CMD_NET_EXIT;
        cmd |= net & ~sPrevNet;
    }
    if (menu && !sPrevMenu) cmd |= XB_CMD_MENU;
    if (menu) pads[0] = pads[1] = 0;
    sPrevMenu = menu;
    sPrevNet = net;
    sPrevButtons = buttons;
    return cmd;
}
