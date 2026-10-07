/* Full-ROI automatic admission. Packet timing/binning matches payload_frontend
 * 0.1.0; pixels are independent and share a bounded pool of packet slots. */
#include "pfa_core.h"
#include <string.h>
enum { SLOT_FREE, SLOT_ACTIVE, SLOT_READY };
static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p+2) << 16); }
static bool valid(const uint8_t *r) {
    return u16(r) <= 5 && u16(r+4) < 1000 && u16(r+6) < 1000 &&
           (u16(r) > 1 || (u16(r+8) < 320 && u16(r+10) < 320));
}
static uint64_t timestamp(const uint8_t *r) {
    return (uint64_t)u16(r+2)*1000000 + (uint32_t)u16(r+4)*1000 + u16(r+6);
}
static int map_index(const pfa_engine *e, unsigned x, unsigned y) {
    if (x < e->lo || y < e->lo || x >= e->lo+e->width || y >= e->lo+e->width) return -1;
    return (int)((y-e->lo)*e->width+x-e->lo);
}
static void deactivate(pfa_engine *e, pfa_slot *s) {
    e->active_map[map_index(e, s->key%320, s->key/320)] = 0;
}
static void release(pfa_engine *e, unsigned i) {
    e->slots[i].state = SLOT_FREE;
    e->free_slots[e->free_count++] = (uint8_t)i;
    --e->pending;
}
static void next_end(pfa_engine *e) {
    e->next_end = UINT64_MAX;
    for (unsigned i=0; i<e->capacity; ++i)
        if (e->slots[i].state == SLOT_ACTIVE && e->slots[i].end < e->next_end)
            e->next_end = e->slots[i].end;
}
static void discard_active(pfa_engine *e, unsigned reason) {
    for (unsigned i=0; i<e->capacity; ++i) {
        if (e->slots[i].state != SLOT_ACTIVE) continue;
        ++e->counters[reason];
        deactivate(e, &e->slots[i]);
        release(e, i);
    }
    e->next_end = UINT64_MAX;
}
bool pfa_init(pfa_engine *e, pfa_slot *slots, uint8_t *map, unsigned cap,
              unsigned width, bool reject_empty) {
    if (!e || !slots || !map || cap<2 || cap>PFA_MAX_CAPACITY ||
        width<20 || width>320 || width%2) return false;
    memset(e, 0, sizeof(*e));
    memset(slots, 0, cap*sizeof(*slots));
    memset(map, 0, width*width);
    e->slots=slots; e->active_map=map; e->capacity=cap;
    e->width=width; e->lo=(320-width)/2; e->reject_empty=reject_empty;
    e->free_count=cap;
    for (unsigned i=0; i<cap; ++i) e->free_slots[i]=(uint8_t)(cap-i-1);
    e->next_end=UINT64_MAX;
    return true;
}
void pfa_reset(pfa_engine *e) {
    pfa_init(e, e->slots, e->active_map, e->capacity, e->width, e->reject_empty);
}
static void advance(pfa_engine *e, uint64_t t) {
    if (e->have_previous && t-e->previous>PFA_GAP_US) {
        ++e->global_gaps_over_10ms;
        discard_active(e, PFA_rejected_gap);
    }
    if (!e->have_previous) e->observed_from=t;
    e->previous=t; e->have_previous=true;
    if (t<e->next_end) return;
    /* Sort completed windows by end time, then admission order. This scan is
     * needed only when a window expires, never for ordinary event routing. */
    for (;;) {
        int best=-1;
        for (unsigned i=0; i<e->capacity; ++i) {
            pfa_slot *s=&e->slots[i];
            if (s->state != SLOT_ACTIVE || s->end>t) continue;
            if (best<0 || s->end<e->slots[best].end ||
                (s->end==e->slots[best].end && s->order<e->slots[best].order)) best=(int)i;
        }
        if (best<0) break;
        pfa_slot *s=&e->slots[best];
        deactivate(e,s);
        bool empty=!s->on_bins && !s->off_bins;
        if (empty) ++e->counters[PFA_empty_windows];
        if (empty && e->reject_empty) {
            ++e->counters[PFA_rejected_empty];
            release(e,(unsigned)best);
        } else {
            s->state=SLOT_READY;
            e->queue[(e->queue_head+e->queue_len)%e->capacity]=(uint8_t)best;
            ++e->queue_len; ++e->counters[PFA_completed];
            if (e->queue_len>e->peak_ready) e->peak_ready=e->queue_len;
        }
    }
    next_end(e);
}
static void header(pfa_engine *e, const uint8_t *h, uint64_t t) {
    unsigned x=u16(h+8), y=u16(h+10);
    int key=map_index(e,x,y);
    if (key<0) { ++e->outside_roi_candidates; return; }
    ++e->counters[PFA_candidates];
    uint32_t age=(uint32_t)t-u32(h);
    if ((uint64_t)age>t || t-age<e->observed_from) {
        ++e->counters[PFA_rejected_boundary]; return;
    }
    if (e->active_map[key]) { ++e->counters[PFA_rejected_busy]; return; }
    if (!e->free_count) { ++e->counters[PFA_rejected_capacity]; return; }
    unsigned index=e->free_slots[--e->free_count];
    pfa_slot *s=&e->slots[index];
    memset(s,0,sizeof(*s));
    s->first=t-age; s->end=s->first+16*PFA_TB_US; s->key=y*320+x;
    s->state=SLOT_ACTIVE;
    s->order=++e->counters[PFA_admitted];
    e->active_map[key]=(uint8_t)(index+1);
    ++e->pending;
    if (e->pending>e->peak_pending) e->peak_pending=e->pending;
    if (s->end<e->next_end) e->next_end=s->end;
}
int pfa_feed(pfa_engine *e, const uint8_t *rows, size_t n,
             const uint8_t *headers, size_t found) {
    if (e->finished) return -2;
    if (found > n) return -1;
    /* Prescan detects an epoch change before any window can complete from this
     * batch. This preserves the Python reference's whole-batch reset policy. */
    uint64_t prev = e->previous, first = 0, last = 0, invalid = 0, reversals = 0;
    bool have_prev = e->have_previous, have_row = false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = rows + i * PFA_ROW_BYTES;
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
            const uint8_t *h = headers + i * PFA_ROW_BYTES;
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
        discard_active(e, PFA_rejected_reset);
        e->previous = e->observed_from = last; e->have_previous = true;
        return 1;
    }
    size_t next_header = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = rows + i * PFA_ROW_BYTES;
        if (!valid(r)) continue;
        uint64_t t = timestamp(r);
        advance(e, t);
        while (next_header < found &&
               (uint32_t)((uint32_t)t - u32(headers + next_header * PFA_ROW_BYTES + 4)) < UINT32_C(0x80000000)) {
            header(e, headers + next_header * PFA_ROW_BYTES, t);
            ++next_header;
        }
        uint16_t type = u16(r);
        if (type > 1) continue;
        int key = map_index(e, u16(r + 8), u16(r + 10));
        if (key < 0 || !e->active_map[key]) continue;
        pfa_slot *slot = &e->slots[e->active_map[key] - 1];
        uint64_t start = slot->first + 4 * PFA_TB_US;
        if (t < start || t >= slot->end) continue;
        unsigned channel = type == 1 ? 0 : 1;
        size_t offset = (size_t)((t - start) / PFA_TICK_US) * 2 + channel;
        if (!slot->packet[offset]) {
            slot->packet[offset] = 1;
            if (channel) ++slot->off_bins; else ++slot->on_bins;
        }
    }
    return next_header == found ? 0 : -1;
}

bool pfa_pop(pfa_engine *e, uint8_t packet[PFA_PACKET_BYTES], pfa_meta *meta) {
    if (!e->queue_len) return false;
    unsigned index=e->queue[e->queue_head];
    pfa_slot *s=&e->slots[index];
    memcpy(packet,s->packet,PFA_PACKET_BYTES);
    meta->first=s->first; meta->x=s->key%320; meta->y=s->key/320;
    meta->on_bins=s->on_bins; meta->off_bins=s->off_bins;
    ++e->counters[PFA_delivered];
    e->queue_head=(e->queue_head+1)%e->capacity; --e->queue_len;
    release(e,index);
    return true;
}
void pfa_finish(pfa_engine *e) { discard_active(e,PFA_incomplete_at_stop); e->finished=true; }
bool pfa_accounting_ok(const pfa_engine *e) {
    const uint64_t *c=e->counters;
    return e->free_count+e->pending==e->capacity &&
        c[PFA_admitted]==c[PFA_delivered]+c[PFA_rejected_gap]+c[PFA_rejected_reset]+
            c[PFA_incomplete_at_stop]+c[PFA_rejected_empty]+e->pending &&
        c[PFA_completed]==c[PFA_delivered]+e->queue_len &&
        c[PFA_candidates]==c[PFA_admitted]+c[PFA_rejected_capacity]+
            c[PFA_rejected_busy]+c[PFA_rejected_boundary];
}
