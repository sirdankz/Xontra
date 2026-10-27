/* xbox_audio.c -- the APU's sound through a DirectSound stream.
 *
 * The game thread pushes each frame's samples (nes_frame's, 48 kHz mono)
 * into a ring; a thread of its own hands them to DirectSound a packet at a
 * time, as mono explicitly routed to both front speakers. The NES's 60.10 frames a second against the television's
 * 59.94 (or six frames in five at 50 Hz) leave the ring slowly filling or
 * draining; a packet then takes one sample more or less than it plays,
 * which nobody hears, and the ring stays near its mark. */
#include "xbox_xtl.h"
#include <dsound.h>

#include <string.h>

#include "xbox_port.h"

#define RING 16384              /* samples; a power of two */
#define PACKET 512              /* samples a packet plays: 10.7 ms */
#define PACKETS 3
#define MARK (PACKET * 2)       /* where the ring's fill is kept */

static int16_t sRing[RING];
static volatile LONG sWrite, sRead;     /* counts; the index is & (RING - 1) */
static int16_t sLast;
static volatile LONG sPaused = 1;

void xb_audio_pause(int paused)
{
    InterlockedExchange(&sPaused, paused != 0);
}

static LPDIRECTSOUND sDS;
static LPDIRECTSOUNDSTREAM sStream;
static int16_t *sPcm[PACKETS];
static DWORD sStatus[PACKETS];
static HANDLE sDone[PACKETS];

void xb_audio_push(const int16_t *s, int n)
{
    LONG w = sWrite;
    int i;

    if (sStream == NULL || sPaused)
        return;
    if (w - sRead + n > RING)
        return;                 /* the consumer has stopped: drop rather than overrun */
    for (i = 0; i < n; i++)
        sRing[(w + i) & (RING - 1)] = s[i];
    __asm__ volatile("" ::: "memory");     /* the samples land before the count */
    sWrite = w + n;
}

/* Mono source, sent equally to left and right by the hardware mixer. */
static void fill_packet(int k)
{
    int16_t *out = sPcm[k];
    LONG r = sRead, have = sWrite - r;
    int take = PACKET, i;

    if (sPaused) {
        memset(out, 0, PACKET * sizeof(*out));
        sRead = sWrite;
        sLast = 0;
        return;
    }
    if (have > MARK + PACKET)
        take = PACKET + 1;
    else if (have < MARK - PACKET)
        take = PACKET - 1;
    if (have < take)
    {
        /* starved (the game is starting, or held up): what there is, then
         * the last sample held, not a click to zero */
        for (i = 0; i < PACKET; i++)
        {
            if (i < have)
                sLast = sRing[(r + i) & (RING - 1)];
            out[i] = sLast;
        }
        sRead = r + have;
        return;
    }
    for (i = 0; i < PACKET; i++)
    {
        int16_t v = sRing[(r + i * take / PACKET) & (RING - 1)];

        out[i] = v;
    }
    sLast = sRing[(r + take - 1) & (RING - 1)];
    __asm__ volatile("" ::: "memory");     /* the samples are read before the count frees them */
    sRead = r + take;
}

static void submit(int k)
{
    XMEDIAPACKET xp;

    memset(&xp, 0, sizeof(xp));
    xp.pvBuffer = sPcm[k];
    xp.dwMaxSize = PACKET * sizeof(int16_t);
    xp.pdwStatus = &sStatus[k];
    xp.hCompletionEvent = sDone[k];
    sStatus[k] = XMEDIAPACKET_STATUS_PENDING;
    IDirectSoundStream_Process(sStream, &xp, NULL);
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    int k = 0, j, pending;

    (void)arg;
    for (;;)
    {
        /* packets complete in order: wait for the oldest, refill it */
        WaitForSingleObject(sDone[k], 20);
        DirectSoundDoWork();
        if (sStatus[k] == XMEDIAPACKET_STATUS_PENDING)
            continue;
        for (j = 0, pending = 0; j < PACKETS; j++)
            pending += sStatus[j] == XMEDIAPACKET_STATUS_PENDING;
        if (pending == 0)
        {
            /* ran dry: start the stream over, the whole queue at once */
            IDirectSoundStream_Flush(sStream);
            for (j = 0; j < PACKETS; j++)
            {
                fill_packet(j);
                submit(j);
            }
            k = 0;
            continue;
        }
        fill_packet(k);
        submit(k);
        k = (k + 1) % PACKETS;
    }
    return 0;
}

int xb_audio_init(void)
{
    static const DSMIXBINVOLUMEPAIR speakers[] = {
        { DSMIXBIN_FRONT_LEFT, 0 }, { DSMIXBIN_FRONT_RIGHT, 0 }
    };
    static const DSMIXBINS mix = { 2, speakers };
    WAVEFORMATEX wfx;
    DSSTREAMDESC desc;
    HANDLE th;
    int k;

    if (FAILED(DirectSoundCreate(NULL, &sDS, NULL)))
        return -1;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = XB_AUDIO_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = sizeof(int16_t);
    wfx.nAvgBytesPerSec = XB_AUDIO_RATE * wfx.nBlockAlign;
    memset(&desc, 0, sizeof(desc));
    desc.dwMaxAttachedPackets = PACKETS;
    desc.lpwfxFormat = &wfx;
    desc.lpMixBins = &mix;
    if (FAILED(DirectSoundCreateStream(&desc, &sStream)))
    {
        sStream = NULL;
        return -1;
    }
    for (k = 0; k < PACKETS; k++)
    {
        sPcm[k] = (int16_t *)XPhysicalAlloc(PACKET * sizeof(int16_t), MAXULONG_PTR, 0, PAGE_READWRITE);
        sDone[k] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (sPcm[k] == NULL || sDone[k] == NULL)
        {
            sStream = NULL;
            return -1;
        }
        memset(sPcm[k], 0, PACKET * sizeof(int16_t));
    }
    for (k = 0; k < PACKETS; k++)
    {
        fill_packet(k);
        submit(k);
    }
    th = CreateThread(NULL, 64 * 1024, audio_thread, NULL, 0, NULL);
    if (th == NULL)
    {
        sStream = NULL;
        return -1;
    }
    SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(th);
    xb_log("audio: %d Hz mono to LEFT + RIGHT, %d ms of stream", XB_AUDIO_RATE, PACKETS * PACKET * 1000 / XB_AUDIO_RATE);
    return 0;
}
