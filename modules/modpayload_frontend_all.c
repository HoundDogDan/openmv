#include "py/runtime.h"
#include "py/mphal.h"
#include "pfa_core.h"
#include <string.h>

typedef struct {
    mp_obj_base_t base;
    pfa_engine engine;
    uint64_t kernel_us;
    uint32_t last_us, max_us;
} collector_obj;

static void put(mp_obj_t d, qstr k, mp_obj_t value) { mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), value); }
static void num(mp_obj_t d, qstr k, uint64_t value) { put(d, k, mp_obj_new_int_from_ull(value)); }
static mp_obj_t collector_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { CAPACITY, WIDTH, EMPTY };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_capacity, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 64}},
        {MP_QSTR_roi_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 320}},
        {MP_QSTR_reject_empty, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[CAPACITY].u_int < 2 || a[CAPACITY].u_int > PFA_MAX_CAPACITY ||
        a[WIDTH].u_int < 20 || a[WIDTH].u_int > 320 || a[WIDTH].u_int % 2)
        mp_raise_ValueError(MP_ERROR_TEXT("capacity must be 2..64; roi_width even, 20..320"));
    collector_obj *o = mp_obj_malloc(collector_obj, type);
    memset(&o->engine, 0, sizeof(o->engine));
    o->kernel_us = o->last_us = o->max_us = 0;
    /* Store each heap pointer before the next allocation so MicroPython's
     * conservative GC can retain all previously allocated buffers. */
    o->engine.slots = m_new0(pfa_slot, (size_t)a[CAPACITY].u_int);
    o->engine.active_map = m_new0(uint8_t, (size_t)a[WIDTH].u_int * a[WIDTH].u_int);
    if (!pfa_init(&o->engine, o->engine.slots, o->engine.active_map,
                  (unsigned)a[CAPACITY].u_int, (unsigned)a[WIDTH].u_int, a[EMPTY].u_bool))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid collector configuration"));
    return MP_OBJ_FROM_PTR(o);
}
static mp_obj_t collector_feed(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    collector_obj *o = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t src, hdr;
    mp_get_buffer_raise(args[1], &src, MP_BUFFER_READ);
    mp_get_buffer_raise(args[3], &hdr, MP_BUFFER_READ);
    mp_int_t n = mp_obj_get_int(args[2]), found = mp_obj_get_int(args[4]);
    if (n < 0 || found < 0 || (size_t)n > src.len / 12 ||
        (size_t)found > hdr.len / 12 || found > n)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid row or header count"));
    uint32_t start = (uint32_t)mp_hal_ticks_us();
    int status = pfa_feed(&o->engine, src.buf, (size_t)n, hdr.buf, (size_t)found);
    o->last_us = (uint32_t)mp_hal_ticks_us() - start;
    o->kernel_us += o->last_us;
    if (o->last_us > o->max_us) o->max_us = o->last_us;
    if (status == -2) mp_raise_ValueError(MP_ERROR_TEXT("collector is finished; call reset before feeding"));
    if (status < 0) mp_raise_ValueError(MP_ERROR_TEXT("header outputs do not match camera batch or timing contract"));
    return mp_obj_new_bool(status == 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(collector_feed_obj, 5, 5, collector_feed);

static mp_obj_t collector_pop(mp_obj_t self, mp_obj_t destination) {
    collector_obj *o = MP_OBJ_TO_PTR(self);
    mp_buffer_info_t dst;
    mp_get_buffer_raise(destination, &dst, MP_BUFFER_WRITE);
    if (dst.len != PFA_PACKET_BYTES)
        mp_raise_ValueError(MP_ERROR_TEXT("destination must be exactly 598 writable bytes"));
    pfa_meta meta;
    if (!pfa_pop(&o->engine, dst.buf, &meta)) return mp_const_none;
    mp_obj_t out[] = {mp_obj_new_int(meta.x), mp_obj_new_int(meta.y),
        mp_obj_new_int_from_ull(meta.first), mp_obj_new_int(meta.on_bins), mp_obj_new_int(meta.off_bins)};
    return mp_obj_new_tuple(MP_ARRAY_SIZE(out), out);
}
static MP_DEFINE_CONST_FUN_OBJ_2(collector_pop_obj, collector_pop);
static mp_obj_t collector_finish(mp_obj_t self) {
    pfa_finish(&((collector_obj *)MP_OBJ_TO_PTR(self))->engine); return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(collector_finish_obj, collector_finish);
static mp_obj_t collector_reset(mp_obj_t self) {
    collector_obj *o = MP_OBJ_TO_PTR(self);
    pfa_reset(&o->engine); o->kernel_us = o->last_us = o->max_us = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(collector_reset_obj, collector_reset);
static mp_obj_t collector_last(mp_obj_t self) {
    return mp_obj_new_int_from_uint(((collector_obj *)MP_OBJ_TO_PTR(self))->last_us);
}
static MP_DEFINE_CONST_FUN_OBJ_1(collector_last_obj, collector_last);
static mp_obj_t collector_ready(mp_obj_t self) {
    return mp_obj_new_int_from_uint(((collector_obj *)MP_OBJ_TO_PTR(self))->engine.queue_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(collector_ready_obj, collector_ready);

static mp_obj_t collector_stats(mp_obj_t self) {
    collector_obj *o = MP_OBJ_TO_PTR(self); pfa_engine *e = &o->engine;
    mp_obj_t d = mp_obj_new_dict(0);
#define COUNTER(name) num(d, MP_QSTR_##name, e->counters[PFA_##name]);
    PFA_COUNTERS(COUNTER)
#undef COUNTER
#define GLOBAL(name) num(d, MP_QSTR_##name, e->name)
    GLOBAL(rows); GLOBAL(invalid_rows); GLOBAL(headers_seen); GLOBAL(outside_roi_candidates);
    GLOBAL(headers_in_discarded_reset_batches); GLOBAL(timestamp_reset_batches);
    GLOBAL(timestamp_reversals); GLOBAL(global_gaps_over_10ms); GLOBAL(peak_pending); GLOBAL(peak_ready);
#undef GLOBAL
    num(d, MP_QSTR_active_windows, e->pending - e->queue_len);
    num(d, MP_QSTR_unclassified_ready, e->queue_len);
    num(d, MP_QSTR_capacity, e->capacity);
    num(d, MP_QSTR_state_bytes, sizeof(*o) + e->capacity * sizeof(pfa_slot) + e->width * e->width);
    num(d, MP_QSTR_kernel_us, o->kernel_us);
    num(d, MP_QSTR_max_feed_us, o->max_us);
    put(d, MP_QSTR_sensor_first_us, e->have_first ? mp_obj_new_int_from_ull(e->first_seen) : mp_const_none);
    put(d, MP_QSTR_sensor_last_us, e->have_previous ? mp_obj_new_int_from_ull(e->previous) : mp_const_none);
    put(d, MP_QSTR_reject_empty, mp_obj_new_bool(e->reject_empty));
    put(d, MP_QSTR_finished, mp_obj_new_bool(e->finished));
    put(d, MP_QSTR_window_accounting_ok, mp_obj_new_bool(pfa_accounting_ok(e)));
    num(d, MP_QSTR_roi_width, e->width);
    num(d, MP_QSTR_eligible_pixels, e->width * e->width);
    num(d, MP_QSTR_lookup_bytes, e->width * e->width);
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(collector_stats_obj, collector_stats);
static const mp_rom_map_elem_t locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&collector_feed_obj)},
    {MP_ROM_QSTR(MP_QSTR_pop_into), MP_ROM_PTR(&collector_pop_obj)},
    {MP_ROM_QSTR(MP_QSTR_finish), MP_ROM_PTR(&collector_finish_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&collector_reset_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_us), MP_ROM_PTR(&collector_last_obj)},
    {MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&collector_ready_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&collector_stats_obj)},
};
static MP_DEFINE_CONST_DICT(locals, locals_table);
MP_DEFINE_CONST_OBJ_TYPE(pfa_collector_type, MP_QSTR_Collector, MP_TYPE_FLAG_NONE,
    make_new, collector_make, locals_dict, &locals);

static mp_obj_t module_info(void) {
    mp_obj_t d = mp_obj_new_dict(0);
    put(d, MP_QSTR_version, mp_obj_new_str(PFA_VERSION, strlen(PFA_VERSION)));
    num(d, MP_QSTR_tb_us, PFA_TB_US); num(d, MP_QSTR_tick_us, PFA_TICK_US);
    num(d, MP_QSTR_input_ticks, PFA_INPUT_TICKS); num(d, MP_QSTR_tail_ticks, PFA_TAIL_TICKS);
    num(d, MP_QSTR_packet_bytes, PFA_PACKET_BYTES);
    num(d, MP_QSTR_max_eligible_pixels, 320 * 320);
    put(d, MP_QSTR_automatic_pixel_admission, mp_const_true);
    num(d, MP_QSTR_max_capacity, PFA_MAX_CAPACITY); num(d, MP_QSTR_gap_us, PFA_GAP_US);
    num(d, MP_QSTR_slot_bytes, sizeof(pfa_slot)); num(d, MP_QSTR_object_bytes, sizeof(collector_obj));
    put(d, MP_QSTR_compiler, mp_obj_new_str(__VERSION__, strlen(__VERSION__)));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(module_info_obj, module_info);
static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_payload_frontend_all)},
    {MP_ROM_QSTR(MP_QSTR_Collector), MP_ROM_PTR(&pfa_collector_type)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&module_info_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t payload_frontend_all_module = {.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals};
MP_REGISTER_MODULE(MP_QSTR_payload_frontend_all, payload_frontend_all_module);
