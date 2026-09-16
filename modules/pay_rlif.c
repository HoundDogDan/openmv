/*
 * pay_rlif_module.c -- MicroPython binding for the stage-2 recurrent LIF
 * payload decoder exported by ook_stage2_snn.py --export-c.
 *
 * v3: written against the ACTUAL pay_rlif.h, and with NO #warning anywhere,
 *     because the OpenMV build uses -Werror=cpp and a #warning is fatal.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE HEADER ACTUALLY EXPORTS  (verified, not assumed)
 * ---------------------------------------------------------------------------
 *     #define PAY_C      2          occ, nsrc
 *     #define PAY_H1     32
 *     #define PAY_H2     24
 *     #define PAY_NCLASS 6
 *     #define PAY_SLOTS  16
 *     static const uint8_t PAY_CLASSES[6] = {1,15,85,170,240,255};
 *
 *     static void pay_reset(void);
 *     static void pay_step(const int16_t *x);     <-- int16, Q15
 *     static int  pay_decide(void);               <-- returns class INDEX
 *     static int32_t pay_acc[PAY_NCLASS];         <-- visible: margin works
 *
 * There is no pay_readout(). There is no PAY_MU / PAY_SD / PAY_RAW_LO. There
 * is no golden test vector. All three of those absences matter and are dealt
 * with below.
 *
 * ---------------------------------------------------------------------------
 * THE INPUT CONDITIONING -- THE ONLY PART THAT CAN BE SILENTLY WRONG
 * ---------------------------------------------------------------------------
 * On the PC, a raw per-slot feature travels through FOUR transforms before it
 * reaches the network:
 *
 *   1. ook_stage2.py  : per-file normalisation, divide by that file's median
 *                       over occupied slots.  occ -> /1,  nsrc -> /19
 *   2. ook_stage2.py  : clip at 4.0
 *   3. ook_stage2_snn : standardise, (x - mu) / sd, stats from TRAIN only
 *   4. ook_stage2_snn : divide by 4 (sigma), clip to [-1, 1], scale to Q15
 *
 * Steps 1, 3 and 4 are all affine, so the composition is affine and collapses
 * to ONE multiply-add per channel:
 *
 *     q15 = A * raw - B
 *
 *     A = 32767 / (scale * sd * M)      M = per-file median (1 for occ,
 *     B = 32767 * mu / (scale * sd)         19 for nsrc on this dataset)
 *
 * A is held as Q16 so the device does an int32 multiply and a >>16. That is
 * two instructions per channel per slot -- 4 multiplies for a whole 16-slot
 * packet at PAY_C = 2.
 *
 * Get these numbers out of the checkpoint you already trained:
 *
 *     import torch, numpy as np
 *     ck = torch.load("pay_rlif.pt", map_location="cpu", weights_only=False)
 *     mu, sd = np.asarray(ck["mu"]), np.asarray(ck["sd"])
 *     sc = float(ck.get("scale", 4.0))
 *     M  = np.array([1.0, 19.0])            # occ, nsrc  <- per-file medians
 *     A  = 32767.0 / (sc * sd * M)
 *     B  = 32767.0 * mu / (sc * sd)
 *     print("A_Q16", [int(round(a * 65536)) for a in A])
 *     print("B    ", [int(round(b))         for b in B])
 *
 * Paste the two lists into PAY_A_Q16_DEFAULT / PAY_B_DEFAULT below.
 *
 * You do NOT have to rebuild to change them. pay_rlif.set_cond() overwrites
 * them at runtime, so calibration is a Python-side concern and a refocus does
 * not mean a firmware cycle. That is deliberate: the nsrc median moves when
 * the optics move, and it is the one constant here that is a property of the
 * lens rather than of the network.
 *
 * ---------------------------------------------------------------------------
 * THE nsrc MEDIAN IS THE WEAK LINK, AND IT IS NOT A WRAPPER PROBLEM
 * ---------------------------------------------------------------------------
 * M[1] = 19 came from these specific recordings. nsrc scales with blob size,
 * so it changes with distance, focus and LED current. Three ways out, in
 * increasing order of how much you should trust them:
 *
 *   (a) hardcode 19          fine on the bench, breaks silently in the field
 *   (b) running median       track it over recent CRC-OK packets, one update
 *                            per frame; call set_cond() when it moves
 *   (c) retrain on raw nsrc  delete step 1 from the dataset builder, feed
 *                            0..25 directly. Removes the calibration step
 *                            from deployment entirely. Costs one training run
 *                            and is the right answer before you ship.
 *
 * (a) gets you end to end today. Do (c) before believing any field number.
 *
 * ---------------------------------------------------------------------------
 * NO GOLDEN VECTOR IN THIS EXPORT
 * ---------------------------------------------------------------------------
 * pay_rlif.h has no PAY_TEST_N, so selftest() cannot check the arithmetic and
 * returns None. That is a real gap: fixed-point ports fail QUIETLY. A wrong
 * shift moves accuracy a couple of points, which reads as ordinary variation
 * and sends you hunting through the feature extraction instead.
 *
 * Add to --export-c: push ~32 val samples through the numpy integer
 * simulation, emit the CONDITIONED Q15 inputs (post A/B, so the vector tests
 * the network alone) plus the expected class indices:
 *
 *     #define PAY_TEST_N 32
 *     static const int16_t PAY_TEST_X[PAY_TEST_N][PAY_SLOTS][PAY_C] = {...};
 *     static const uint8_t PAY_TEST_Y[PAY_TEST_N] = {...};
 *
 * Then selftest() below starts working with no other change.
 *
 * ---------------------------------------------------------------------------
 * WHERE nsrc COMES FROM ON THE DEVICE -- STILL THE REAL WORK
 * ---------------------------------------------------------------------------
 * `occ` already exists: it is the bit ook_decoder.cpp sets in c->bits.
 * `nsrc` is the number of DISTINCT pixels in the K x K neighbourhood that
 * fired in that slot window, and it is computed nowhere in the C core yet.
 *
 * Without it you are running the occ-only model: 93.7% on crc-bad, not 99.2%.
 * The wrapper will happily run that way with x[1] = 0; it just is not the
 * result.
 *
 * Cheap version, no extra pass over the event stream -- a 25-bit mask per
 * slot per channel, OR-ed during CAPTURE:
 *
 *     uint32_t nb_mask[OOK_MAX_PKT_BITS];        // 64 bytes per channel
 *
 *     int dx = x - c->cx, dy = y - c->cy;        // -2..+2 for feat-win 5
 *     if (dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2)
 *         c->nb_mask[k] |= 1u << ((dy + 2) * 5 + (dx + 2));
 *
 *     nsrc[k] = __builtin_popcount(c->nb_mask[k]);
 *
 * One OR per event, no search, no per-pixel list. Note the channel must
 * OBSERVE events outside its own pixel while still DECODING only its own --
 * that is exactly the --feat-win split ook_stage2.py does, and it is why the
 * hard word stays bit-identical to the cell-1 decode.
 *
 * ---------------------------------------------------------------------------
 * BUILD
 * ---------------------------------------------------------------------------
 *   FIRMWARE USER MODULE   define MODULE_PAY_RLIF_ENABLED=1, add the directory
 *                          to USER_C_MODULES. Always works.
 *
 *   NATIVE .mpy            define MICROPY_ENABLE_DYNRUNTIME, build with
 *                          mpy_ld.py, copy to the SD card. No firmware
 *                          rebuild; iteration is seconds.
 *
 * ---------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------------
 *   pay_rlif.reset()                  clear membranes + accumulator
 *   pay_rlif.step(occ, nsrc)          advance one bit slot, RAW units
 *   pay_rlif.step_q15(q0, q1)         advance one slot, pre-conditioned Q15
 *   pay_rlif.decide()                 -> (payload_byte, class_idx, margin)
 *   pay_rlif.run(buf)                 whole packet: 32 ints, [occ,nsrc]x16
 *   pay_rlif.classes()                -> (1, 15, 85, 170, 240, 255)
 *   pay_rlif.acc()                    -> the six raw accumulators
 *   pay_rlif.set_cond(a0,b0,a1,b1)    override the conditioning at runtime
 *   pay_rlif.get_cond()               -> ((a0,b0), (a1,b1))
 *   pay_rlif.selftest()               -> True/False, or None if not exported
 *   pay_rlif.info()                   -> dict of shapes / build mode
 */

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objtuple.h"

#include "pay_rlif.h"

/* ------------------------------------------------------------------------ */
/* sanity: this wrapper is written for the exact shape of the export        */
/* ------------------------------------------------------------------------ */

#if !defined(PAY_C) || !defined(PAY_H1) || !defined(PAY_H2) || \
    !defined(PAY_NCLASS) || !defined(PAY_SLOTS)
#error "pay_rlif.h is missing PAY_C / PAY_H1 / PAY_H2 / PAY_NCLASS / PAY_SLOTS"
#endif

#if PAY_C != 2
#error "This wrapper's step(occ, nsrc) assumes PAY_C == 2. Re-export with \
--channels occ,nsrc, or widen feed_slot() and py_step()."
#endif

/*
 * Raw feature -> trained Q15 input.
 *
 * Training:
 *
 *   x_norm = raw / M
 *   z      = (x_norm - mu) / sd
 *   x      = clip(z / 4.0, -1, 1)
 *   q15    = round(x * 32768)
 *
 * Folded into:
 *
 *   q15 ~= (A_Q16 * raw >> 16) - B
 *
 * Dataset normalization:
 *   M_occ  = 1
 *   M_nsrc = 19
 *
 * Checkpoint:
 *   mu = [0.5932645, 0.5851593]
 *   sd = [0.49097365, 0.50747746]
 *   scale = 4.0
 */
#ifndef PAY_A_Q16_DEFAULT
#define PAY_A_Q16_DEFAULT { 1093482129L, 55680037L }
#endif

#ifndef PAY_B_DEFAULT
#define PAY_B_DEFAULT { 9899L, 9446L }
#endif

static int32_t pay_a_q16[PAY_C] = PAY_A_Q16_DEFAULT;
static int32_t pay_b[PAY_C]     = PAY_B_DEFAULT;

/* ------------------------------------------------------------------------ */
/* framing state                                                            */
/* ------------------------------------------------------------------------ */
/*
 * Membranes live in static storage inside pay_rlif.h. We only track framing,
 * so a mis-sequenced call is caught in Python rather than quietly producing a
 * plausible wrong answer -- which is the failure mode that costs days.
 */
typedef struct {
    uint8_t started;   /* reset() seen since the last decide() */
    uint8_t nstep;     /* slots fed so far                     */
} pay_ctx_t;

static pay_ctx_t ctx;

static inline int32_t clip_q15(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* raw feature -> Q15 network input */
static inline int16_t condition(int ch, int32_t raw)
{
    int64_t t = (int64_t)pay_a_q16[ch] * (int64_t)raw;
    int32_t q = (int32_t)(t >> 16) - pay_b[ch];
    return (int16_t)clip_q15(q);
}

/* one slot, raw units */
static void feed_slot(int32_t occ, int32_t nsrc)
{
    int16_t x[PAY_C];
    x[0] = condition(0, occ);
    x[1] = condition(1, nsrc);
    pay_step(x);
    if (ctx.nstep < 255) ctx.nstep++;
}

/* ------------------------------------------------------------------------ */
/* reset / step                                                             */
/* ------------------------------------------------------------------------ */

static mp_obj_t py_reset(void)
{
    pay_reset();
    ctx.started = 1;
    ctx.nstep = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_reset_obj, py_reset);

static mp_obj_t py_step(mp_obj_t occ_in, mp_obj_t nsrc_in)
{
    if (!ctx.started)
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("call reset() before step()"));

    feed_slot(mp_obj_get_int(occ_in), mp_obj_get_int(nsrc_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_step_obj, py_step);

/* Pre-conditioned entry point. Use this to replay a golden vector, or when
 * the conditioning is done elsewhere (e.g. folded into the decoder). */
static mp_obj_t py_step_q15(mp_obj_t q0, mp_obj_t q1)
{
    if (!ctx.started)
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("call reset() before step_q15()"));

    int16_t x[PAY_C];
    x[0] = (int16_t)clip_q15(mp_obj_get_int(q0));
    x[1] = (int16_t)clip_q15(mp_obj_get_int(q1));
    pay_step(x);
    if (ctx.nstep < 255) ctx.nstep++;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_step_q15_obj, py_step_q15);

/* ------------------------------------------------------------------------ */
/* decide                                                                   */
/* ------------------------------------------------------------------------ */
/*
 * margin = best - runner_up, straight off pay_acc[]. It is the confidence
 * signal, and it is what gives you a reject option: a packet the network is
 * unsure about can be dropped rather than delivered wrong. Under a Z-channel
 * with a min-drop-1 codebook (0x01 aliases to 0x0F after ONE lost bit) that
 * matters -- ties are not rare, they are the whole reason nsrc is here.
 */
static mp_obj_t py_decide(void)
{
    if (!ctx.started)
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("call reset() before decide()"));
    if (ctx.nstep != PAY_SLOTS)
        mp_raise_msg(&mp_type_ValueError,
                     MP_ERROR_TEXT("wrong number of step() calls for one packet"));

    int idx = pay_decide();

    int32_t best = pay_acc[idx], second = INT32_MIN;
    for (int i = 0; i < PAY_NCLASS; i++)
        if (i != idx && pay_acc[i] > second) second = pay_acc[i];
    int32_t margin = (second == INT32_MIN) ? 0 : (best - second);

    ctx.started = 0;   /* force an explicit reset() before the next frame */

    mp_obj_t t[3] = {
        MP_OBJ_NEW_SMALL_INT(PAY_CLASSES[idx]),
        MP_OBJ_NEW_SMALL_INT(idx),
        mp_obj_new_int(margin),
    };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_decide_obj, py_decide);

/* ------------------------------------------------------------------------ */
/* run -- whole packet in one call                                          */
/* ------------------------------------------------------------------------ */
/*
 * 32 ints laid out [occ0,nsrc0, occ1,nsrc1, ...]. One FFI crossing instead
 * of 17, which on this interpreter is the difference between the Python
 * overhead dominating and the network dominating.
 */
static mp_obj_t py_run(mp_obj_t seq)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);

    if (n != (size_t)(PAY_SLOTS * PAY_C))
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("expected %d ints, got %d"),
                          PAY_SLOTS * PAY_C, (int)n);

    pay_reset();
    ctx.started = 1;
    ctx.nstep = 0;

    for (int k = 0; k < PAY_SLOTS; k++)
        feed_slot(mp_obj_get_int(items[k * PAY_C + 0]),
                  mp_obj_get_int(items[k * PAY_C + 1]));

    return py_decide();
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_run_obj, py_run);

/* ------------------------------------------------------------------------ */
/* introspection                                                            */
/* ------------------------------------------------------------------------ */

static mp_obj_t py_classes(void)
{
    mp_obj_t t[PAY_NCLASS];
    for (int i = 0; i < PAY_NCLASS; i++)
        t[i] = MP_OBJ_NEW_SMALL_INT(PAY_CLASSES[i]);
    return mp_obj_new_tuple(PAY_NCLASS, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_classes_obj, py_classes);

/* Raw accumulators, for calibrating a reject threshold offline. */
static mp_obj_t py_acc(void)
{
    mp_obj_t t[PAY_NCLASS];
    for (int i = 0; i < PAY_NCLASS; i++)
        t[i] = mp_obj_new_int(pay_acc[i]);
    return mp_obj_new_tuple(PAY_NCLASS, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_acc_obj, py_acc);

static mp_obj_t py_set_cond(size_t n, const mp_obj_t *a)
{
    if (n != 2 * PAY_C)
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("expected %d args (a,b per channel)"),
                          2 * PAY_C);
    for (int c = 0; c < PAY_C; c++) {
        pay_a_q16[c] = mp_obj_get_int(a[2 * c + 0]);
        pay_b[c]     = mp_obj_get_int(a[2 * c + 1]);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_set_cond_obj,
                                           2 * PAY_C, 2 * PAY_C, py_set_cond);

static mp_obj_t py_get_cond(void)
{
    mp_obj_t ch[PAY_C];
    for (int c = 0; c < PAY_C; c++) {
        mp_obj_t p[2] = { mp_obj_new_int(pay_a_q16[c]),
                          mp_obj_new_int(pay_b[c]) };
        ch[c] = mp_obj_new_tuple(2, p);
    }
    return mp_obj_new_tuple(PAY_C, ch);
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_get_cond_obj, py_get_cond);

/* ------------------------------------------------------------------------ */
/* selftest                                                                 */
/* ------------------------------------------------------------------------ */
/*
 * Returns None when the export carries no golden vector, which is the case
 * for the current pay_rlif.h. None means "unverified", NOT "passed" -- do not
 * read it as a pass. Re-export with PAY_TEST_N to turn this on.
 *
 * The vector is fed through step_q15(), i.e. already conditioned, so a
 * failure here is unambiguously the network arithmetic and not the A/B
 * constants. That separation is the point of having it.
 */
static mp_obj_t py_selftest(void)
{
#if defined(PAY_TEST_N) && PAY_TEST_N > 0
    int bad = 0;
    for (int s = 0; s < PAY_TEST_N; s++) {
        pay_reset();
        for (int k = 0; k < PAY_SLOTS; k++)
            pay_step(&PAY_TEST_X[s][k][0]);
        if (pay_decide() != (int)PAY_TEST_Y[s]) bad++;
    }
    ctx.started = 0;
    ctx.nstep = 0;
    return bad ? mp_const_false : mp_const_true;
#else
    return mp_const_none;
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_selftest_obj, py_selftest);

static mp_obj_t py_info(void)
{
    mp_obj_t d = mp_obj_new_dict(8);
    #define SET(k, v) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##k), v)

    SET(chan,   MP_OBJ_NEW_SMALL_INT(PAY_C));
    SET(h1,     MP_OBJ_NEW_SMALL_INT(PAY_H1));
    SET(h2,     MP_OBJ_NEW_SMALL_INT(PAY_H2));
    SET(nclass, MP_OBJ_NEW_SMALL_INT(PAY_NCLASS));
    SET(slots,  MP_OBJ_NEW_SMALL_INT(PAY_SLOTS));
    SET(state_bytes, MP_OBJ_NEW_SMALL_INT(
        (int)(sizeof(pay_v1) + sizeof(pay_v2) +
              sizeof(pay_s1) + sizeof(pay_s2) + sizeof(pay_acc))));
#if defined(PAY_TEST_N) && PAY_TEST_N > 0
    SET(golden, MP_OBJ_NEW_SMALL_INT(PAY_TEST_N));
#else
    SET(golden, MP_OBJ_NEW_SMALL_INT(0));
#endif
#ifdef MICROPY_ENABLE_DYNRUNTIME
    SET(build, MP_OBJ_NEW_QSTR(MP_QSTR_mpy));
#else
    SET(build, MP_OBJ_NEW_QSTR(MP_QSTR_firmware));
#endif

    #undef SET
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_info_obj, py_info);

/* ------------------------------------------------------------------------ */
/* module table                                                             */
/* ------------------------------------------------------------------------ */

#if !MICROPY_ENABLE_DYNRUNTIME

static const mp_rom_map_elem_t pay_rlif_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_pay_rlif) },
    { MP_ROM_QSTR(MP_QSTR_reset),     MP_ROM_PTR(&py_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_step),      MP_ROM_PTR(&py_step_obj) },
    { MP_ROM_QSTR(MP_QSTR_step_q15),  MP_ROM_PTR(&py_step_q15_obj) },
    { MP_ROM_QSTR(MP_QSTR_decide),    MP_ROM_PTR(&py_decide_obj) },
    { MP_ROM_QSTR(MP_QSTR_run),       MP_ROM_PTR(&py_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_classes),   MP_ROM_PTR(&py_classes_obj) },
    { MP_ROM_QSTR(MP_QSTR_acc),       MP_ROM_PTR(&py_acc_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_cond),  MP_ROM_PTR(&py_set_cond_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_cond),  MP_ROM_PTR(&py_get_cond_obj) },
    { MP_ROM_QSTR(MP_QSTR_selftest),  MP_ROM_PTR(&py_selftest_obj) },
    { MP_ROM_QSTR(MP_QSTR_info),      MP_ROM_PTR(&py_info_obj) },
};
static MP_DEFINE_CONST_DICT(pay_rlif_globals, pay_rlif_globals_table);

const mp_obj_module_t pay_rlif_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pay_rlif_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pay_rlif, pay_rlif_user_cmodule);

#else /* ---------------- native .mpy ---------------- */

#include "py/dynruntime.h"

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw,
                  mp_obj_t *args)
{
    MP_DYNRUNTIME_INIT_ENTRY

    mp_store_global(MP_QSTR_reset,    MP_OBJ_FROM_PTR(&py_reset_obj));
    mp_store_global(MP_QSTR_step,     MP_OBJ_FROM_PTR(&py_step_obj));
    mp_store_global(MP_QSTR_step_q15, MP_OBJ_FROM_PTR(&py_step_q15_obj));
    mp_store_global(MP_QSTR_decide,   MP_OBJ_FROM_PTR(&py_decide_obj));
    mp_store_global(MP_QSTR_run,      MP_OBJ_FROM_PTR(&py_run_obj));
    mp_store_global(MP_QSTR_classes,  MP_OBJ_FROM_PTR(&py_classes_obj));
    mp_store_global(MP_QSTR_acc,      MP_OBJ_FROM_PTR(&py_acc_obj));
    mp_store_global(MP_QSTR_set_cond, MP_OBJ_FROM_PTR(&py_set_cond_obj));
    mp_store_global(MP_QSTR_get_cond, MP_OBJ_FROM_PTR(&py_get_cond_obj));
    mp_store_global(MP_QSTR_selftest, MP_OBJ_FROM_PTR(&py_selftest_obj));
    mp_store_global(MP_QSTR_info,     MP_OBJ_FROM_PTR(&py_info_obj));

    MP_DYNRUNTIME_INIT_EXIT
}

#endif