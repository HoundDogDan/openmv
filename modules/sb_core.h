#ifndef SB_CORE_H
#define SB_CORE_H
#include <stdint.h>
#define SB_CHANNELS 32
#define SB_DEPTH 32
#define SB_EDGES 1024
// Synthetic replay only. Keep a power of two for fast wrap-around.
// 1024 frames use 4 KiB, saving 12 KiB over the initial release.
#define SB_INPUTS 1024
#define SB_MAX_FEED_FRAMES 4096
#if SB_INPUTS < 1 || (SB_INPUTS & (SB_INPUTS - 1)) != 0
#error "SB_INPUTS must be a positive power of two"
#endif
#define SB_MAX_STEPS 1000000

typedef struct { uint8_t src, dst, delay; int16_t weight; } sb_edge;
typedef struct {
    uint16_t sources, neurons, edges, threshold;
    uint8_t leak_shift;
    uint16_t start[SB_CHANNELS + 1];
    sb_edge edge[SB_EDGES];
} sb_graph;
typedef struct {
    uint32_t steps, input_spikes, edge_checks, synapse_adds;
    uint32_t neuron_visits, neuron_updates, output_spikes;
} sb_stats;
typedef struct {
    int32_t pending[SB_DEPTH][SB_CHANNELS];
    uint32_t due[SB_DEPTH], active, tick;
    int32_t voltage[SB_CHANNELS];
    sb_stats stats;
} sb_state;
typedef enum { SB_SHIFT, SB_MUL2, SB_RING, SB_OP_SHIFT, SB_OP_MUL,
               SB_DENSE, SB_SPARSE } sb_mode;

extern uint32_t sb_input[SB_INPUTS];
extern sb_graph sb_config;
extern sb_state sb_network;
extern uint32_t sb_history[SB_CHANNELS];
extern uint32_t sb_scalar, sb_next;

void sb_prepare(unsigned rate_per_mille, uint32_t seed);
void sb_reset(void);
void sb_sort_graph(sb_graph *g);
uint32_t sb_step(sb_state *s, const sb_graph *g, uint32_t mask, int sparse);
void sb_run(sb_mode mode, uint32_t steps);
uint32_t sb_checksum(sb_mode mode);
int sb_explicit_ops(void);
uint32_t sb_valid_mask(unsigned count);
#endif
