#include "netplay.h"
#include <string.h>

static NpInput *input(NpSession *n, unsigned p, uint32_t f)
{
    NpInput *v = &n->inputs[p][f & (NP_HISTORY - 1)];
    return v->valid && v->frame == f ? v : NULL;
}
static void check_hash(NpSession *n, uint32_t f)
{
    NpHash *a = &n->hashes[f & (NP_HISTORY - 1)], *b = &n->peer_hashes[f & (NP_HISTORY - 1)];
    /* f is the state AFTER f frames: never compare a speculative state. */
    if (f <= n->confirmed && f <= n->frame && n->rollback == NP_NONE &&
        a->valid && b->valid && a->frame == f && b->frame == f && a->value != b->value)
        n->fault = 2;
}
void np_init(NpSession *n, unsigned slot, unsigned delay, unsigned window)
{
    unsigned p, f;
    memset(n, 0, sizeof(*n));
    n->local_slot = (uint8_t)(slot & 1);
    n->window = (uint8_t)(window > NP_MAX_ROLLBACK ? NP_MAX_ROLLBACK : window);
    np_set_delay(n, delay);
    n->rollback = n->sampled = n->hash_frame = n->latest = NP_NONE;
    for (p = 0; p < 2; ++p)
        for (f = 0; f < n->delay; ++f) np_receive(n, p, f, 0);
    if (n->delay) n->latest = n->delay - 1;
}
void np_set_delay(NpSession *n, unsigned delay)
{
    n->delay = (uint8_t)(delay > NP_MAX_DELAY ? NP_MAX_DELAY : delay);
}
int np_receive(NpSession *n, unsigned slot, uint32_t frame, uint8_t pad)
{
    NpInput *v;
    if (slot > 1 || n->fault) return 0;
    if ((frame < n->frame && n->frame - frame >= NP_HISTORY) ||
        frame > n->frame + 2 * NP_MAX_DELAY + 2 * NP_MAX_ROLLBACK) return 0;
    v = &n->inputs[slot][frame & (NP_HISTORY - 1)];
    if (v->valid && v->frame == frame) {
        if (v->pad != pad) n->fault = 1;
        return !n->fault;
    }
    v->valid = 1; v->frame = frame; v->pad = pad;
    if (slot != n->local_slot && frame < n->frame) {
        NpInput *used = &n->used[frame & (NP_HISTORY - 1)];
        if (!used->valid || used->frame != frame) { n->fault = 3; return 0; }
        if (used->pad != pad) {
            if (n->frame - frame > n->window || frame < n->emitted) { n->fault = 3; return 0; }
            if (n->rollback == NP_NONE || frame < n->rollback) n->rollback = frame;
            ++n->corrections;
        }
    }
    while (input(n, 0, n->confirmed) && input(n, 1, n->confirmed)) ++n->confirmed;
    return 1;
}
void np_receive_hash(NpSession *n, uint32_t frame, uint32_t hash)
{
    NpHash *v;
    if (frame == NP_NONE || frame % 60 || frame > n->frame + 2 * NP_MAX_DELAY + 6 ||
        (frame < n->frame && n->frame - frame >= NP_HISTORY)) return;
    v = &n->peer_hashes[frame & (NP_HISTORY - 1)];
    if (v->valid && v->frame == frame && v->value != hash) { n->fault = 2; return; }
    v->valid = 1; v->frame = frame; v->value = hash;
    check_hash(n, frame);
}
int np_sample(NpSession *n, uint8_t pad)
{
    uint32_t target = n->frame + n->delay, next;
    int changed = 0;
    if (n->sampled == n->frame || n->fault) return 0;
    n->sampled = n->frame;
    next = n->latest == NP_NONE ? 0 : n->latest + 1;
    /* Increasing delay fills the new gap with hold-last. Decreasing it waits
     * for already-sent values to drain. Never relabel or overwrite an input. */
    while (next <= target) {
        uint8_t value = next == target ? pad : n->last_local;
        if (!np_receive(n, n->local_slot, next, value)) return changed;
        n->latest = next++; n->last_local = value; changed = 1;
    }
    return changed;
}
static uint8_t prediction(NpSession *n, uint32_t frame)
{
    unsigned remote = 1 - n->local_slot;
    uint32_t i;
    /* Includes already received intermediate values even if earlier input is
     * missing. Replaying recomputes this, propagating corrected held input. */
    for (i = 0; i < NP_HISTORY && i <= frame; ++i) {
        NpInput *v = input(n, remote, frame - i);
        if (v) return v->pad;
    }
    return 0;
}
uint32_t np_state_hash(const Nes *nes)
{
    Nes copy;
    const uint8_t *bytes = (const uint8_t *)&copy;
    size_t i;
    uint32_t hash = 2166136261u;
    memcpy(&copy, nes, sizeof(copy));
    copy.cpu.ctx = NULL; copy.cpu.read = NULL; copy.cpu.write = NULL;
    copy.ppu.fb = NULL; copy.apu.ctx = NULL; copy.apu.read = NULL; copy.apu.out = NULL;
    copy.prg = NULL; copy.log = NULL;
    /* Audio destination/capacity do not affect the emulated machine. */
    copy.apu.out_n = copy.apu.out_max = 0;
    for (i = 0; i < sizeof(copy); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
static void simulate(NpSession *n, Nes *nes)
{
    uint32_t f = n->frame;
    unsigned remote = 1 - n->local_slot;
    NpInput *local = input(n, n->local_slot, f), *other = input(n, remote, f);
    NpInput *used = &n->used[f & (NP_HISTORY - 1)];
    NpSnapshot *snap = &n->snapshots[f % NP_SNAPSHOTS];
    NpHash *h;
    /* One process, same Nes address, same immutable cartridge and framebuffer.
     * A byte-exact snapshot retains every emulated field and padding byte. */
    memcpy(&snap->nes, nes, sizeof(*nes)); snap->frame = f; snap->valid = 1;
    nes->pads[n->local_slot] = local->pad;
    nes->pads[remote] = other ? other->pad : prediction(n, f);
    used->valid = 1; used->frame = f; used->pad = nes->pads[remote];
    snap->audio_n = nes_frame(nes, snap->audio, NP_AUDIO_MAX);
    ++n->frame;
    h = &n->hashes[n->frame & (NP_HISTORY - 1)];
    if (n->frame % 60 == 0) {
        h->valid = 1; h->frame = n->frame; h->value = np_state_hash(nes);
    }
}
int np_step(NpSession *n, Nes *nes)
{
    NpInput *other;
    if (n->fault || n->rollback != NP_NONE || !input(n, n->local_slot, n->frame)) return 0;
    /* The limit is relative to the OLDEST unconfirmed frame, not consecutive
     * missing packets. Receiving later frames cannot reopen the window. */
    if (n->frame >= n->confirmed && n->frame - n->confirmed >= n->window &&
        !input(n, 1 - n->local_slot, n->frame)) return 0;
    if (n->frame > n->confirmed && n->frame - n->confirmed >= n->window) return 0;
    if (n->frame - n->emitted >= NP_SNAPSHOTS) return 0;
    other = input(n, 1 - n->local_slot, n->frame);
    if (!other) ++n->predictions;
    simulate(n, nes);
    return 1;
}
void np_reconcile(NpSession *n, Nes *nes, void (*audio)(const int16_t *, int))
{
    uint32_t end, f;
    if (n->fault) return;
    if (n->rollback != NP_NONE) {
        uint32_t start = n->rollback, depth = n->frame - start;
        NpSnapshot *snap = &n->snapshots[start % NP_SNAPSHOTS];
        if (!snap->valid || snap->frame != start || depth > n->window || start < n->emitted) {
            n->fault = 3; return;
        }
        end = n->frame;
        memcpy(nes, &snap->nes, sizeof(*nes));
        n->frame = start; n->rollback = NP_NONE;
        if (depth > n->max_depth) n->max_depth = depth;
        while (n->frame < end) { simulate(n, nes); ++n->replays; }
    }
    end = n->confirmed < n->frame ? n->confirmed : n->frame;
    while (n->emitted < end) {
        NpSnapshot *snap = &n->snapshots[n->emitted % NP_SNAPSHOTS];
        if (!snap->valid || snap->frame != n->emitted) { n->fault = 3; return; }
        if (audio) audio(snap->audio, snap->audio_n);
        ++n->emitted;
    }
    f = end - end % 60;
    if (f && (n->hash_frame == NP_NONE || f > n->hash_frame)) {
        NpHash *h = &n->hashes[f & (NP_HISTORY - 1)];
        if (h->valid && h->frame == f) { n->hash_frame = f; n->hash = h->value; }
    }
    if (n->hash_frame != NP_NONE) check_hash(n, n->hash_frame);
    /* Compare delayed peer hashes as well as the newest checkpoint. */
    for (f = 0; f < NP_HISTORY; ++f)
        if (n->peer_hashes[f].valid) check_hash(n, n->peer_hashes[f].frame);
}
void np_delay_init(NpDelay *f, unsigned window)
{
    memset(f, 0, sizeof(*f)); f->delay = 1;
    f->window = window > NP_MAX_ROLLBACK ? NP_MAX_ROLLBACK : window;
}
unsigned np_delay_sample(NpDelay *f, unsigned rtt_ms)
{
    unsigned sorted[9], i, j, coverage, candidate;
    if (rtt_ms > 2000) return f->delay;
    f->rtt[f->pos] = (uint16_t)rtt_ms; f->pos = (f->pos + 1) % 9;
    if (f->count < 9) ++f->count;
    for (i = 0; i < f->count; ++i) sorted[i] = f->rtt[i];
    for (i = 1; i < f->count; ++i) {
        unsigned value = sorted[i]; j = i;
        while (j && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = value;
    }
    f->median = sorted[f->count / 2];
    f->jitter = sorted[f->count - 1] - f->median;
    /* ceil((RTT/2 + jitter) / 16.667ms) plus one scheduling frame.
     * The selected prediction window covers part of this budget. Fast routes
     * can use zero added delay; window zero budgets entirely for real input. */
    coverage = ((f->median + 2 * f->jitter) * 60 + 1999) / 2000 + 1;
    candidate = coverage > f->window ? coverage - f->window : 0;
    if (candidate > NP_MAX_DELAY) candidate = NP_MAX_DELAY;
    if (f->count < 9) return f->delay;
    if (!f->initialized) { f->initialized = 1; f->delay = candidate; f->pending = candidate; return f->delay; }
    if (candidate == f->delay) { f->pending = candidate; f->votes = 0; return f->delay; }
    if (candidate != f->pending) { f->pending = candidate; f->votes = 1; }
    else ++f->votes;
    if (f->votes >= (candidate > f->delay ? 8u : 24u)) { f->delay = candidate; f->votes = 0; }
    return f->delay;
}
