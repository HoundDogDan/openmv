#include "pfc_core.h"
#include <string.h>

enum { SLOT_FREE, SLOT_ACTIVE, SLOT_READY };
static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }
static bool valid(const uint8_t *r) {
    return u16(r) <= 5 && u16(r + 4) < 1000 && u16(r + 6) < 1000 &&
           (u16(r) > 1 || (u16(r + 8) < 320 && u16(r + 10) < 320));
}
static uint64_t timestamp(const uint8_t *r) {
    return (uint64_t)u16(r + 2) * 1000000 + (uint32_t)u16(r + 4) * 1000 + u16(r + 6);
}
static void bump(pfc_engine *e, unsigned watch, unsigned counter) {
    ++e->counters[counter]; ++e->per_pixel[watch][counter];
}
static int watch_index(const pfc_engine *e, uint32_t key) {
    for (unsigned i = 0; i < e->count; ++i) if (e->keys[i] == key) return (int)i;
    return -1;
}
static void next_end(pfc_engine *e) {
    e->next_end = UINT64_MAX;
    for (unsigned i = 0; i < e->count; ++i) {
        int index = e->active[i];
        if (index >= 0 && e->slots[index].end < e->next_end) e->next_end = e->slots[index].end;
    }
}
static void discard_active(pfc_engine *e, unsigned reason) {
    for (unsigned i = 0; i < e->count; ++i) {
        int index = e->active[i];
        if (index >= 0) {
            bump(e, i, reason);
            e->slots[index].state = SLOT_FREE;
            e->active[i] = -1;
            --e->pending;
        }
    }
    e->next_end = UINT64_MAX;
}
bool pfc_init(pfc_engine *e, pfc_slot *slots, unsigned capacity,
              const uint32_t *keys, unsigned count, bool reject_empty) {
    if (!e || !slots || !keys || !count || count > PFC_MAX_PIXELS ||
        capacity < 2 || capacity > PFC_MAX_CAPACITY || capacity < count) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (keys[i] >= 320u * 320u) return false;
        for (unsigned j = 0; j < i; ++j) if (keys[i] == keys[j]) return false;
    }
    memset(e, 0, sizeof(*e));
    memset(slots, 0, capacity * sizeof(*slots));
    e->slots = slots; e->capacity = capacity; e->count = count; e->reject_empty = reject_empty;
    memcpy(e->keys, keys, count * sizeof(*keys));
    for (unsigned i = 0; i < PFC_MAX_PIXELS; ++i) e->active[i] = -1;
    e->next_end = UINT64_MAX;
    return true;
}
void pfc_reset(pfc_engine *e) {
    uint32_t keys[PFC_MAX_PIXELS];
    memcpy(keys, e->keys, sizeof(keys));
    pfc_init(e, e->slots, e->capacity, keys, e->count, e->reject_empty);
}
static void advance(pfc_engine *e, uint64_t t) {
    if (e->have_previous && t - e->previous > PFC_GAP_US) {
        ++e->global_gaps_over_10ms;
        /* This precedes completion: a gap crossing window end rejects it. */
        discard_active(e, PFC_rejected_gap);
    }
    if (!e->have_previous) e->observed_from = t;
    e->previous = t; e->have_previous = true;
    if (t < e->next_end) return;
    /* At most eight active windows; finish chronologically, FIFO for ties. */
    for (;;) {
        int best = -1;
        for (unsigned w = 0; w < e->count; ++w) {
            int i = e->active[w];
            if (i < 0 || e->slots[i].end > t) continue;
            if (best < 0 || e->slots[i].end < e->slots[best].end ||
                (e->slots[i].end == e->slots[best].end && e->slots[i].order < e->slots[best].order)) best = i;
        }
        if (best < 0) break;
        pfc_slot *slot = &e->slots[best];
        e->active[slot->watch] = -1;
        bool empty = !slot->on_bins && !slot->off_bins;
        if (empty) bump(e, slot->watch, PFC_empty_windows);
        if (empty && e->reject_empty) {
            bump(e, slot->watch, PFC_rejected_empty);
            slot->state = SLOT_FREE; --e->pending;
        } else {
            slot->state = SLOT_READY;
            e->queue[(e->queue_head + e->queue_len) % e->capacity] = (uint8_t)best;
            ++e->queue_len;
            bump(e, slot->watch, PFC_completed);
            if (e->queue_len > e->peak_ready) e->peak_ready = e->queue_len;
        }
    }
    next_end(e);
}
static void header(pfc_engine *e, const uint8_t *h, uint64_t t) {
    int w = watch_index(e, (uint32_t)u16(h + 10) * 320 + u16(h + 8));
    if (w < 0) { ++e->unselected_candidates; return; }
    bump(e, (unsigned)w, PFC_candidates);
    uint32_t age = (uint32_t)t - u32(h);
    if ((uint64_t)age > t || t - age < e->observed_from) {
        bump(e, (unsigned)w, PFC_rejected_boundary); return;
    }
    if (e->active[w] >= 0) { bump(e, (unsigned)w, PFC_rejected_busy); return; }
    if (e->pending == e->capacity) { bump(e, (unsigned)w, PFC_rejected_capacity); return; }
    unsigned index = 0;
    while (e->slots[index].state != SLOT_FREE) ++index;
    pfc_slot *slot = &e->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->first = t - age; slot->end = slot->first + 16 * PFC_TB_US;
    slot->watch = (uint8_t)w; slot->state = SLOT_ACTIVE;
    bump(e, (unsigned)w, PFC_admitted);
    slot->order = e->counters[PFC_admitted];
    e->active[w] = (int16_t)index;
    ++e->pending;
    if (e->pending > e->peak_pending) e->peak_pending = e->pending;
    if (slot->end < e->next_end) e->next_end = slot->end;
}
int pfc_feed(pfc_engine *e, const uint8_t *rows, size_t n,
             const uint8_t *headers, size_t found) {
    if (e->finished) return -2;
    if (found > n) return -1;
    /* Prescan detects an epoch change before any window can complete from this
     * batch. This preserves the Python reference's whole-batch reset policy. */
    uint64_t prev = e->previous, first = 0, last = 0, invalid = 0, reversals = 0;
    bool have_prev = e->have_previous, have_row = false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = rows + i * PFC_ROW_BYTES;
        if (!valid(r)) { ++invalid; continue; }
        uint64_t t = timestamp(r);
        if (!have_row) first = t;
        if (have_prev && t < prev) ++reversals;
        prev = last = t; have_prev = have_row = true;
    }
    if (found && !have_row) return -1;
    if (!reversals) {
        uint64_t prev_header = first;
        for (size_t i = 0; i < found; ++i) {
            const uint8_t *h = headers + i * PFC_ROW_BYTES;
            uint32_t age = (uint32_t)last - u32(h + 4);
            uint32_t span = u32(h + 4) - u32(h);
            if ((uint64_t)age > last || last - age < prev_header ||
                u16(h + 8) >= 320 || u16(h + 10) >= 320 ||
                span < 3 * (1956 - 489) || span > 3 * (1956 + 489)) return -1;
            prev_header = last - age;
        }
    }
    e->rows += n; e->invalid_rows += invalid; e->headers_seen += found;
    if (have_row && !e->have_first) { e->first_seen = first; e->have_first = true; }
    if (reversals) {
        ++e->timestamp_reset_batches; e->timestamp_reversals += reversals;
        e->headers_in_discarded_reset_batches += found;
        discard_active(e, PFC_rejected_reset);
        e->previous = e->observed_from = last; e->have_previous = true;
        return 1;
    }
    size_t next_header = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = rows + i * PFC_ROW_BYTES;
        if (!valid(r)) continue;
        uint64_t t = timestamp(r);
        advance(e, t);
        while (next_header < found &&
               (uint32_t)((uint32_t)t - u32(headers + next_header * PFC_ROW_BYTES + 4)) < UINT32_C(0x80000000)) {
            header(e, headers + next_header * PFC_ROW_BYTES, t);
            ++next_header;
        }
        uint16_t type = u16(r);
        if (type > 1) continue;
        int w = watch_index(e, (uint32_t)u16(r + 10) * 320 + u16(r + 8));
        if (w < 0 || e->active[w] < 0) continue;
        pfc_slot *slot = &e->slots[e->active[w]];
        uint64_t start = slot->first + 4 * PFC_TB_US;
        if (t < start || t >= slot->end) continue;
        unsigned channel = type == 1 ? 0 : 1;
        size_t offset = (size_t)((t - start) / PFC_TICK_US) * 2 + channel;
        if (!slot->packet[offset]) {
            slot->packet[offset] = 1;
            if (channel) ++slot->off_bins; else ++slot->on_bins;
        }
    }
    return next_header == found ? 0 : -1;
}
bool pfc_pop(pfc_engine *e, uint8_t packet[PFC_PACKET_BYTES], pfc_meta *meta) {
    if (!e->queue_len) return false;
    unsigned index = e->queue[e->queue_head];
    pfc_slot *slot = &e->slots[index];
    memcpy(packet, slot->packet, PFC_PACKET_BYTES);
    meta->first = slot->first; meta->x = e->keys[slot->watch] % 320;
    meta->y = e->keys[slot->watch] / 320;
    meta->on_bins = slot->on_bins; meta->off_bins = slot->off_bins;
    bump(e, slot->watch, PFC_delivered);
    slot->state = SLOT_FREE;
    e->queue_head = (e->queue_head + 1) % e->capacity;
    --e->queue_len; --e->pending;
    return true;
}
void pfc_finish(pfc_engine *e) {
    discard_active(e, PFC_incomplete_at_stop); e->finished = true;
}
bool pfc_accounting_ok(const pfc_engine *e) {
    const uint64_t *c = e->counters;
    return c[PFC_admitted] == c[PFC_delivered] + c[PFC_rejected_gap] +
        c[PFC_rejected_reset] + c[PFC_incomplete_at_stop] + c[PFC_rejected_empty] + e->pending &&
        c[PFC_completed] == c[PFC_delivered] + e->queue_len &&
        c[PFC_candidates] == c[PFC_admitted] + c[PFC_rejected_capacity] +
        c[PFC_rejected_busy] + c[PFC_rejected_boundary];
}
