#ifndef FLOAT_SNN_API_H
#define FLOAT_SNN_API_H
#include <stdint.h>
#include <stddef.h>
#define SNN_INPUT_TICKS 235
#define SNN_TAIL_TICKS 64
#define SNN_STEPS 299
#define SNN_CHANNELS 2
#define SNN_CLASSES 6
#define SNN_PACKET_BYTES 598
#define SNN_MAX_BENCH_PACKETS 256
void gf_run(const uint8_t *inputs, size_t examples, size_t steps,
            float *scores, uint64_t *nonfinite_states);
size_t gf_state_bytes(void);
#endif
