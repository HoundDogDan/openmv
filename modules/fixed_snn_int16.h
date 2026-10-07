#ifndef FIXED_SNN_INT16_API_H
#define FIXED_SNN_INT16_API_H
#include <stdint.h>
#include <stddef.h>
#define SNN_INPUT_TICKS 235
#define SNN_TAIL_TICKS 64
#define SNN_STEPS (SNN_INPUT_TICKS + SNN_TAIL_TICKS)
#define SNN_CHANNELS 2
#define SNN_CLASSES 6
#define SNN_PACKET_BYTES (SNN_STEPS * SNN_CHANNELS)
#define SNN_MAX_BENCH_PACKETS 256
void g16_run(const uint8_t *inputs, size_t examples, size_t steps,
            int32_t *scores, uint64_t *saturations);
size_t g16_state_bytes(void);
#endif
