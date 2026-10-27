/* xbox_main.c -- the Xbox entry point: Contra (NES, 1988) on the port's
 * NES (src/core), one NES frame per television frame.
 *
 * The cartridge is D:\data\Contra.nes, which setup.py assembles from the
 * disassembly (with your baserom.nes's graphics and music) and puts on the
 * disc. */
#include "xbox_xtl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/osd.h"
#include "xbox_port.h"
#include "xbox_netplay.h"
#include "xbox_menu.h"

/* the lines a television shows: the NES's top and bottom eight are under
 * the bezel on an NTSC set, and Contra keeps nothing there */
#define SHOW_TOP 8
#define SHOW_LINES 224
#define MESSAGE_FRAMES 120

static Nes sNes;
static uint32_t sFb[NES_W * NES_H];
static uint32_t sDisplay[NES_W * NES_H];
static int16_t sAudio[XB_AUDIO_RATE / 50];
static Osd sOsd;

void xb_log(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n] = '\n';
    line[n + 1] = 0;
    OutputDebugStringA(line);
}

static void core_log(const char *msg)
{
    xb_log("%s", msg);
}

/* A fault's address, to the debug channel, before the console restarts:
 * look it up in out/<Config>/default.exe's symbols. */
static LONG WINAPI fault_report(struct _EXCEPTION_POINTERS *ep)
{
    xb_log("cnx: FAULT %08lx at %08lx", (unsigned long)ep->ExceptionRecord->ExceptionCode,
           (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    return 0;                   /* EXCEPTION_CONTINUE_SEARCH */
}

/* Both disc installs and HDD folders are supported. A missing cartridge
 * stays on a readable retry screen rather than relaunching the dashboard. */
static int sRomProblem = 1; /* 1 missing, 2 unsupported, 3 read failure */
static uint8_t *read_whole(const char *path, size_t *len, int *found)
{
    HANDLE h;
    DWORD size, got = 0, error;
    uint8_t *data;
    *len = 0; *found = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            *found = 1;
            xb_log("cnx: open %s failed: %lu", path, (unsigned long)error);
        }
        return NULL;
    }
    *found = 1;
    size = GetFileSize(h, NULL);
    /* Contra is 128 KiB plus header. Bound allocation for a damaged file. */
    if (!size || size == INVALID_FILE_SIZE || size > 1024u * 1024u) {
        xb_log("cnx: %s has an unusable size: %lu", path, (unsigned long)size);
        CloseHandle(h); return NULL;
    }
    data = (uint8_t *)malloc(size);
    if (!data) {
        xb_log("cnx: no memory reading %s", path);
        CloseHandle(h); return NULL;
    }
    if (!ReadFile(h, data, size, &got, NULL) || got != size) {
        error = GetLastError();
        xb_log("cnx: read %s failed: %lu, %lu/%lu bytes", path,
               (unsigned long)error, (unsigned long)got, (unsigned long)size);
        free(data); data = NULL;
    }
    CloseHandle(h);
    *len = data ? size : 0;
    return data;
}
static uint8_t *load_cartridge(size_t *len)
{
    static const char *const paths[] = {
        "D:\\data\\Contra.nes", "D:\\Contra.nes", "D:\\baserom.nes",
        "D:\\Contra (USA).nes", "D:\\Contra (U).nes",
        "data\\Contra.nes", "Contra.nes", "baserom.nes",
        "Contra (USA).nes", "Contra (U).nes"
    };
    unsigned i;
    sRomProblem = 1;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int found;
        uint8_t *rom = read_whole(paths[i], len, &found);
        if (!rom) { if (found) sRomProblem = 3; continue; }
        sNes.log = core_log;
        if (!nes_init(&sNes, rom, *len, sFb, XB_AUDIO_RATE)) {
            xb_log("cnx: loaded %s; PRG CRC32 %08lx", paths[i], (unsigned long)sNes.prg_crc);
            return rom;
        }
        xb_log("cnx: unsupported cartridge at %s", paths[i]);
        sRomProblem = 2;
        free(rom);
    }
    *len = 0;
    return NULL;
}
static void cold_boot(const uint8_t *rom, size_t len)
{
    /* rom was validated by load_cartridge and is immutable for this run. */
    nes_init(&sNes, rom, len, sFb, XB_AUDIO_RATE);
    memset(sFb, 0, sizeof(sFb));
    memset(&sOsd, 0, sizeof(sOsd));
}

/* a game is on (not the title, the intro or the demo) */
static int in_game(void)
{
    return sNes.ram[CONTRA_GAME_ROUTINE] >= 3 && !sNes.ram[CONTRA_DEMO_MODE];
}

static void save_state(void)
{
    size_t len = nes_state_size();
    uint8_t *buf;

    if (!in_game() || !sNes.ram[CONTRA_PAUSE])
    {
        osd_show(&sOsd, "PAUSE TO SAVE", MESSAGE_FRAMES);
        return;
    }
    buf = (uint8_t *)malloc(len);
    if (!buf)
        return;
    nes_state_save(&sNes, buf);
    osd_show(&sOsd, xb_save_store(buf, len) ? "STATE SAVED" : "SAVE FAILED", MESSAGE_FRAMES);
    free(buf);
}

static void load_state(void)
{
    size_t len;
    uint8_t *buf;

    if (in_game() && !sNes.ram[CONTRA_PAUSE])
    {
        osd_show(&sOsd, "PAUSE TO LOAD", MESSAGE_FRAMES);
        return;
    }
    buf = xb_save_load(&len);
    if (!buf)
    {
        osd_show(&sOsd, "NO SAVED STATE", MESSAGE_FRAMES);
        return;
    }
    if (nes_state_load(&sNes, buf, len) == 0)
        osd_show(&sOsd, "STATE LOADED - START TO PLAY", MESSAGE_FRAMES);
    else
        osd_show(&sOsd, "SAVED STATE NOT USABLE", MESSAGE_FRAMES);
    free(buf);
}

void __cdecl main(void)
{
    uint8_t *rom;
    size_t rom_len = 0;
    int hz = 60, acc = 0, in_menu = 1, lobby = 0, audio_attempted = 0;
    int lobby_online = 0, lobby_host = 0;
    uint8_t previous = 0;
    XbMenu menu;

    SetUnhandledExceptionFilter(fault_report);
    xb_input_init();
    if (xb_video_init(&hz) != 0) {
        xb_log("cnx: no Direct3D device");
        XLaunchNewImage(NULL, NULL);
        return;
    }
    xb_menu_init(&menu, 0);
    rom = load_cartridge(&rom_len);

    for (;;) {
        uint8_t local[2], pressed;
        unsigned cmd;
        /* Correct late remote frames before sampling fresh local input. */
        if (!in_menu) { xb_net_poll(); xb_net_reconcile(&sNes); }
        cmd = xb_input_poll(local);
        pressed = (uint8_t)(local[0] & ~previous);
        previous = local[0];
        if (cmd & XB_CMD_QUIT) {
            xb_net_stop();
            XLaunchNewImage(NULL, NULL);
        }
        if (!rom) {
            if (pressed & (PAD_B | PAD_SELECT)) XLaunchNewImage(NULL, NULL);
            if (pressed & (PAD_A | PAD_START)) {
                rom = load_cartridge(&rom_len);
                xb_menu_init(&menu, local[0]);
            }
            if (!rom) {
                xb_menu_error(sDisplay, sRomProblem);
                xb_video_present(sDisplay, SHOW_TOP, SHOW_LINES, 0xff0b1628u);
                continue;
            }
        }
        if (!audio_attempted) {
            audio_attempted = 1;
            if (xb_audio_init() != 0) xb_log("cnx: no sound");
            xb_audio_pause(1);
        }
        if (in_menu) {
            if (lobby) {
                if ((cmd & (XB_CMD_MENU | XB_CMD_NET_EXIT)) ||
                    ((pressed & (PAD_B | PAD_SELECT)) && !xb_net_back())) {
                    xb_net_stop(); lobby = 0; menu.previous = local[0];
                } else {
                    xb_net_ui(local[0]);
                    xb_net_poll();
                    if (xb_net_running() && xb_net_take_reset()) {
                        cold_boot(rom, rom_len);
                        xb_audio_pause(0);
                        in_menu = lobby = 0; acc = 0;
                        continue;
                    }
                    xb_menu_lobby(sDisplay, lobby_online, lobby_host);
                    xb_video_present(sDisplay, SHOW_TOP, SHOW_LINES, 0xff0b1628u);
                    continue;
                }
            } else {
                int action = xb_menu_poll(&menu, local[0]);
                if (action == XB_MENU_QUIT) XLaunchNewImage(NULL, NULL);
                if (action == XB_MENU_LOCAL) {
                    cold_boot(rom, rom_len);
                    xb_audio_pause(0); in_menu = 0; acc = 0;
                    continue;
                }
                if (action >= XB_MENU_ONLINE_HOST && action <= XB_MENU_LAN_JOIN) {
                    lobby_online = action == XB_MENU_ONLINE_HOST || action == XB_MENU_ONLINE_JOIN;
                    lobby_host = action == XB_MENU_ONLINE_HOST || action == XB_MENU_LAN_HOST;
                    xb_net_begin(lobby_host, lobby_online, sNes.prg_crc, xb_menu_passcode(&menu));
                    lobby = 1; acc = 0;
                    xb_menu_lobby(sDisplay, lobby_online, lobby_host);
                    xb_video_present(sDisplay, SHOW_TOP, SHOW_LINES, 0xff0b1628u);
                    continue;
                }
            }
            xb_menu_draw(&menu, sDisplay);
            xb_video_present(sDisplay, SHOW_TOP, SHOW_LINES, 0xff0b1628u);
            continue;
        }
        if (cmd & (XB_CMD_MENU | XB_CMD_NET_EXIT)) {
            xb_net_stop(); xb_audio_pause(1);
            in_menu = 1; lobby = 0; acc = 0;
            xb_menu_init(&menu, local[0]);
            continue;
        }
        if (xb_net_busy() && !xb_net_running()) {
            xb_audio_pause(1); in_menu = lobby = 1; acc = 0;
            continue;
        }
        if (cmd & XB_CMD_SMOOTH) xb_video_toggle_smooth();
        if ((cmd & XB_CMD_SPRITES) && !xb_net_busy()) {
            sNes.ppu.unlimited_sprites ^= 1;
            osd_show(&sOsd, sNes.ppu.unlimited_sprites ? "ALL SPRITES - NO FLICKER" : "SPRITE LIMIT AS THE NES", MESSAGE_FRAMES);
        }
        if ((cmd & XB_CMD_SAVE) && !xb_net_busy()) save_state();
        if ((cmd & XB_CMD_LOAD) && !xb_net_busy()) load_state();
        for (acc += 60; acc >= hz; acc -= hz) {
            if (xb_net_busy()) {
                if (!xb_net_advance(&sNes, local[0])) { acc = 0; break; }
            } else {
                int n;
                sNes.pads[0] = local[0]; sNes.pads[1] = local[1];
                n = nes_frame(&sNes, sAudio, (int)(sizeof(sAudio) / sizeof(sAudio[0])));
                xb_audio_push(sAudio, n);
            }
        }
        memcpy(sDisplay, sFb, sizeof(sDisplay));
        /* Netplay presents only the game's own picture; all port text stays in menus. */
        if (xb_net_busy()) sOsd.frames = 0;
        else osd_draw(&sOsd, sDisplay, NES_W, NES_H);
        xb_video_present(sDisplay, SHOW_TOP, SHOW_LINES, ppu_backdrop(&sNes.ppu));
    }
}
