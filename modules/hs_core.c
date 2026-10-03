#include "hs_core.h"
#include <string.h>
#define SEEN (1u << 18)
#define BLOCK (1u << 19)
#define STAGE(p) (((p) >> 16) & 3u)
#define AGE(p) ((p) & 65535u)
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v) { w16(p,(uint16_t)v); w16(p+2,(uint16_t)(v>>16)); }
int hs_valid_model(const hs_model *m) {
    return m->tb >= 100 && m->tb <= 4000 && m->tol < m->tb &&
        m->refr <= m->tb-m->tol && m->gap > m->tb+m->tol &&
        m->gap <= 1000000 && m->frame_bits >= 4 && m->frame_bits <= 16 &&
        m->frame_bits*m->tb <= 65535;
}
void hs_reset(hs_engine *e) {
    memset(e->pixels, 0, (size_t)e->allocation_width*e->allocation_width*sizeof(hs_pixel));
    memset(&e->stats,0,sizeof(e->stats));
    e->origin_valid=0; e->previous_raw=0;
}
void hs_init(hs_engine *e, hs_pixel *pixels, unsigned width, hs_model m) {
    memset(e,0,sizeof(*e)); e->pixels=pixels; e->allocation_width=width;
    e->width=width; e->lo=(320-width)/2; e->model=m; hs_reset(e);
}
int hs_set_roi(hs_engine *e, unsigned width) {
    if (width < 20 || width > e->allocation_width || (width & 1)) return 0;
    e->width=width; e->lo=(320-width)/2;
    memset(e->pixels,0,(size_t)e->allocation_width*e->allocation_width*sizeof(hs_pixel));
    e->origin_valid=0; /* counters survive ROI changes, temporal states do not */
    return 1;
}
size_t hs_feed(hs_engine *e, const uint8_t *rows, size_t n,
               uint8_t *output, size_t output_records) {
    size_t written=0;
    const hs_model *m=&e->model;
    for (size_t i=0;i<n;i++) {
        const uint8_t *r=rows+i*12;
        uint32_t type=u16(r), sec=u16(r+2), ms=u16(r+4), us=u16(r+6);
        uint32_t x=u16(r+8), y=u16(r+10);
        e->stats.rows++;
        if (type>5 || ms>=1000 || us>=1000 || (type<=1 && (x>=320 || y>=320))) {
            e->stats.invalid++; continue;
        }
        uint64_t raw=(uint64_t)sec*1000000+ms*1000+us;
        uint32_t t=(uint32_t)raw;
        if (e->origin_valid && raw<e->previous_raw) {
            /* Timestamp reset / out-of-order stream: don't stitch headers over it. */
            memset(e->pixels,0,(size_t)e->allocation_width*e->allocation_width*sizeof(hs_pixel));
            e->origin_valid=0; e->stats.timestamp_resets++;
        }
        e->previous_raw=raw;
        if (!e->origin_valid) { e->origin=t; e->origin_valid=1; }
        if (type>1) { e->stats.triggers++; continue; }
        if (type==0) e->stats.off++; else e->stats.on++;
        if (x<e->lo || x>=e->lo+e->width || y<e->lo || y>=e->lo+e->width) {
            e->stats.outside_roi++; continue;
        }
        if (type!=1) continue;
        hs_pixel *s=&e->pixels[(y-e->lo)*e->width+(x-e->lo)];
        uint32_t p=s->packed, seen=p&SEEN;
        uint32_t dt=seen ? t-s->last : t-e->origin;
        if (seen && dt<m->refr) { e->stats.refractory_rejects++; continue; }
        e->stats.accepted_on++;
        s->last=t;
        if (p&BLOCK) {
            if (dt<AGE(p)) { s->packed=SEEN|BLOCK|(AGE(p)-dt); continue; }
            p=SEEN; /* frame lockout elapsed */
        }
        unsigned stage=STAGE(p);
        if (stage && dt>=m->tb-m->tol && dt<=m->tb+m->tol) {
            /* A delayed rectangular EPSP contributes 1 inside this window.
             * Current ON contributes 1. Threshold 2 produces one neuron spike.
             * The spike resets this neuron and arms the next delayed synapse. */
            uint32_t span=AGE(p)+dt;
            e->stats.neuron_spikes++;
            if (stage<3) {
                s->packed=SEEN|((stage+1)<<16)|span;
            } else {
                e->stats.headers++;
                if (written<output_records) {
                    uint8_t *o=output+written*12;
                    w32(o,t-span); w32(o+4,t); w16(o+8,(uint16_t)x); w16(o+10,(uint16_t)y);
                    written++;
                } else e->stats.output_overflow++;
                uint32_t frame=m->frame_bits*m->tb;
                s->packed=SEEN|(frame>span ? BLOCK|(frame-span) : 0);
            }
        } else {
            /* An accepted mistimed ON inhibits the partial chain. A long quiet
             * gap can seed a new chain. No seed before an observed gap at startup. */
            s->packed=SEEN;
            if (dt>=m->gap) { s->packed|=1u<<16; e->stats.seeds++; }
        }
    }
    return written;
}
