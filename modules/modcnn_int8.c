#include "py/runtime.h"
#include "py/mphal.h"
#include "fixed_cnn_int8.h"
#include <string.h>

MP_REGISTER_ROOT_POINTER(void *cnn_int8_workspace);
static const uint8_t labels[6] = {1, 15, 85, 170, 240, 255};
static cnn_workspace_t *workspace(void) {
    if (MP_STATE_VM(cnn_int8_workspace) == NULL)
        MP_STATE_VM(cnn_int8_workspace) = m_new(cnn_workspace_t, 1);
    return MP_STATE_VM(cnn_int8_workspace);
}
static void put(mp_obj_t d, qstr k, mp_obj_t v) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), v);
}
static void number(mp_obj_t d, qstr k, uint32_t v) { put(d, k, mp_obj_new_int_from_uint(v)); }
static mp_obj_t cnn_info(void) {
    mp_obj_t d = mp_obj_new_dict(0), classes[6];
    for (size_t i = 0; i < 6; ++i) classes[i] = mp_obj_new_int(labels[i]);
    put(d, MP_QSTR_classes, mp_obj_new_tuple(6, classes));
    put(d, MP_QSTR_version, mp_obj_new_str("1.0.0", 5));
    put(d, MP_QSTR_coefficient_sha256, mp_obj_new_str(cnn_coefficient_hash(), 64));
    put(d, MP_QSTR_compiler, mp_obj_new_str(__VERSION__, strlen(__VERSION__)));
    number(d, MP_QSTR_weight_bits, 8);
    number(d, MP_QSTR_activation_bits, 16);
    number(d, MP_QSTR_accumulator_bits, 32);
    number(d, MP_QSTR_state_bytes, cnn_workspace_bytes());
    number(d, MP_QSTR_packet_bytes, CNN_PACKET_BYTES);
    number(d, MP_QSTR_input_ticks, CNN_INPUT_TICKS);
    number(d, MP_QSTR_tail_ticks, 64);
    number(d, MP_QSTR_channels, 2);
    number(d, MP_QSTR_macs_per_packet, CNN_MACS);
    number(d, MP_QSTR_weight_bytes, 6096);
    number(d, MP_QSTR_bias_bytes, 120);
    put(d, MP_QSTR_reset_per_packet, mp_const_true);
    put(d, MP_QSTR_uses_npu, mp_const_false);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(cnn_info_obj, cnn_info);
static mp_obj_t cnn_infer(mp_obj_t input) {
    mp_buffer_info_t b;
    mp_get_buffer_raise(input, &b, MP_BUFFER_READ);
    if (b.len != CNN_PACKET_BYTES)
        mp_raise_ValueError(MP_ERROR_TEXT("packet must contain 299 binary ON/OFF pairs"));
    const uint8_t *p = b.buf;
    for (size_t i = 0; i < b.len; ++i) {
        if (p[i] > 1) mp_raise_ValueError(MP_ERROR_TEXT("input must be binary"));
        if (i >= CNN_INPUT_TICKS*2 && p[i])
            mp_raise_ValueError(MP_ERROR_TEXT("64 tail ticks must be zero"));
    }
    cnn_workspace_t *w = workspace();
    int32_t scores[6]; uint32_t saturations;
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    cnn_run(p, w, scores, &saturations);
    uint32_t elapsed = (uint32_t)mp_hal_ticks_us() - start;
    size_t best = 0, ties = 0; mp_obj_t values[6];
    for (size_t i = 0; i < 6; ++i) {
        if (scores[i] > scores[best]) best = i;
        values[i] = mp_obj_new_int(scores[i]);
    }
    for (size_t i = 0; i < 6; ++i) ties += scores[i] == scores[best];
    mp_obj_t d = mp_obj_new_dict(0);
    number(d, MP_QSTR_class_id, best); number(d, MP_QSTR_payload, labels[best]);
    number(d, MP_QSTR_elapsed_us, elapsed); number(d, MP_QSTR_saturations, saturations);
    put(d, MP_QSTR_score_tie, mp_obj_new_bool(ties > 1));
    put(d, MP_QSTR_scores, mp_obj_new_tuple(6, values));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(cnn_infer_obj, cnn_infer);
static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_cnn_int8)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&cnn_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_infer), MP_ROM_PTR(&cnn_infer_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t cnn_int8_module = {
    .base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR_cnn_int8, cnn_int8_module);
