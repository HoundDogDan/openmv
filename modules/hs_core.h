#ifndef HS_CORE_H
#define HS_CORE_H
#include <stdint.h>
#include <stddef.h>
/* One active neuron in a three-neuron delayed coincidence chain per pixel.
 * Its state is packed because the chain cannot have parallel active stages. */
typedef struct { uint32_t last, packed; } hs_pixel;
typedef struct { uint32_t tb, tol, gap, refr, frame_bits; } hs_model;
typedef struct {
    uint64_t rows, on, off, triggers, invalid, outside_roi, accepted_on;
    uint64_t refractory_rejects, seeds, neuron_spikes, headers;
    uint64_t output_overflow, timestamp_resets;
} hs_stats;
typedef struct {
    hs_pixel *pixels;
    uint32_t allocation_width, width, lo, origin, origin_valid;
    uint64_t previous_raw;
    hs_model model;
    hs_stats stats;
} hs_engine;
int hs_valid_model(const hs_model *m);
void hs_init(hs_engine *e, hs_pixel *pixels, unsigned width, hs_model m);
int hs_set_roi(hs_engine *e, unsigned width);
void hs_reset(hs_engine *e);
/* Input: <6H (type,sec,ms,us,x,y). Output: <IIHH (first_us,last_us,x,y).
 * Both timestamps in output are modulo 2^32 sensor microseconds.
 * Returns records written. Total detections also counted when output is full. */
size_t hs_feed(hs_engine *e, const uint8_t *rows, size_t n,
               uint8_t *output, size_t output_records);
#endif
