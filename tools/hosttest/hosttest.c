/* hosttest.c -- the port's NES (src/core) on the PC, without the Xbox:
 * runs the game for a number of frames with scripted input and writes
 * screenshots (PNG) and the sound (WAV). No Xbox, and no emulator, is
 * needed to see that the game runs.
 *
 *     hosttest Contra.nes [options]
 *       --frames N           frames to run (default 600)
 *       --shot F[,F...]      a screenshot at each of these frames
 *       --every N            and one every N frames
 *       --press F1-F2:BTNS   player 1's buttons held over frames F1..F2
 *                            (repeatable); BTNS joined by '+': A B SELECT
 *                            START UP DOWN LEFT RIGHT
 *       --press2 F1-F2:BTNS  the same for player 2
 *       --out DIR            where the PNGs go (default .)
 *       --wav FILE           the sound, 48 kHz mono
 *       --unlimited          no eight-sprites-a-line limit
 *       --osd F:TEXT         the port's message TEXT from frame F
 *       --poke F:ADDR=VAL    write VAL to RAM at ADDR (hex) at frame F
 *                            (repeatable)
 *       --peek ADDR,N        print N bytes of RAM from ADDR (hex) at the end
 *       --state-save F:FILE  a save state at (the end of) frame F
 *       --state-load F:FILE  load one at (the start of) frame F
 *
 * Each screenshot is the whole 256 x 240 picture, two PNG pixels to one.
 * It also prints what the core reports (a jammed CPU, ...). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/nes.h"
#include "../../src/core/osd.h"
int netplay_selftest(const uint8_t *rom, size_t length);

#define RATE 48000
#define MAX_SAMPLES (RATE / 50)

typedef struct
{
    int from, to, pad;
    uint8_t buttons;
} Press;

typedef struct
{
    int frame;
    unsigned addr, val;
} Poke;

typedef struct
{
    int frame;
    const char *path;
} StateOp;

static Press sPress[256];
static int sPressCount;
static Poke sPoke[64];
static int sPokeCount;
static StateOp sSaves[16], sLoads[16];
static int sSaveCount, sLoadCount;

static void log_line(const char *msg)
{
    printf("%s\n", msg);
}

/* ------------------------------------------------------------------ PNG */

static uint32_t sCrc[256];

static uint32_t crc32_(const uint8_t *p, size_t n, uint32_t c)
{
    size_t i;

    if (!sCrc[1])
    {
        uint32_t k, j, v;

        for (k = 0; k < 256; k++)
        {
            for (v = k, j = 0; j < 8; j++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            sCrc[k] = v;
        }
    }
    c = ~c;
    for (i = 0; i < n; i++)
        c = sCrc[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8], crc[4];
    uint32_t c;

    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    c = crc32_(hdr + 4, 4, 0);
    c = crc32_(data, len, c);
    be32(crc, c);
    fwrite(crc, 1, 4, f);
}

/* RGB, 2x (each NES pixel four PNG pixels: easier to look at), the zlib
 * stream in stored blocks */
static int write_png(const char *path, const uint32_t *fb, int w, int h)
{
    int W = w * 2, H = h * 2, x, y;
    size_t raw_len = (size_t)H * (1 + (size_t)W * 3), zlen, pos, i;
    uint8_t *raw = (uint8_t *)malloc(raw_len), *z, ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;

    if (!raw)
        return -1;
    for (y = 0; y < H; y++)
    {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)W * 3);

        row[0] = 0;
        for (x = 0; x < W; x++)
        {
            uint32_t p = fb[(y / 2) * w + x / 2];

            row[1 + x * 3] = (uint8_t)(p >> 16);
            row[2 + x * 3] = (uint8_t)(p >> 8);
            row[3 + x * 3] = (uint8_t)p;
        }
    }
    zlen = 2 + raw_len + (raw_len / 65535 + 1) * 5 + 4;
    z = (uint8_t *)malloc(zlen);
    if (!z)
    {
        free(raw);
        return -1;
    }
    pos = 0;
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (i = 0; i < raw_len; i += 65535)
    {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;

        z[pos++] = (uint8_t)(i + n >= raw_len ? 1 : 0);
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + i, n);
        pos += n;
    }
    for (i = 0; i < raw_len; i++)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be32(z + pos, (b << 16) | a);
    pos += 4;

    f = fopen(path, "wb");
    if (!f)
    {
        free(raw);
        free(z);
        return -1;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)pos);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

/* ------------------------------------------------------------------ WAV */

static void write_wav(const char *path, const int16_t *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)(n * 2), v;
    uint16_t h;

    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    h = 1; fwrite(&h, 2, 1, f);
    h = 1; fwrite(&h, 2, 1, f);
    v = RATE; fwrite(&v, 4, 1, f);
    v = RATE * 2; fwrite(&v, 4, 1, f);
    h = 2; fwrite(&h, 2, 1, f);
    h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(s, 2, n, f);
    fclose(f);
}

/* ---------------------------------------------------------------- input */

static int button_by_name(const char *k)
{
    static const struct { const char *name; int bit; } kButtons[] = {
        { "A", PAD_A }, { "B", PAD_B }, { "SELECT", PAD_SELECT }, { "START", PAD_START },
        { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
    };
    size_t i;

    for (i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
        if (strcmp(k, kButtons[i].name) == 0)
            return kButtons[i].bit;
    return 0;
}

static int add_press(const char *spec, int pad)
{
    Press *p;
    char buf[256], *tok, *colon;

    if (sPressCount >= 256)
        return -1;
    p = &sPress[sPressCount];
    memset(p, 0, sizeof(*p));
    p->pad = pad;
    if (sscanf(spec, "%d-%d", &p->from, &p->to) != 2)
    {
        if (sscanf(spec, "%d", &p->from) != 1)
            return -1;
        p->to = p->from;
    }
    colon = strchr(spec, ':');
    if (!colon)
        return -1;
    snprintf(buf, sizeof(buf), "%s", colon + 1);
    for (tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+"))
    {
        int b = button_by_name(tok);

        if (!b)
        {
            fprintf(stderr, "hosttest: unknown button %s\n", tok);
            return -1;
        }
        p->buttons |= (uint8_t)b;
    }
    sPressCount++;
    return 0;
}

static int add_state_op(StateOp *ops, int *count, const char *spec)
{
    const char *colon = strchr(spec, ':');

    if (*count >= 16 || !colon || sscanf(spec, "%d", &ops[*count].frame) != 1)
        return -1;
    ops[*count].path = colon + 1;
    (*count)++;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (d && n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n)
    {
        free(d);
        d = NULL;
    }
    fclose(f);
    *len = (size_t)(n > 0 ? n : 0);
    return d;
}

int main(int argc, char **argv)
{
    static Nes n;
    static uint32_t fb[NES_W * NES_H];
    static int16_t audio[MAX_SAMPLES];
    static Osd osd;
    int frames = 600, every = 0, unlimited = 0, i, f, osd_frame = -1, net_test = 0;
    const char *out = ".", *wav = NULL, *osd_text = NULL;
    int shots[256], nshots = 0;
    unsigned peek_addr = 0, peek_len = 0;
    int16_t *all = NULL;
    size_t all_len = 0, rom_len;
    uint8_t *rom;

    if (argc < 2)
    {
        fprintf(stderr, "usage: hosttest Contra.nes [--frames N] [--shot F,...] [--every N] "
                        "[--press F1-F2:BTNS] [--press2 F1-F2:BTNS] [--out DIR] [--wav FILE] "
                        "[--state-save F:FILE] [--state-load F:FILE]\n");
        return 2;
    }
    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc)
            every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
        {
            char *s = argv[++i], *t;

            for (t = strtok(s, ","); t && nshots < 256; t = strtok(NULL, ","))
                shots[nshots++] = atoi(t);
        }
        else if ((strcmp(argv[i], "--press") == 0 || strcmp(argv[i], "--press2") == 0) && i + 1 < argc)
        {
            int pad = argv[i][7] == '2';

            if (add_press(argv[++i], pad) != 0)
            {
                fprintf(stderr, "hosttest: bad --press %s\n", argv[i]);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--poke") == 0 && i + 1 < argc)
        {
            Poke *p = &sPoke[sPokeCount];

            if (sPokeCount >= 64 || sscanf(argv[++i], "%d:%x=%x", &p->frame, &p->addr, &p->val) != 3)
            {
                fprintf(stderr, "hosttest: bad --poke %s\n", argv[i]);
                return 2;
            }
            sPokeCount++;
        }
        else if (strcmp(argv[i], "--state-save") == 0 && i + 1 < argc)
        {
            if (add_state_op(sSaves, &sSaveCount, argv[++i]) != 0)
                return 2;
        }
        else if (strcmp(argv[i], "--state-load") == 0 && i + 1 < argc)
        {
            if (add_state_op(sLoads, &sLoadCount, argv[++i]) != 0)
                return 2;
        }
        else if (strcmp(argv[i], "--osd") == 0 && i + 1 < argc)
        {
            const char *colon = strchr(argv[++i], ':');

            if (!colon || sscanf(argv[i], "%d", &osd_frame) != 1)
                return 2;
            osd_text = colon + 1;
        }
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
            wav = argv[++i];
        else if (strcmp(argv[i], "--peek") == 0 && i + 1 < argc)
        {
            if (sscanf(argv[++i], "%x,%u", &peek_addr, &peek_len) != 2)
            {
                fprintf(stderr, "hosttest: bad --peek %s\n", argv[i]);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--netplay-test") == 0)
            net_test = 1;
        else if (strcmp(argv[i], "--unlimited") == 0)
            unlimited = 1;
        else
        {
            fprintf(stderr, "hosttest: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    rom = read_file(argv[1], &rom_len);
    if (!rom)
    {
        perror(argv[1]);
        return 2;
    }
    if (net_test) {
        int result = netplay_selftest(rom, rom_len);
        free(rom);
        return result;
    }
    n.log = log_line;
    if (nes_init(&n, rom, rom_len, fb, RATE) != 0)
    {
        fprintf(stderr, "hosttest: %s is not a cartridge the core takes\n", argv[1]);
        return 2;
    }
    n.ppu.unlimited_sprites = unlimited;
    if (wav)
        all = (int16_t *)malloc((size_t)frames * MAX_SAMPLES * sizeof(int16_t));

    for (f = 0; f < frames; f++)
    {
        int k, ns;

        for (k = 0; k < sLoadCount; k++)
            if (sLoads[k].frame == f)
            {
                size_t len;
                uint8_t *st = read_file(sLoads[k].path, &len);

                if (!st || nes_state_load(&n, st, len) != 0)
                    printf("hosttest: frame %d: %s is not a state of this cartridge\n", f, sLoads[k].path);
                else
                    printf("hosttest: frame %d: state loaded from %s\n", f, sLoads[k].path);
                free(st);
            }
        n.pads[0] = n.pads[1] = 0;
        for (k = 0; k < sPressCount; k++)
            if (f >= sPress[k].from && f <= sPress[k].to)
                n.pads[sPress[k].pad] |= sPress[k].buttons;
        for (k = 0; k < sPokeCount; k++)
            if (sPoke[k].frame == f)
                n.ram[sPoke[k].addr & 0x7FF] = (uint8_t)sPoke[k].val;

        ns = nes_frame(&n, audio, MAX_SAMPLES);
        if (all)
        {
            memcpy(all + all_len, audio, (size_t)ns * sizeof(int16_t));
            all_len += (size_t)ns;
        }
        if (osd_text && f == osd_frame)
            osd_show(&osd, osd_text, 120);
        osd_draw(&osd, fb, NES_W, NES_H);

        for (k = 0; k < sSaveCount; k++)
            if (sSaves[k].frame == f)
            {
                size_t len = nes_state_size();
                uint8_t *st = (uint8_t *)malloc(len);
                FILE *sf = fopen(sSaves[k].path, "wb");

                nes_state_save(&n, st);
                if (sf)
                {
                    fwrite(st, 1, len, sf);
                    fclose(sf);
                }
                printf("hosttest: frame %d: state saved to %s (%u bytes)\n", f, sSaves[k].path, (unsigned)len);
                free(st);
            }

        {
            int want = every > 0 && f % every == 0;

            for (k = 0; k < nshots && !want; k++)
                want = shots[k] == f;
            if (want)
            {
                char path[512];

                snprintf(path, sizeof(path), "%s/shot_%05d.png", out, f);
                if (write_png(path, fb, NES_W, NES_H) != 0)
                    fprintf(stderr, "hosttest: cannot write %s\n", path);
            }
        }
    }

    printf("hosttest: %d frames; bank %u; game routine %02X, demo %u, paused %u, level %u, lives %u/%u\n",
           frames, n.bank, n.ram[CONTRA_GAME_ROUTINE], n.ram[CONTRA_DEMO_MODE], n.ram[CONTRA_PAUSE],
           n.ram[CONTRA_LEVEL], n.ram[CONTRA_P1_LIVES], n.ram[CONTRA_P2_LIVES]);
    if (peek_len)
    {
        unsigned a;

        printf("hosttest: RAM %04X:", peek_addr);
        for (a = 0; a < peek_len; a++)
            printf(" %02X", n.ram[(peek_addr + a) & 0x7FF]);
        printf("\n");
    }
    if (all)
    {
        write_wav(wav, all, all_len);
        free(all);
    }
    free(rom);
    return n.cpu.jammed ? 1 : 0;
}
