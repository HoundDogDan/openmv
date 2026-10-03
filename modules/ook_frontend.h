#ifndef OOK_FRONTEND_H
#define OOK_FRONTEND_H
/* Option-3 per-pixel OOK front end. C99, fixed workspace, no malloc/float.
 * No labels, network, ROI, or neighbouring-pixel evidence in this core.
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OOF_VERSION "1.0.0"
#define OOF_POST_CAP 32
#define OOF_NO_CAPTURE UINT16_MAX
#define OOF_FLAG_OFFGRID 1u
#define OOF_FLAG_TIME_OVERFLOW 2u
#define OOF_FLAG_NO_POST_EVENTS 4u
#define OOF_OK 0
#define OOF_ERR_CONFIG (-1)
#define OOF_ERR_ORDER (-2)
#define OOF_ERR_ROW (-3)

/* All times are in microseconds. Frames are fixed at 4+4+8=16 slots. */
typedef struct {
    uint16_t width, height, capture_capacity, reserved;
    uint32_t bit_us, tolerance_us, refractory_us, min_gap_us;
} OofConfig;

/* Times are relative to the FIRST header pulse, NOT to post-header slot 4.
 * dt_us[0:n_events] retains every refractory-accepted ON event in the
 * post-header window, including off-grid ones. word sets only on-grid bits.
 * Window: [t0+4*Tb-tol, t0+15*Tb+tol), a half-open interval.
 * Padding dt_us[n_events:32] is zero; always use n_events.
 * t0 is a modulo-2^32 timestamp in the same clock as the input events.
 */
typedef struct {
    uint32_t t0_us;
    uint16_t x, y, word, flags;
    uint16_t header_last_error_us, max_post_error_us, offgrid_events, n_events;
    uint16_t dt_us[OOF_POST_CAP];
} OofSample;

typedef struct {
    uint64_t rows, on_events, off_events, other_events, invalid_xy;
    uint64_t debounce_rejected, cold_first_on, run_starts, run_breaks;
    uint64_t header_locks, pool_exhausted, samples, crc_ok, crc_bad;
    uint64_t offgrid_events, time_overflows, samples_no_post, order_errors;
    uint64_t invalid_rows, max_stream_gap_us;
    uint32_t active, peak_active, last_event_us, reserved;
} OofStats;

typedef struct OofReceiver OofReceiver;
/* Callback runs synchronously; copy the sample if it must survive return.
 * Must not reenter the receiver. Must not retain s or the input buffer.
 */
typedef void (*OofEmit)(void *user, const OofSample *s);

void oof_default_config(OofConfig *cfg);
size_t oof_workspace_bytes(const OofConfig *cfg);
size_t oof_pixel_state_bytes(void);
size_t oof_capture_state_bytes(void);
/* workspace must have at least 8-byte alignment, e.g. malloc or uint64_t[]. */
OofReceiver *oof_init(void *workspace, size_t bytes, const OofConfig *cfg,
                      OofEmit emit, void *user);
void oof_reset(OofReceiver *r);
/* Ordered timestamps modulo 2^32; consecutive input gaps must be <2^31 us.
 * Invalid coordinate rows are counted, not decoded. Non-ON rows advance time.
 */
int oof_feed_event(OofReceiver *r, uint32_t t_us, uint16_t x,
                   uint16_t y, uint16_t type);
/* Zero-copy traversal of little-endian [type,sec,ms,us,x,y] uint16 rows.
 * n is VALID ROWS, never full capacity unless every row is valid.
 * On error rows_done identifies the prefix consumed; caller must not retry
 * that prefix. Timestamp components ms/us must both be <1000.
 */
int oof_feed_rows_le(OofReceiver *r, const void *data, size_t n,
                     size_t *rows_done);
/* Only call with a proven sensor-stream watermark, not time.ticks_us().
 * All events with t < watermark must already have been consumed.
 * No force-flush: EOF partial frames remain active and excluded.
 */
int oof_advance_watermark(OofReceiver *r, uint32_t watermark_us);
const OofStats *oof_stats(const OofReceiver *r);
uint8_t oof_crc4(uint8_t payload);
#ifdef __cplusplus
}
#endif
#endif
