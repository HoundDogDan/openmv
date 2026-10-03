#include "sb_core.h"
#include <string.h>

uint32_t sb_input[SB_INPUTS];
sb_graph sb_config;
sb_state sb_network;
uint32_t sb_history[SB_CHANNELS];
uint32_t sb_scalar, sb_next;

uint32_t sb_valid_mask(unsigned count) {
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1u;
}
static uint32_t random_word(uint32_t *x) {
    *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
    return *x;
}
void sb_prepare(unsigned rate, uint32_t seed) {
    if (!seed) seed = 1;
    for (unsigned t = 0; t < SB_INPUTS; ++t) {
        uint32_t mask = 0;
        for (unsigned n = 0; n < SB_CHANNELS; ++n)
            if (random_word(&seed) % 1000u < rate) mask |= UINT32_C(1) << n;
        sb_input[t] = mask;
    }
}
void sb_reset(void) {
    memset(&sb_network, 0, sizeof(sb_network));
    memset(sb_history, 0, sizeof(sb_history));
    sb_next = 0;
    sb_scalar = UINT32_C(0x13579bdf);
}
void sb_sort_graph(sb_graph *g) {
    // In-place insertion sort occurs only during configuration, outside timing.
    for (unsigned i = 1; i < g->edges; ++i) {
        sb_edge e = g->edge[i]; unsigned j = i;
        while (j && g->edge[j - 1].src > e.src) {
            g->edge[j] = g->edge[j - 1]; --j;
        }
        g->edge[j] = e;
    }
    unsigned e = 0;
    for (unsigned n = 0; n <= g->sources; ++n) {
        while (e < g->edges && g->edge[e].src < n) ++e;
        g->start[n] = (uint16_t)e;
    }
}
static unsigned take_bit(uint32_t *mask) {
    unsigned n = (unsigned)__builtin_ctz(*mask); // caller guarantees nonzero
    *mask &= *mask - 1u;
    return n;
}
static void schedule(sb_state *s, const sb_edge *e) {
    unsigned slot = (s->tick + e->delay) & 31u;
    s->pending[slot][e->dst] += e->weight;
    s->due[slot] |= UINT32_C(1) << e->dst;
    ++s->stats.synapse_adds;
}
static uint32_t integrate(sb_state *s, const sb_graph *g, unsigned n, unsigned slot) {
    uint32_t bit = UINT32_C(1) << n;
    int32_t v = s->voltage[n], input = s->pending[slot][n];
    s->pending[slot][n] = 0;
    ++s->stats.neuron_visits;
    if (!v && !input) { s->active &= ~bit; return 0; }
    ++s->stats.neuron_updates;
    // Symmetric integer decay with rounding toward zero for the residual.
    // Decay magnitude rounds UP so every quiescent membrane reaches zero.
    int32_t magnitude = v < 0 ? -v : v;
    magnitude -= (magnitude + ((1 << g->leak_shift) - 1)) >> g->leak_shift;
    v = (v < 0 ? -magnitude : magnitude) + input;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    uint32_t fired = v >= g->threshold ? bit : 0;
    if (fired) { v = 0; ++s->stats.output_spikes; }
    s->voltage[n] = v;
    if (v) s->active |= bit; else s->active &= ~bit;
    return fired;
}
uint32_t sb_step(sb_state *s, const sb_graph *g, uint32_t mask, int sparse) {
    mask &= sb_valid_mask(g->sources);
    s->stats.input_spikes += (uint32_t)__builtin_popcount(mask);
    if (sparse) {
        while (mask) {
            unsigned src = take_bit(&mask);
            for (unsigned e = g->start[src]; e < g->start[src + 1]; ++e) {
                ++s->stats.edge_checks;
                schedule(s, &g->edge[e]);
            }
        }
    } else {
        for (unsigned e = 0; e < g->edges; ++e) {
            ++s->stats.edge_checks;
            if (mask & (UINT32_C(1) << g->edge[e].src)) schedule(s, &g->edge[e]);
        }
    }
    unsigned slot = s->tick & 31u;
    uint32_t output = 0;
    if (sparse) {
        uint32_t work = s->active | s->due[slot];
        while (work) output |= integrate(s, g, take_bit(&work), slot);
    } else {
        for (unsigned n = 0; n < g->neurons; ++n) output |= integrate(s, g, n, slot);
    }
    s->due[slot] = 0;
    ++s->tick; ++s->stats.steps;
    return output;
}

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__arm__) || defined(__thumb__) || defined(__aarch64__) || defined(__x86_64__) || defined(__i386__))
#define SB_ASM 1
#else
#define SB_ASM 0
#endif
int sb_explicit_ops(void) { return SB_ASM; }

// Separate loops keep the opcode/mode branch outside measured iterations.
__attribute__((noinline)) static uint32_t op_shift(uint32_t steps) {
    uint32_t x = sb_scalar;
    for (uint32_t t = 0; t < steps; ++t) {
#if defined(__arm__) || defined(__thumb__)
        __asm__ volatile("lsl %0, %0, #1" : "+r"(x) : : "cc");
#elif defined(__aarch64__)
        __asm__ volatile("lsl %w0, %w0, #1" : "+r"(x));
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("shll $1, %0" : "+r"(x) : : "cc");
#else
        x <<= 1; // wrapper refuses to label fallback as an explicit op test
#endif
        x |= sb_input[t & (SB_INPUTS - 1u)] & 1u;
    }
    return x;
}
__attribute__((noinline)) static uint32_t op_mul(uint32_t steps) {
    uint32_t x = sb_scalar;
#if SB_ASM
    const uint32_t two = 2;
#endif
    for (uint32_t t = 0; t < steps; ++t) {
#if defined(__arm__) || defined(__thumb__)
        __asm__ volatile("mul %0, %0, %1" : "+r"(x) : "r"(two) : "cc");
#elif defined(__aarch64__)
        __asm__ volatile("mul %w0, %w0, %w1" : "+r"(x) : "r"(two));
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("imull %1, %0" : "+r"(x) : "r"(two) : "cc");
#else
        x *= 2;
#endif
        x |= sb_input[t & (SB_INPUTS - 1u)] & 1u;
    }
    return x;
}
__attribute__((noinline)) void sb_run(sb_mode mode, uint32_t steps) {
    switch (mode) {
    case SB_OP_SHIFT: sb_scalar = op_shift(steps); break;
    case SB_OP_MUL: sb_scalar = op_mul(steps); break;
    case SB_SHIFT:
        for (uint32_t t = 0; t < steps; ++t)
            for (unsigned n = 0; n < 32; ++n)
                sb_history[n] = (sb_history[n] << 1) | ((sb_input[t & (SB_INPUTS - 1u)] >> n) & 1u);
        break;
    case SB_MUL2:
        for (uint32_t t = 0; t < steps; ++t)
            for (unsigned n = 0; n < 32; ++n)
                sb_history[n] = sb_history[n] * 2u | ((sb_input[t & (SB_INPUTS - 1u)] >> n) & 1u);
        break;
    case SB_RING:
        for (uint32_t t = 0; t < steps; ++t) {
            sb_history[sb_next] = sb_input[t & (SB_INPUTS - 1u)]; sb_next = (sb_next + 1u) & 31u;
        }
        break;
    case SB_DENSE: case SB_SPARSE:
        for (uint32_t t = 0; t < steps; ++t)
            sb_step(&sb_network, &sb_config, sb_input[t & (SB_INPUTS - 1u)], mode == SB_SPARSE);
        break;
    }
}
static uint32_t hash_word(uint32_t h, uint32_t x) { return (h ^ x) * UINT32_C(16777619); }
uint32_t sb_checksum(sb_mode mode) {
    if (mode == SB_OP_SHIFT || mode == SB_OP_MUL) return sb_scalar;
    uint32_t h = UINT32_C(2166136261);
    if (mode == SB_DENSE || mode == SB_SPARSE) {
        for (unsigned n = 0; n < sb_config.neurons; ++n)
            h = hash_word(h, (uint32_t)sb_network.voltage[n]);
        for (unsigned d = 0; d < SB_DEPTH; ++d)
            for (unsigned n = 0; n < sb_config.neurons; ++n)
                h = hash_word(h, (uint32_t)sb_network.pending[(sb_network.tick + d) & 31u][n]);
        h = hash_word(h, sb_network.stats.output_spikes);
    } else {
        for (unsigned n = 0; n < 32; ++n) {
            uint32_t word = sb_history[n];
            if (mode == SB_RING) {
                word = 0;
                for (unsigned age = 0; age < 32; ++age)
                    word |= ((sb_history[(sb_next + 31u - age) & 31u] >> n) & 1u) << age;
            }
            h = hash_word(h, word);
        }
    }
    return h;
}
