#include "py/runtime.h"
#include "py/mphal.h"
#include "sb_core.h"
#include <string.h>

static bool graph_ready, input_ready;
static unsigned input_rate;
static uint32_t input_seed;
static volatile uint32_t result_sink;
static const char *mode_names[] = {"shift", "mul2", "ring", "op_shift", "op_mul", "dense", "sparse"};

static void put(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}
static void number(mp_obj_t d, qstr key, uint32_t value) {
    put(d, key, mp_obj_new_int_from_uint(value));
}
static mp_int_t bounded(mp_obj_t obj, mp_int_t low, mp_int_t high) {
    mp_int_t x = mp_obj_get_int(obj);
    if (x < low || x > high) mp_raise_ValueError(MP_ERROR_TEXT("integer argument out of range"));
    return x;
}
static sb_mode parse_mode(mp_obj_t value) {
    const char *s = mp_obj_str_get_str(value);
    for (unsigned i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); ++i)
        if (!strcmp(s, mode_names[i])) return (sb_mode)i;
    mp_raise_ValueError(MP_ERROR_TEXT("unknown benchmark mode"));
}
static void need_graph(void) {
    if (!graph_ready) mp_raise_ValueError(MP_ERROR_TEXT("call configure() first"));
}
static mp_obj_t stats_dict(uint32_t elapsed) {
    mp_obj_t d = mp_obj_new_dict(0);
    number(d, MP_QSTR_elapsed_us, elapsed);
    number(d, MP_QSTR_input_spikes, sb_network.stats.input_spikes);
    number(d, MP_QSTR_edge_checks, sb_network.stats.edge_checks);
    number(d, MP_QSTR_synapse_adds, sb_network.stats.synapse_adds);
    number(d, MP_QSTR_neuron_visits, sb_network.stats.neuron_visits);
    number(d, MP_QSTR_neuron_updates, sb_network.stats.neuron_updates);
    number(d, MP_QSTR_output_spikes, sb_network.stats.output_spikes);
    number(d, MP_QSTR_tick, sb_network.tick);
    return d;
}
static mp_obj_t sb_info(void) {
    mp_obj_t d = mp_obj_new_dict(0);
    put(d, MP_QSTR_version, mp_obj_new_str("0.1.1", 5));
    put(d, MP_QSTR_compiler, mp_obj_new_str(__VERSION__, strlen(__VERSION__)));
    put(d, MP_QSTR_explicit_ops, mp_obj_new_bool(sb_explicit_ops()));
    put(d, MP_QSTR_graph_ready, mp_obj_new_bool(graph_ready));
    put(d, MP_QSTR_input_ready, mp_obj_new_bool(input_ready));
    number(d, MP_QSTR_max_sources, SB_CHANNELS);
    number(d, MP_QSTR_max_neurons, SB_CHANNELS);
    number(d, MP_QSTR_max_edges, SB_EDGES);
    number(d, MP_QSTR_delay_slots, SB_DEPTH);
    number(d, MP_QSTR_input_frames, SB_INPUTS);
    number(d, MP_QSTR_max_feed_frames, SB_MAX_FEED_FRAMES);
    number(d, MP_QSTR_max_steps, SB_MAX_STEPS);
    number(d, MP_QSTR_core_static_bytes, sizeof(sb_config) + sizeof(sb_network) + sizeof(sb_input) + sizeof(sb_history) + 8);
    number(d, MP_QSTR_sources, sb_config.sources);
    number(d, MP_QSTR_neurons, sb_config.neurons);
    number(d, MP_QSTR_edges, sb_config.edges);
    number(d, MP_QSTR_threshold, sb_config.threshold);
    number(d, MP_QSTR_leak_shift, sb_config.leak_shift);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(sb_info_obj, sb_info);

static mp_obj_t sb_configure(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
    enum { A_sources, A_neurons, A_edges, A_threshold, A_leak_shift };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_sources, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_neurons, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_edges, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_threshold, MP_ARG_INT, {.u_int = 1024}},
        {MP_QSTR_leak_shift, MP_ARG_INT, {.u_int = 3}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[A_sources].u_int < 1 || a[A_sources].u_int > 32 ||
        a[A_neurons].u_int < 1 || a[A_neurons].u_int > 32 ||
        a[A_threshold].u_int < 1 || a[A_threshold].u_int > 32767 ||
        a[A_leak_shift].u_int < 1 || a[A_leak_shift].u_int > 15)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid graph dimensions, threshold or leak"));
    size_t count; mp_obj_t *items;
    mp_obj_get_array(a[A_edges].u_obj, &count, &items);
    if (count > SB_EDGES) mp_raise_ValueError(MP_ERROR_TEXT("maximum 1024 edges"));
    // Candidate lives in GC memory, not the MCU stack. Only commit after validation.
    sb_graph *g = m_new_obj(sb_graph);
    memset(g, 0, sizeof(*g));
    g->sources = a[A_sources].u_int; g->neurons = a[A_neurons].u_int;
    g->threshold = a[A_threshold].u_int; g->leak_shift = a[A_leak_shift].u_int;
    g->edges = count;
    for (size_t i = 0; i < count; ++i) {
        size_t fields; mp_obj_t *e;
        mp_obj_get_array(items[i], &fields, &e);
        if (fields != 4) mp_raise_ValueError(MP_ERROR_TEXT("edge must be (source, destination, weight, delay)"));
        g->edge[i].src = bounded(e[0], 0, g->sources - 1);
        g->edge[i].dst = bounded(e[1], 0, g->neurons - 1);
        g->edge[i].weight = bounded(e[2], -32768, 32767);
        g->edge[i].delay = bounded(e[3], 1, 31);
    }
    sb_sort_graph(g);
    memcpy(&sb_config, g, sizeof(*g));
    m_del_obj(sb_graph, g);
    graph_ready = true; sb_reset();
    return sb_info();
}
static MP_DEFINE_CONST_FUN_OBJ_KW(sb_configure_obj, 3, sb_configure);

static mp_obj_t sb_prepare_py(size_t n_args, const mp_obj_t *args) {
    input_rate = bounded(args[0], 0, 1000);
    input_seed = n_args > 1 ? (uint32_t)mp_obj_get_int_truncated(args[1]) : 1;
    sb_prepare(input_rate, input_seed); input_ready = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sb_prepare_obj, 1, 2, sb_prepare_py);
static mp_obj_t sb_reset_py(void) { sb_reset(); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(sb_reset_obj, sb_reset_py);

static mp_obj_t sb_run_py(mp_obj_t mode_obj, mp_obj_t steps_obj) {
    sb_mode mode = parse_mode(mode_obj);
    uint32_t steps = bounded(steps_obj, 1, SB_MAX_STEPS);
    if (!input_ready) mp_raise_ValueError(MP_ERROR_TEXT("call prepare() first"));
    if (mode >= SB_DENSE) need_graph();
    if ((mode == SB_OP_SHIFT || mode == SB_OP_MUL) && !sb_explicit_ops())
        mp_raise_ValueError(MP_ERROR_TEXT("explicit opcodes unsupported by this build"));
    sb_reset(); // reset excluded from internal time; each trial repeats same initial state
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    sb_run(mode, steps);
    uint32_t elapsed = (uint32_t)mp_hal_ticks_us() - start;
    result_sink = sb_checksum(mode); // checksum and dict allocation excluded
    mp_obj_t d = stats_dict(elapsed);
    put(d, MP_QSTR_mode, mode_obj);
    number(d, MP_QSTR_steps, steps);
    number(d, MP_QSTR_checksum, result_sink);
    number(d, MP_QSTR_rate_per_mille, input_rate);
    number(d, MP_QSTR_seed, input_seed);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sb_run_obj, sb_run_py);

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write_le32(uint8_t *p, uint32_t x) {
    p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24;
}
static mp_obj_t sb_feed(size_t n_args, const mp_obj_t *args) {
    need_graph();
    mp_buffer_info_t in, out = {0};
    mp_get_buffer_raise(args[0], &in, MP_BUFFER_READ);
    if (in.len % 4 || in.len > 4 * SB_MAX_FEED_FRAMES)
        mp_raise_ValueError(MP_ERROR_TEXT("input must be <=4096 little-endian uint32 masks"));
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_get_buffer_raise(args[1], &out, MP_BUFFER_WRITE);
        if (out.len != in.len) mp_raise_ValueError(MP_ERROR_TEXT("output buffer length must equal input"));
        uintptr_t x = (uintptr_t)in.buf, y = (uintptr_t)out.buf;
        if (x != y && x < y + out.len && y < x + in.len)
            mp_raise_ValueError(MP_ERROR_TEXT("partially overlapping buffers"));
    }
    bool sparse = n_args < 3 || mp_obj_is_true(args[2]);
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    for (size_t i = 0; i < in.len; i += 4) {
        uint32_t fired = sb_step(&sb_network, &sb_config, read_le32((uint8_t *)in.buf + i), sparse);
        if (out.buf) write_le32((uint8_t *)out.buf + i, fired);
    }
    uint32_t elapsed = (uint32_t)mp_hal_ticks_us() - start;
    mp_obj_t d = stats_dict(elapsed);
    number(d, MP_QSTR_frames, in.len / 4);
    number(d, MP_QSTR_checksum, sb_checksum(SB_SPARSE));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sb_feed_obj, 1, 3, sb_feed);

static mp_obj_t sb_snapshot(void) {
    need_graph();
    mp_obj_t voltage[SB_CHANNELS];
    for (unsigned i = 0; i < sb_config.neurons; ++i) voltage[i] = mp_obj_new_int(sb_network.voltage[i]);
    mp_obj_t d = stats_dict(0);
    put(d, MP_QSTR_voltage, mp_obj_new_tuple(sb_config.neurons, voltage));
    number(d, MP_QSTR_active_mask, sb_network.active);
    number(d, MP_QSTR_checksum, sb_checksum(SB_SPARSE));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(sb_snapshot_obj, sb_snapshot);

static const mp_rom_map_elem_t globals[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_spikebench)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&sb_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&sb_configure_obj)},
    {MP_ROM_QSTR(MP_QSTR_prepare), MP_ROM_PTR(&sb_prepare_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&sb_reset_obj)},
    {MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&sb_run_obj)},
    {MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&sb_feed_obj)},
    {MP_ROM_QSTR(MP_QSTR_snapshot), MP_ROM_PTR(&sb_snapshot_obj)},
};
static MP_DEFINE_CONST_DICT(globals_dict, globals);
const mp_obj_module_t spikebench_module = {
    .base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals_dict,
};
MP_REGISTER_MODULE(MP_QSTR_spikebench, spikebench_module);
