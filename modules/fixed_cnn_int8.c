/* Dense CPU CNN: INT8 weights, INT16 ReLU activations, INT32 accumulators.
 * No NPU/CMSIS-NN, heap allocation, floating point, or signed right shift.
 * Weight export proves worst-case accumulator bounds for each layer.
 */
#include "fixed_cnn_int8.h"
#include "model_cnn_int8.h"
#include <limits.h>

static int32_t rounded_shift(int32_t value, int shift) {
    if (shift <= 0) return (int32_t)((int64_t)value * (INT64_C(1) << -shift));
    uint32_t magnitude = value < 0 ? 0U - (uint32_t)value : (uint32_t)value;
    magnitude = (magnitude + (UINT32_C(1) << (shift - 1))) >> shift;
    return value < 0 ? -(int32_t)magnitude : (int32_t)magnitude;
}

static int16_t relu(int32_t value, int shift, uint32_t *saturations) {
    value = rounded_shift(value, shift);
    if (value < 0) return 0;
    if (value > INT16_MAX) { ++*saturations; return INT16_MAX; }
    return (int16_t)value;
}

void cnn_run(const uint8_t input[CNN_PACKET_BYTES], cnn_workspace_t *work,
             int32_t scores[6], uint32_t *saturations) {
    *saturations = 0;
    for (int o = 0; o < 8; ++o) {
        for (int t = 0; t < CNN_T1; ++t) {
            int32_t acc = cnn_b1[o];
            for (int c = 0; c < 2; ++c) {
                for (int k = 0; k < 3; ++k) {
                    int ti = 2*t + k - 1;
                    if (ti >= 0 && ti < CNN_INPUT_TICKS)
                        acc += (int32_t)input[ti*2+c] * cnn_w1[(o*2+c)*3+k];
                }
            }
            work->a1[o*CNN_T1+t] = relu(acc, CNN_W1_FRAC - 10, saturations);
        }
    }
    for (int o = 0; o < 16; ++o) {
        for (int t = 0; t < CNN_T2; ++t) {
            int32_t acc = cnn_b2[o];
            for (int c = 0; c < 8; ++c) {
                for (int k = 0; k < 3; ++k) {
                    int ti = 2*t + k - 1;
                    if (ti >= 0 && ti < CNN_T1)
                        acc += (int32_t)work->a1[c*CNN_T1+ti] * cnn_w2[(o*8+c)*3+k];
                }
            }
            work->a2[o*CNN_T2+t] = relu(acc, CNN_W2_FRAC, saturations);
        }
    }
    for (int o = 0; o < 6; ++o) {
        int32_t acc = cnn_b3[o];
        for (int i = 0; i < 16*CNN_T2; ++i)
            acc += (int32_t)work->a2[i] * cnn_w3[o*16*CNN_T2+i];
        scores[o] = rounded_shift(acc, CNN_W3_FRAC);
    }
}

const char *cnn_coefficient_hash(void) { return CNN_COEFFICIENT_SHA256; }
size_t cnn_workspace_bytes(void) { return sizeof(cnn_workspace_t); }
