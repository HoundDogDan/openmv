/*
 * pay_cnn.c -- MicroPython firmware binding for generated pay_cnn.h.
 *
 * API:
 *   pay_cnn.classify(word, nsrc16) -> (payload, class_idx, margin)
 *       word   : full 16-bit framed OOK word
 *       nsrc16 : 16 raw neighbourhood counts; CNN uses slots CNN_SLOT0...
 *
 *   pay_cnn.run(raw24)       -> (payload, class_idx, margin)
 *       raw24 = [occ,nsrc] for each of CNN_SLOTS slots, slot-major.
 *
 *   pay_cnn.run_q15(q15_24)  -> (payload, class_idx, margin)
 *   pay_cnn.logits()         -> six integer logits from last inference
 *   pay_cnn.classes()
 *   pay_cnn.set_cond(a0,b0,a1,b1)
 *   pay_cnn.get_cond()
 *   pay_cnn.info()
 */

#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objtuple.h"

#include "pay_cnn.h"

#if CNN_C != 2
#error "pay_cnn.c currently expects occ,nsrc (CNN_C == 2)"
#endif

static int32_t cnn_a_q16[CNN_C] = { CNN_A0_Q16, CNN_A1_Q16 };
static int32_t cnn_b[CNN_C]     = { CNN_B0, CNN_B1 };
static cnn_acc_t last_logits[CNN_NCLASS];

static inline int16_t clip_q15(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Raw feature -> Q15. q ~= A*raw - B, A held in Q16. */
static inline int16_t condition(int ch, int32_t raw)
{
    int64_t t = (int64_t)cnn_a_q16[ch] * (int64_t)raw;
    int64_t q = (t >> 16) - (int64_t)cnn_b[ch];
    return clip_q15(q);
}

static mp_obj_t acc_obj(cnn_acc_t v)
{
#if CNN_WBITS == 8
    return mp_obj_new_int((mp_int_t)v);
#else
    return mp_obj_new_int_from_ll((long long)v);
#endif
}

static mp_obj_t result_tuple(void)
{
    int idx = cnn_decide(last_logits);
    cnn_acc_t best = last_logits[idx];
#if CNN_WBITS == 8
    cnn_acc_t second = INT32_MIN;
#else
    cnn_acc_t second = INT64_MIN;
#endif
    for (int i = 0; i < CNN_NCLASS; i++) {
        if (i != idx && last_logits[i] > second) second = last_logits[i];
    }
    cnn_acc_t margin = best - second;

    mp_obj_t out[3] = {
        MP_OBJ_NEW_SMALL_INT(CNN_CLASSES[idx]),
        MP_OBJ_NEW_SMALL_INT(idx),
        acc_obj(margin),
    };
    return mp_obj_new_tuple(3, out);
}

static void run_raw_items(const mp_obj_t *items, size_t n)
{
    const size_t want = (size_t)(CNN_SLOTS * CNN_C);
    if (n != want)
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("expected %d raw ints, got %d"),
                          (int)want, (int)n);

    int16_t x[CNN_SLOTS * CNN_C];
    for (int t = 0; t < CNN_SLOTS; t++) {
        x[t * CNN_C + 0] = condition(0, mp_obj_get_int(items[t * CNN_C + 0]));
        x[t * CNN_C + 1] = condition(1, mp_obj_get_int(items[t * CNN_C + 1]));
    }
    cnn_run_q15(x, last_logits);
}

static mp_obj_t py_run(mp_obj_t seq)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);
    run_raw_items(items, n);
    return result_tuple();
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_run_obj, py_run);

static mp_obj_t py_run_q15(mp_obj_t seq)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);
    const size_t want = (size_t)(CNN_SLOTS * CNN_C);
    if (n != want)
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("expected %d Q15 ints, got %d"),
                          (int)want, (int)n);

    int16_t x[CNN_SLOTS * CNN_C];
    for (size_t i = 0; i < want; i++)
        x[i] = clip_q15(mp_obj_get_int(items[i]));

    cnn_run_q15(x, last_logits);
    return result_tuple();
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_run_q15_obj, py_run_q15);

/* Direct deployment entry point: full word + full 16-slot nsrc list. */
static mp_obj_t py_classify(mp_obj_t word_obj, mp_obj_t nsrc_obj)
{
    mp_int_t word = mp_obj_get_int(word_obj);
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(nsrc_obj, &n, &items);
    if (n < (size_t)(CNN_SLOT0 + CNN_SLOTS))
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("nsrc needs at least %d entries"),
                          CNN_SLOT0 + CNN_SLOTS);

    int16_t x[CNN_SLOTS * CNN_C];
    for (int t = 0; t < CNN_SLOTS; t++) {
        int slot = CNN_SLOT0 + t;
        int occ = ((uint32_t)word >> (15 - slot)) & 1u;
        int nsrc = mp_obj_get_int(items[slot]);
        x[t * CNN_C + 0] = condition(0, occ);
        x[t * CNN_C + 1] = condition(1, nsrc);
    }
    cnn_run_q15(x, last_logits);
    return result_tuple();
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_classify_obj, py_classify);

static mp_obj_t py_logits(void)
{
    mp_obj_t out[CNN_NCLASS];
    for (int i = 0; i < CNN_NCLASS; i++) out[i] = acc_obj(last_logits[i]);
    return mp_obj_new_tuple(CNN_NCLASS, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_logits_obj, py_logits);

static mp_obj_t py_classes(void)
{
    mp_obj_t out[CNN_NCLASS];
    for (int i = 0; i < CNN_NCLASS; i++)
        out[i] = MP_OBJ_NEW_SMALL_INT(CNN_CLASSES[i]);
    return mp_obj_new_tuple(CNN_NCLASS, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_classes_obj, py_classes);

static mp_obj_t py_set_cond(size_t n, const mp_obj_t *a)
{
    if (n != 2 * CNN_C)
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("expected %d args"), 2 * CNN_C);
    for (int c = 0; c < CNN_C; c++) {
        cnn_a_q16[c] = mp_obj_get_int(a[2*c + 0]);
        cnn_b[c]     = mp_obj_get_int(a[2*c + 1]);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_set_cond_obj,
                                           2*CNN_C, 2*CNN_C, py_set_cond);

static mp_obj_t py_get_cond(void)
{
    mp_obj_t ch[CNN_C];
    for (int c = 0; c < CNN_C; c++) {
        mp_obj_t p[2] = { mp_obj_new_int(cnn_a_q16[c]),
                          mp_obj_new_int(cnn_b[c]) };
        ch[c] = mp_obj_new_tuple(2, p);
    }
    return mp_obj_new_tuple(CNN_C, ch);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_get_cond_obj, py_get_cond);

static mp_obj_t py_info(void)
{
    mp_obj_t d = mp_obj_new_dict(12);
#define SET(k, v) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##k), v)
    SET(chan, MP_OBJ_NEW_SMALL_INT(CNN_C));
    SET(slots, MP_OBJ_NEW_SMALL_INT(CNN_SLOTS));
    SET(slot0, MP_OBJ_NEW_SMALL_INT(CNN_SLOT0));
    SET(c1, MP_OBJ_NEW_SMALL_INT(CNN_C1));
    SET(c2, MP_OBJ_NEW_SMALL_INT(CNN_C2));
    SET(kernel, MP_OBJ_NEW_SMALL_INT(CNN_K));
    SET(nclass, MP_OBJ_NEW_SMALL_INT(CNN_NCLASS));
    SET(wbits, MP_OBJ_NEW_SMALL_INT(CNN_WBITS));
    SET(act_bytes, MP_OBJ_NEW_SMALL_INT((CNN_C1*CNN_SLOTS + CNN_C2*CNN_SLOTS)*2));
    SET(weight_bytes, MP_OBJ_NEW_SMALL_INT(
        (int)(sizeof(cnn_w1) + sizeof(cnn_w2) + sizeof(cnn_wf))));
    SET(bias_bytes, MP_OBJ_NEW_SMALL_INT(
        (int)(sizeof(cnn_b1) + sizeof(cnn_b2) + sizeof(cnn_bf))));
    SET(build, MP_OBJ_NEW_QSTR(MP_QSTR_firmware));
#undef SET
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_info_obj, py_info);

static const mp_rom_map_elem_t pay_cnn_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_pay_cnn) },
    { MP_ROM_QSTR(MP_QSTR_classify),  MP_ROM_PTR(&py_classify_obj) },
    { MP_ROM_QSTR(MP_QSTR_run),       MP_ROM_PTR(&py_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_run_q15),  MP_ROM_PTR(&py_run_q15_obj) },
    { MP_ROM_QSTR(MP_QSTR_logits),    MP_ROM_PTR(&py_logits_obj) },
    { MP_ROM_QSTR(MP_QSTR_classes),   MP_ROM_PTR(&py_classes_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_cond),  MP_ROM_PTR(&py_set_cond_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_cond),  MP_ROM_PTR(&py_get_cond_obj) },
    { MP_ROM_QSTR(MP_QSTR_info),      MP_ROM_PTR(&py_info_obj) },
};
static MP_DEFINE_CONST_DICT(pay_cnn_globals, pay_cnn_globals_table);

const mp_obj_module_t pay_cnn_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pay_cnn_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pay_cnn, pay_cnn_user_cmodule);
