#include "ook_frontend.h"
#include <string.h>

#define F_HAVE_LAST 1u
#define HALF_RANGE UINT32_C(0x80000000)
#define ALIGN8(n) (((n) + (size_t)7) & ~(size_t)7)

typedef struct {
    uint32_t last_on_us, run_start_us;
    uint16_t capture;
    uint8_t run, flags;
} Pixel;

typedef struct {
    OofSample sample;
    uint32_t deadline, pixel_id;
} Capture;

struct OofReceiver {
    OofConfig cfg;
    OofStats st;
    OofEmit emit;
    void *user;
    Pixel *pixels;
    Capture *captures;
    uint16_t *heap, *free_ids;
    uint16_t heap_n, free_n;
    uint8_t have_clock;
    uint32_t npixels;
};

static int valid(const OofConfig *c) {
    if (!c || !c->width || !c->height || !c->capture_capacity ||
        c->capture_capacity == OOF_NO_CAPTURE || c->reserved) return 0;
    /* Bounds avoid workspace-size overflow even on 32-bit hosts. */
    if (c->width > 320 || c->height > 320) return 0;
    if (!c->bit_us || c->bit_us > 4095 || !c->tolerance_us || !c->min_gap_us ||
        c->min_gap_us >= HALF_RANGE || c->refractory_us > c->bit_us ||
        c->tolerance_us >= (c->bit_us + 1) / 2) return 0;
    if (15u * c->bit_us + c->tolerance_us > UINT16_MAX) return 0;
    return 1;
}
void oof_default_config(OofConfig *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->width = c->height = 320;
    c->capture_capacity = 512;
    c->bit_us = 1956;
    c->tolerance_us = c->refractory_us = 489;
    c->min_gap_us = 8u * c->bit_us;
}
size_t oof_pixel_state_bytes(void) { return sizeof(Pixel); }
size_t oof_capture_state_bytes(void) { return sizeof(Capture); }
size_t oof_workspace_bytes(const OofConfig *c) {
    if (!valid(c)) return 0;
    return ALIGN8(sizeof(OofReceiver))
        + ALIGN8((size_t)c->width * c->height * sizeof(Pixel))
        + ALIGN8((size_t)c->capture_capacity * sizeof(Capture))
        + ALIGN8((size_t)c->capture_capacity * sizeof(uint16_t)) * 2;
}
OofReceiver *oof_init(void *w, size_t bytes, const OofConfig *c,
                      OofEmit emit, void *user) {
    size_t need = oof_workspace_bytes(c);
    uint8_t *p;
    OofReceiver *r;
    if (!w || !need || bytes < need || ((uintptr_t)w & 7u)) return NULL;
    memset(w, 0, need);
    r = (OofReceiver *)w;
    r->cfg = *c; r->emit = emit; r->user = user;
    r->npixels = (uint32_t)c->width * c->height;
    p = (uint8_t *)w + ALIGN8(sizeof(*r));
    r->pixels = (Pixel *)p;
    p += ALIGN8((size_t)r->npixels * sizeof(Pixel));
    r->captures = (Capture *)p;
    p += ALIGN8((size_t)c->capture_capacity * sizeof(Capture));
    r->heap = (uint16_t *)p;
    p += ALIGN8((size_t)c->capture_capacity * sizeof(uint16_t));
    r->free_ids = (uint16_t *)p;
    oof_reset(r);
    return r;
}
void oof_reset(OofReceiver *r) {
    uint32_t i;
    if (!r) return;
    memset(&r->st, 0, sizeof(r->st));
    memset(r->pixels, 0, (size_t)r->npixels * sizeof(Pixel));
    for (i = 0; i < r->npixels; ++i) r->pixels[i].capture = OOF_NO_CAPTURE;
    for (i = 0; i < r->cfg.capture_capacity; ++i) r->free_ids[i] = (uint16_t)i;
    r->free_n = r->cfg.capture_capacity;
    r->heap_n = 0; r->have_clock = 0;
}
uint8_t oof_crc4(uint8_t data) {
    uint8_t reg = 0;
    int i;
    for (i = 7; i >= 0; --i) {
        uint8_t fb = (uint8_t)(((reg >> 3) & 1u) ^ ((data >> i) & 1u));
        reg = (uint8_t)((reg << 1) & 15u);
        if (fb) reg ^= 3u;
    }
    return reg;
}
/* Legal outstanding deadlines differ by far less than 2^31. */
static int before(uint32_t a, uint32_t b) {
    return a != b && (uint32_t)(b - a) < HALF_RANGE;
}
static int capture_before(const OofReceiver *r, uint16_t a, uint16_t b) {
    uint32_t da = r->captures[a].deadline, db = r->captures[b].deadline;
    if (da == db) return r->captures[a].pixel_id < r->captures[b].pixel_id;
    return before(da, db);
}
static void heap_push(OofReceiver *r, uint16_t id) {
    uint16_t i = r->heap_n++;
    while (i) {
        uint16_t parent = (uint16_t)((i - 1u) / 2u);
        if (!capture_before(r, id, r->heap[parent])) break;
        r->heap[i] = r->heap[parent]; i = parent;
    }
    r->heap[i] = id;
}
static uint16_t heap_pop(OofReceiver *r) {
    uint16_t result = r->heap[0], id = r->heap[--r->heap_n], i = 0;
    while ((uint32_t)i * 2u + 1u < r->heap_n) {
        uint16_t child = (uint16_t)(2u * i + 1u);
        if (child + 1u < r->heap_n &&
            capture_before(r, r->heap[child + 1u], r->heap[child])) ++child;
        if (!capture_before(r, r->heap[child], id)) break;
        r->heap[i] = r->heap[child]; i = child;
    }
    if (r->heap_n) r->heap[i] = id;
    return result;
}
static void expire(OofReceiver *r, uint32_t now) {
    while (r->heap_n) {
        uint16_t id = r->heap[0];
        Capture *c = &r->captures[id];
        OofSample *s = &c->sample;
        if ((uint32_t)(now - c->deadline) >= HALF_RANGE) break;
        (void)heap_pop(r);
        r->pixels[c->pixel_id].capture = OOF_NO_CAPTURE;
        r->st.active--;
        if (!s->n_events) { s->flags |= OOF_FLAG_NO_POST_EVENTS; r->st.samples_no_post++; }
        r->st.samples++;
        if (((s->word >> 8) & 15u) == oof_crc4((uint8_t)s->word)) r->st.crc_ok++;
        else r->st.crc_bad++;
        if (r->emit) r->emit(r->user, s);
        r->free_ids[r->free_n++] = id;
    }
}
static int set_clock(OofReceiver *r, uint32_t t) {
    if (r->have_clock) {
        uint32_t dt = t - r->st.last_event_us;
        if (dt >= HALF_RANGE) { r->st.order_errors++; return OOF_ERR_ORDER; }
        if (dt > r->st.max_stream_gap_us) r->st.max_stream_gap_us = dt;
    }
    r->have_clock = 1; r->st.last_event_us = t;
    expire(r, t); /* [lo, hi): expire BEFORE accepting an event exactly at hi. */
    return OOF_OK;
}
int oof_advance_watermark(OofReceiver *r, uint32_t now) {
    return r ? set_clock(r, now) : OOF_ERR_CONFIG;
}
static uint32_t absdiff(uint32_t a, uint32_t b) { return a >= b ? a - b : b - a; }

/* The gap is attached to run_start, not a sticky permission bit. A broken
 * header candidate cannot borrow an earlier gap to lock onto payload 1111. */
static void begin_if_gap(OofReceiver *r, Pixel *p, uint32_t t,
                          uint32_t dt, int had_previous) {
    p->run = 0;
    if (had_previous && dt >= r->cfg.min_gap_us) {
        p->run = 1; p->run_start_us = t; r->st.run_starts++;
    }
}
static void lock_header(OofReceiver *r, Pixel *p, uint32_t pixel_id,
                         uint16_t x, uint16_t y, uint32_t header_error) {
    uint16_t id;
    Capture *c;
    r->st.header_locks++;
    p->run = 0;
    if (!r->free_n) { r->st.pool_exhausted++; return; }
    id = r->free_ids[--r->free_n]; c = &r->captures[id];
    memset(c, 0, sizeof(*c));
    c->pixel_id = pixel_id;
    c->deadline = p->run_start_us + 15u * r->cfg.bit_us + r->cfg.tolerance_us;
    c->sample.t0_us = p->run_start_us;
    c->sample.x = x; c->sample.y = y; c->sample.word = UINT16_C(0xf000);
    /* Final header pulse's phase error; all four positions were checked. */
    c->sample.header_last_error_us = (uint16_t)header_error;
    p->capture = id;
    heap_push(r, id);
    r->st.active++;
    if (r->st.active > r->st.peak_active) r->st.peak_active = r->st.active;
}
int oof_feed_event(OofReceiver *r, uint32_t t, uint16_t x,
                   uint16_t y, uint16_t type) {
    uint32_t pid, dt = 0;
    Pixel *p;
    int had, rc;
    if (!r) return OOF_ERR_CONFIG;
    rc = set_clock(r, t);
    if (rc != OOF_OK) return rc;
    r->st.rows++;
    if (type == 0) { r->st.off_events++; return OOF_OK; }
    if (type != 1) { r->st.other_events++; return OOF_OK; }
    r->st.on_events++;
    if (x >= r->cfg.width || y >= r->cfg.height) {
        r->st.invalid_xy++; return OOF_OK;
    }
    pid = (uint32_t)y * r->cfg.width + x; p = &r->pixels[pid];
    had = (p->flags & F_HAVE_LAST) != 0;
    if (had) {
        dt = t - p->last_on_us;
        if (dt < r->cfg.refractory_us) { r->st.debounce_rejected++; return OOF_OK; }
    } else { r->st.cold_first_on++; }
    p->last_on_us = t; p->flags |= F_HAVE_LAST;
    if (p->capture != OOF_NO_CAPTURE) {
        OofSample *s = &r->captures[p->capture].sample;
        uint32_t rel = t - s->t0_us;
        uint32_t lo = 4u * r->cfg.bit_us - r->cfg.tolerance_us;
        if (rel >= lo) {
            uint32_t slot = (rel + r->cfg.bit_us / 2u) / r->cfg.bit_us;
            uint32_t error = absdiff(rel, slot * r->cfg.bit_us);
            if (s->n_events < OOF_POST_CAP) s->dt_us[s->n_events++] = (uint16_t)rel;
            else { s->flags |= OOF_FLAG_TIME_OVERFLOW; r->st.time_overflows++; }
            if (error > s->max_post_error_us) s->max_post_error_us = (uint16_t)error;
            if (slot >= 4 && slot < 16 && error <= r->cfg.tolerance_us) {
                s->word |= (uint16_t)(1u << (15u - slot));
            } else {
                s->flags |= OOF_FLAG_OFFGRID;
                if (s->offgrid_events != UINT16_MAX) s->offgrid_events++;
                r->st.offgrid_events++;
            }
        }
        return OOF_OK;
    }
    if (p->run) {
        uint32_t phase = absdiff(t - p->run_start_us, (uint32_t)p->run * r->cfg.bit_us);
        if (absdiff(dt, r->cfg.bit_us) <= r->cfg.tolerance_us && phase <= r->cfg.tolerance_us) {
            p->run++;
            if (p->run == 4) lock_header(r, p, pid, x, y, phase);
        } else {
            r->st.run_breaks++;
            begin_if_gap(r, p, t, dt, had);
        }
    } else begin_if_gap(r, p, t, dt, had);
    return OOF_OK;
}
static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
int oof_feed_rows_le(OofReceiver *r, const void *data, size_t n, size_t *done) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    if (done) *done = 0;
    if (!r || (!data && n)) return OOF_ERR_CONFIG;
    if (n > SIZE_MAX / 12u) return OOF_ERR_ROW;
    for (i = 0; i < n; ++i, p += 12) {
        uint16_t type = u16le(p), sec = u16le(p+2), ms = u16le(p+4), us = u16le(p+6);
        uint32_t t;
        int rc;
        if (ms >= 1000 || us >= 1000) { r->st.invalid_rows++; return OOF_ERR_ROW; }
        t = (uint32_t)((uint64_t)sec * 1000000u + (uint32_t)ms * 1000u + us);
        rc = oof_feed_event(r, t, u16le(p+8), u16le(p+10), type);
        if (rc != OOF_OK) return rc;
        if (done) *done = i + 1;
    }
    return OOF_OK;
}
const OofStats *oof_stats(const OofReceiver *r) { return r ? &r->st : NULL; }
