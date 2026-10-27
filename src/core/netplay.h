/* Deterministic input timeline, bounded prediction and automatic delay. */
#ifndef CNX_NETPLAY_CORE_H
#define CNX_NETPLAY_CORE_H
#include "nes.h"
#define NP_HISTORY 256
#define NP_MAX_DELAY 12
#define NP_MAX_ROLLBACK 3
#define NP_NONE 0xffffffffu
#define NP_AUDIO_MAX 960
#define NP_SNAPSHOTS 4

typedef struct NpInput { uint32_t frame; uint8_t pad, valid; } NpInput;
typedef struct NpHash { uint32_t frame, value; int valid; } NpHash;
typedef struct NpSnapshot {
    Nes nes;
    uint32_t frame;
    int valid, audio_n;
    int16_t audio[NP_AUDIO_MAX];
} NpSnapshot;
typedef struct NpSession {
    NpInput inputs[2][NP_HISTORY], used[NP_HISTORY];
    NpHash hashes[NP_HISTORY], peer_hashes[NP_HISTORY];
    NpSnapshot snapshots[NP_SNAPSHOTS];
    uint32_t frame, confirmed, emitted, latest, rollback, sampled;
    uint32_t hash_frame, hash, predictions, corrections, replays, max_depth;
    uint8_t local_slot, delay, window, last_local;
    int fault;
} NpSession;
typedef struct NpDelay {
    uint16_t rtt[9];
    unsigned count, pos, pending, votes, delay, median, jitter, window, initialized;
} NpDelay;
void np_init(NpSession *n, unsigned slot, unsigned delay, unsigned window);
/* Authoritative values are immutable. Old/out-of-window packets are ignored. */
int np_receive(NpSession *n, unsigned slot, uint32_t frame, uint8_t pad);
void np_receive_hash(NpSession *n, uint32_t frame, uint32_t hash);
void np_set_delay(NpSession *n, unsigned delay);
int np_sample(NpSession *n, uint8_t pad);
/* Apply late inputs before presenting/audio. The callback gets confirmed audio only. */
void np_reconcile(NpSession *n, Nes *nes, void (*audio)(const int16_t *, int));
int np_step(NpSession *n, Nes *nes);
uint32_t np_state_hash(const Nes *nes);
void np_delay_init(NpDelay *f, unsigned window);
unsigned np_delay_sample(NpDelay *f, unsigned rtt_ms);
#endif
