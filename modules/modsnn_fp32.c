#include "py/runtime.h"
#include "py/mphal.h"
#include "float_snn.h"
#include "model_identity_fp32.h"
#include <string.h>
#if MICROPY_FLOAT_IMPL == MICROPY_FLOAT_IMPL_NONE
#error "snn_fp32 needs MicroPython floating-point support"
#endif

static const uint8_t payloads[SNN_CLASSES] = {0x01, 0x0F, 0x55, 0xAA, 0xF0, 0xFF};

static void put(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}
static void number(mp_obj_t d, qstr key, uint64_t value) {
    put(d, key, mp_obj_new_int_from_ull(value));
}
static const uint8_t *packet(mp_obj_t obj) {
    mp_buffer_info_t view;
    mp_get_buffer_raise(obj, &view, MP_BUFFER_READ);
    if (view.len != SNN_PACKET_BYTES)
        mp_raise_ValueError(MP_ERROR_TEXT("packet must be 598 bytes: 299 ON/OFF pairs"));
    const uint8_t *data = view.buf;
    for (size_t i = 0; i < SNN_PACKET_BYTES; ++i) {
        if (data[i] > 1)
            mp_raise_ValueError(MP_ERROR_TEXT("inputs must be binary 0 or 1"));
        if (i >= SNN_INPUT_TICKS * SNN_CHANNELS && data[i] != 0)
            mp_raise_ValueError(MP_ERROR_TEXT("last 64 input ticks must be zero"));
    }
    return data;
}
static mp_obj_t result(const float scores[SNN_CLASSES], uint64_t nonfinite,
                        uint32_t elapsed) {
    if (nonfinite) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("FP32 produced nonfinite state"));
    size_t winner = 0;
    mp_obj_t items[SNN_CLASSES];
    for (size_t i = 0; i < SNN_CLASSES; ++i) {
        if (scores[i] > scores[winner]) winner = i;
        items[i] = mp_obj_new_float(scores[i]);
    }
    size_t ties = 0;
    for (size_t i = 0; i < SNN_CLASSES; ++i) ties += scores[i] == scores[winner];
    mp_obj_t d = mp_obj_new_dict(0);
    number(d, MP_QSTR_class_id, winner);
    number(d, MP_QSTR_payload, payloads[winner]);
    number(d, MP_QSTR_elapsed_us, elapsed);
    number(d, MP_QSTR_saturations, 0);
    number(d, MP_QSTR_nonfinite_states, nonfinite);
    put(d, MP_QSTR_score_tie, mp_obj_new_bool(ties > 1));
    put(d, MP_QSTR_scores, mp_obj_new_tuple(SNN_CLASSES, items));
    return d;
}
static mp_obj_t snn_info(void) {
    mp_obj_t d = mp_obj_new_dict(0);
    put(d, MP_QSTR_version, mp_obj_new_str("0.1.1", 5));
    put(d, MP_QSTR_header_sha256, mp_obj_new_str(SNN_FP32_HEADER_SHA256, 64));
    put(d, MP_QSTR_source_checkpoint_sha256, mp_obj_new_str(SNN_FP32_SOURCE_SHA256, 64));
    put(d, MP_QSTR_compiler, mp_obj_new_str(__VERSION__, strlen(__VERSION__)));
    put(d, MP_QSTR_arithmetic, mp_obj_new_str("fp32", 4));
    number(d, MP_QSTR_weight_bits, 32);
    number(d, MP_QSTR_state_bits, 32);
    put(d, MP_QSTR_state_frac, mp_const_none);
    number(d, MP_QSTR_intermediate_bits, 32);
    number(d, MP_QSTR_state_bytes, gf_state_bytes());
    number(d, MP_QSTR_weight_bytes, 8000);
    number(d, MP_QSTR_coefficient_bytes_estimate, 8484);
    number(d, MP_QSTR_input_ticks, SNN_INPUT_TICKS);
    number(d, MP_QSTR_tail_ticks, SNN_TAIL_TICKS);
    number(d, MP_QSTR_steps, SNN_STEPS);
    number(d, MP_QSTR_tick_us, 100);
    number(d, MP_QSTR_channels, SNN_CHANNELS);
    number(d, MP_QSTR_packet_bytes, SNN_PACKET_BYTES);
    number(d, MP_QSTR_max_bench_packets, SNN_MAX_BENCH_PACKETS);
    mp_obj_t classes[SNN_CLASSES];
    for (size_t i = 0; i < SNN_CLASSES; ++i) classes[i] = mp_obj_new_int(payloads[i]);
    put(d, MP_QSTR_classes, mp_obj_new_tuple(SNN_CLASSES, classes));
    put(d, MP_QSTR_reset_per_packet, mp_const_true);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snn_info_obj, snn_info);

static mp_obj_t snn_infer(mp_obj_t input) {
    const uint8_t *data = packet(input);  /* Buffer checks are outside timing. */
    float scores[SNN_CLASSES];
    uint64_t nonfinite;
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    gf_run(data, 1, SNN_STEPS, scores, &nonfinite);
    uint32_t elapsed = (uint32_t)mp_hal_ticks_us() - start;
    return result(scores, nonfinite, elapsed);
}
static MP_DEFINE_CONST_FUN_OBJ_1(snn_infer_obj, snn_infer);

static mp_obj_t snn_bench(mp_obj_t input, mp_obj_t count) {
    const uint8_t *data = packet(input);
    mp_int_t n = mp_obj_get_int(count);
    if (n < 1 || n > SNN_MAX_BENCH_PACKETS)
        mp_raise_ValueError(MP_ERROR_TEXT("packets must be 1..256"));
    float scores[SNN_CLASSES];
    uint64_t nonfinite = 0;
    uint32_t checksum = UINT32_C(2166136261);
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    for (mp_int_t i = 0; i < n; ++i) {
        /* Memory clobber prevents LTO from hoisting a repeated identical replay. */
        __asm__ volatile ("" : : "r"(data) : "memory");
        uint64_t per_packet;
        gf_run(data, 1, SNN_STEPS, scores, &per_packet);
        nonfinite += per_packet;
        for (size_t j = 0; j < SNN_CLASSES; ++j) {
            uint32_t bits;
            memcpy(&bits, &scores[j], sizeof(bits));
            checksum = (checksum ^ bits) * UINT32_C(16777619);
        }
    }
    uint32_t elapsed = (uint32_t)mp_hal_ticks_us() - start;
    mp_obj_t d = result(scores, nonfinite, elapsed);
    number(d, MP_QSTR_packets, n);
    number(d, MP_QSTR_checksum, checksum);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_2(snn_bench_obj, snn_bench);

static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_snn_fp32)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&snn_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_infer), MP_ROM_PTR(&snn_infer_obj)},
    {MP_ROM_QSTR(MP_QSTR_bench), MP_ROM_PTR(&snn_bench_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t snn_fp32_module = {
    .base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR_snn_fp32, snn_fp32_module);
