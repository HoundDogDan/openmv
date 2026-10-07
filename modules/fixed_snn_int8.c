/* Portable integer reference for the exported, fixed 2-H1-H2-6 network.
 * No dynamic allocation, floating point, or signed negative right shifts.
 * Binary synaptic inputs use conditional integer additions.
 */
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#ifndef GQ_MODEL_HEADER
#define GQ_MODEL_HEADER "model_int8.h"
#endif
#include GQ_MODEL_HEADER
#include "fixed_snn_int8.h"
#if GQ_WEIGHT_BITS != 8 || GQ_H1 != 32 || GQ_H2 != 24 || GQ_STATE_FRAC != 15
#error "This module requires the validated 2-32-24-6 INT8 export with Q15 states"
#endif

typedef struct {
    int32_t m1[GQ_H1], m2[GQ_H2], output[6];
    uint8_t previous[GQ_H1];
} GQ_State;

static int64_t gq_round_shift(int64_t value, int shift) {
    if (shift <= 0) return value * (INT64_C(1) << -shift);
    uint64_t magnitude = (uint64_t)(value < 0 ? -value : value);
    magnitude = (magnitude + (UINT64_C(1) << (shift - 1))) >> shift;
    return value < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
}

static int32_t gq_sat(int64_t value, uint64_t *saturations) {
    if (value > INT32_MAX) { ++*saturations; return INT32_MAX; }
    if (value < INT32_MIN) { ++*saturations; return INT32_MIN; }
    return (int32_t)value;
}

static uint8_t gq_lif(int32_t *mem, int64_t current, uint16_t beta,
                      int32_t threshold, uint64_t *saturations) {
    int64_t old_reset = (*mem > threshold);
    int64_t pre = gq_round_shift((int64_t)beta * *mem, GQ_BETA_FRAC)
                + current - old_reset * threshold;
    uint8_t spike = (pre > threshold);
    *mem = gq_sat(pre - ((int64_t)spike - old_reset) * threshold, saturations);
    return spike;
}

static void gq_step(GQ_State *state, const uint8_t input[2], uint64_t *saturations) {
    uint8_t spike1[GQ_H1], spike2[GQ_H2];
    for (size_t i = 0; i < GQ_H1; ++i) {
        int64_t sum_input = 0, sum_recurrent = 0;
        for (size_t j = 0; j < 2; ++j)
            if (input[j]) sum_input += gq_fc1_w[i * 2 + j];
        for (size_t j = 0; j < GQ_H1; ++j)
            if (state->previous[j]) sum_recurrent += gq_rec1_w[i * GQ_H1 + j];
        int64_t current = gq_round_shift(sum_input, GQ_FC1_FRAC - GQ_STATE_FRAC)
                        + gq_fc1_b[i]
                        + gq_round_shift(sum_recurrent, GQ_REC1_FRAC - GQ_STATE_FRAC);
        spike1[i] = gq_lif(&state->m1[i], current, gq_lif1_beta[i], GQ_LIF1_THRESHOLD, saturations);
    }
    for (size_t i = 0; i < GQ_H2; ++i) {
        int64_t sum = 0;
        for (size_t j = 0; j < GQ_H1; ++j)
            if (spike1[j]) sum += gq_fc2_w[i * GQ_H1 + j];
        int64_t current = gq_round_shift(sum, GQ_FC2_FRAC - GQ_STATE_FRAC) + gq_fc2_b[i];
        spike2[i] = gq_lif(&state->m2[i], current, gq_lif2_beta[i], GQ_LIF2_THRESHOLD, saturations);
    }
    for (size_t i = 0; i < 6; ++i) {
        int64_t sum = 0;
        for (size_t j = 0; j < GQ_H2; ++j)
            if (spike2[j]) sum += gq_readout_w[i * GQ_H2 + j];
        int64_t value = gq_round_shift((int64_t)gq_readout_beta[0] * state->output[i], GQ_BETA_FRAC)
                      + gq_round_shift(sum, GQ_READOUT_FRAC - GQ_STATE_FRAC) + gq_readout_b[i];
        state->output[i] = gq_sat(value, saturations);
    }
    memcpy(state->previous, spike1, sizeof(state->previous));
}

void gq_run(const uint8_t *inputs, size_t examples, size_t steps,
            int32_t *scores, uint64_t *saturations) {
    *saturations = 0;
    for (size_t example = 0; example < examples; ++example) {
        GQ_State state;
        memset(&state, 0, sizeof(state));
        for (size_t tick = 0; tick < steps; ++tick)
            gq_step(&state, inputs + (example * steps + tick) * 2, saturations);
        memcpy(scores + example * 6, state.output, sizeof(state.output));
    }
}

size_t gq_state_bytes(void) { return sizeof(GQ_State); }
