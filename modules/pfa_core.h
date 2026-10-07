#ifndef PFA_CORE_H
#define PFA_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PFA_VERSION "0.1.0"
#define PFA_MAX_CAPACITY 64
#define PFA_ROW_BYTES 12
#define PFA_PACKET_BYTES 598
#define PFA_TB_US 1956
#define PFA_TICK_US 100
#define PFA_INPUT_TICKS 235
#define PFA_TAIL_TICKS 64
#define PFA_GAP_US 10000
#define PFA_COUNTERS(X) \
    X(candidates) X(admitted) X(rejected_capacity) X(rejected_busy) \
    X(rejected_boundary) X(rejected_gap) X(rejected_reset) X(incomplete_at_stop) \
    X(empty_windows) X(rejected_empty) X(completed) X(delivered)
#define PFA_ENUM(name) PFA_##name,
enum { PFA_COUNTERS(PFA_ENUM) PFA_COUNTER_COUNT };
#undef PFA_ENUM
typedef struct {
    uint64_t first, end, order;
    uint32_t key;
    uint16_t on_bins, off_bins;
    uint8_t state;
    uint8_t packet[PFA_PACKET_BYTES];
} pfa_slot;
typedef struct {
    uint64_t first;
    uint16_t x, y, on_bins, off_bins;
} pfa_meta;
typedef struct {
    pfa_slot *slots;
    /* Caller allocates width*width bytes on the GC heap, not in static SRAM.
     * 0 means inactive; 1..capacity identify active packet slots. */
    uint8_t *active_map;
    uint32_t capacity, width, lo;
    uint8_t queue[PFA_MAX_CAPACITY], free_slots[PFA_MAX_CAPACITY];
    uint32_t free_count, queue_head, queue_len, pending, peak_pending, peak_ready;
    bool reject_empty, have_previous, have_first, finished;
    uint64_t previous, observed_from, first_seen, next_end;
    uint64_t counters[PFA_COUNTER_COUNT];
    uint64_t rows, invalid_rows, headers_seen, outside_roi_candidates;
    uint64_t headers_in_discarded_reset_batches, timestamp_reset_batches;
    uint64_t timestamp_reversals, global_gaps_over_10ms;
} pfa_engine;
bool pfa_init(pfa_engine *, pfa_slot *, uint8_t *, unsigned capacity,
              unsigned width, bool reject_empty);
void pfa_reset(pfa_engine *);
/* Header records must be generated from exactly the same camera batch.
 * 1=timestamp epoch reset, 0=normal, -1=malformed header, -2=finished. */
int pfa_feed(pfa_engine *, const uint8_t *rows, size_t n,
             const uint8_t *headers, size_t found);
bool pfa_pop(pfa_engine *, uint8_t packet[PFA_PACKET_BYTES], pfa_meta *);
void pfa_finish(pfa_engine *);
bool pfa_accounting_ok(const pfa_engine *);
#endif
