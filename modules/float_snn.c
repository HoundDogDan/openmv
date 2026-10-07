/* FP32 counterpart of the trained 2-32-24-6 recurrent SNN.
 * Fixed accumulation order, no fast-math or fused multiply/add contraction.
 * Binary spikes select weight additions; all 299 time steps are evaluated.
 */
/* Apply the numerical controls before headers and function definitions, even
 * when the surrounding OpenMV build enables fast-math or FMA contraction. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#endif
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include "float_snn.h"
#include "model_fp32.h"
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MIN_EXP != -125 || FLT_MAX_EXP != 128
#error "This kernel requires the IEEE binary32 float format"
#endif
typedef char gf_float_storage_must_be_32_bits[(sizeof(float) * CHAR_BIT == 32) ? 1 : -1];
/* ISO/IEC TS 18661-3 extends FLT_EVAL_METHOD. Method 16 evaluates _Float16
 * at 16 bits but leaves float at its own 32-bit precision. Method 32 likewise
 * preserves binary32 float. Neither means float is evaluated as float16.
 * Reject wider/indeterminate evaluation (including the C99 values 1 and 2). */
#if !defined(FLT_EVAL_METHOD) || (FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != 16 && FLT_EVAL_METHOD != 32)
#define GF_STRINGIFY_INNER(x) #x
#define GF_STRINGIFY(x) GF_STRINGIFY_INNER(x)
#pragma message ("FP32 kernel: FLT_EVAL_METHOD=" GF_STRINGIFY(FLT_EVAL_METHOD))
#error "Unsupported float evaluation precision; use a target mode that preserves binary32 float"
#endif
typedef struct {
    float m1[GF_H1], m2[GF_H2], output[6];
    uint8_t previous[GF_H1];
} GF_State;

static uint8_t lif(float *mem, float current, float beta, float threshold,
                   uint64_t *nonfinite) {
    float old_reset = *mem > threshold ? 1.0f : 0.0f;
    float pre = (beta * *mem + current) - old_reset * threshold;
    uint8_t spike = pre > threshold;
    *mem = pre - ((float)spike - old_reset) * threshold;
    if (!isfinite(*mem)) ++*nonfinite;
    return spike;
}
static void step(GF_State *s, const uint8_t in[2], uint64_t *nonfinite) {
    uint8_t spk1[GF_H1], spk2[GF_H2];
    for (size_t i=0;i<GF_H1;++i) {
        float a=0.0f, b=0.0f;
        for (size_t j=0;j<2;++j) if (in[j]) a += gf_fc1_w[i*2+j];
        for (size_t j=0;j<GF_H1;++j) if (s->previous[j]) b += gf_rec1_w[i*GF_H1+j];
        float current = (a + gf_fc1_b[i]) + b;
        spk1[i] = lif(&s->m1[i],current,gf_lif1_beta[i],GF_LIF1_THRESHOLD,nonfinite);
    }
    for (size_t i=0;i<GF_H2;++i) {
        float a=0.0f;
        for (size_t j=0;j<GF_H1;++j) if (spk1[j]) a += gf_fc2_w[i*GF_H1+j];
        spk2[i] = lif(&s->m2[i],a+gf_fc2_b[i],gf_lif2_beta[i],GF_LIF2_THRESHOLD,nonfinite);
    }
    for (size_t i=0;i<6;++i) {
        float a=0.0f;
        for (size_t j=0;j<GF_H2;++j) if (spk2[j]) a += gf_readout_w[i*GF_H2+j];
        s->output[i] = GF_READOUT_BETA * s->output[i] + (a + gf_readout_b[i]);
        if (!isfinite(s->output[i])) ++*nonfinite;
    }
    memcpy(s->previous,spk1,sizeof(s->previous));
}
void gf_run(const uint8_t *inputs, size_t examples, size_t steps,
            float *scores, uint64_t *nonfinite_states) {
    *nonfinite_states=0;
    for (size_t n=0;n<examples;++n) {
        GF_State state;
        memset(&state,0,sizeof(state));
        for (size_t t=0;t<steps;++t) step(&state,inputs+(n*steps+t)*2,nonfinite_states);
        memcpy(scores+n*6,state.output,sizeof(state.output));
    }
}
size_t gf_state_bytes(void) { return sizeof(GF_State); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
