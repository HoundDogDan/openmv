#ifndef PFC_CORE_H
#define PFC_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PFC_VERSION "0.1.0"
#define PFC_MAX_PIXELS 8
#define PFC_MAX_CAPACITY 64
#define PFC_ROW_BYTES 12
#define PFC_PACKET_BYTES 598
#define PFC_TB_US 1956
#define PFC_TICK_US 100
#define PFC_INPUT_TICKS 235
#define PFC_TAIL_TICKS 64
#define PFC_GAP_US 10000

/* The same selected-window counters are available globally and per pixel. */
#define PFC_PIXEL_COUNTERS(X) \
    X(candidates) X(admitted) X(rejected_capacity) X(rejected_busy) \
    X(rejected_boundary) X(rejected_gap) X(rejected_reset) X(incomplete_at_stop) \
    X(empty_windows) X(rejected_empty) X(completed) X(delivered)
#define PFC_ENUM(name) PFC_##name,
enum { PFC_PIXEL_COUNTERS(PFC_ENUM) PFC_COUNTER_COUNT };
#undef PFC_ENUM

typedef struct {
    uint64_t first, end, order;
    uint16_t on_bins, off_bins;
    uint8_t watch, state;
    uint8_t packet[PFC_PACKET_BYTES];
} pfc_slot;
typedef struct {
    uint64_t first;
    uint16_t x, y, on_bins, off_bins;
} pfc_meta;
typedef struct {
    pfc_slot *slots;
    uint32_t keys[PFC_MAX_PIXELS], count, capacity;
    int16_t active[PFC_MAX_PIXELS];
    uint8_t queue[PFC_MAX_CAPACITY];
    uint32_t queue_head, queue_len, pending, peak_pending, peak_ready;
    bool reject_empty, have_previous, have_first, finished;
    uint64_t previous, observed_from, first_seen, next_end;
    uint64_t counters[PFC_COUNTER_COUNT];
    uint64_t per_pixel[PFC_MAX_PIXELS][PFC_COUNTER_COUNT];
    uint64_t rows, invalid_rows, headers_seen, unselected_candidates;
    uint64_t headers_in_discarded_reset_batches, timestamp_reset_batches;
    uint64_t timestamp_reversals, global_gaps_over_10ms;
} pfc_engine;

/* Caller owns the bounded slot allocation. No allocation occurs in these APIs. */
bool pfc_init(pfc_engine *e, pfc_slot *slots, unsigned capacity,
              const uint32_t *pixel_keys, unsigned count, bool reject_empty);
void pfc_reset(pfc_engine *e);
/* Source: <6H(type,sec,ms,us,x,y); headers: <IIHH(first32,last32,x,y).
 * Return 1 on a timestamp reversal (discard whole batch; caller clears header
 * detector state), 0 normally, -1 for malformed headers, -2 after finish().
 * Valid input batch timestamps and header outputs must come from the SAME feed.
 */
int pfc_feed(pfc_engine *e, const uint8_t *rows, size_t n,
             const uint8_t *headers, size_t found);
bool pfc_pop(pfc_engine *e, uint8_t packet[PFC_PACKET_BYTES], pfc_meta *meta);
void pfc_finish(pfc_engine *e);
bool pfc_accounting_ok(const pfc_engine *e);
#endif
