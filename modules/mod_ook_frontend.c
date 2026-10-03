/* Optional MicroPython firmware binding. No Python calls/allocations in feed.
 * Buffers allocated once by Receiver(); .pop() allocates result tuples.
 * Add to OpenMV USER_C_MODULES; this is NOT a dynamically loaded desktop DLL.
 */
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objtuple.h"
#include "ook_frontend.h"
#include <string.h>

typedef struct {
    mp_obj_base_t base;
    uint64_t *workspace;
    size_t workspace_bytes;
    OofReceiver *receiver;
    OofSample *out;
    uint16_t cap, head, tail, count;
    uint64_t dropped;
} receiver_obj_t;

static void receive_sample(void *user, const OofSample *s) {
    receiver_obj_t *o=(receiver_obj_t *)user;
    if (o->count==o->cap) { o->dropped++; return; }
    o->out[o->head]=*s;
    o->head=(uint16_t)((o->head+1u)%o->cap);o->count++;
}
static mp_obj_t receiver_make_new(const mp_obj_type_t *type, size_t n_args,
                                 size_t n_kw, const mp_obj_t *args) {
    enum {ARG_width,ARG_height,ARG_pool,ARG_queue,ARG_bit_us,ARG_tol_us,ARG_refr_us,ARG_gap_bits};
    static const mp_arg_t allowed[]={
        {MP_QSTR_width,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=320}},
        {MP_QSTR_height,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=320}},
        {MP_QSTR_pool,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=512}},
        {MP_QSTR_queue,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=512}},
        {MP_QSTR_bit_us,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=1956}},
        {MP_QSTR_tol_us,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=489}},
        {MP_QSTR_refr_us,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=489}},
        {MP_QSTR_gap_bits,MP_ARG_KW_ONLY|MP_ARG_INT,{.u_int=8}},
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed)];
    OofConfig c;
    mp_arg_parse_all_kw_array(n_args,n_kw,args,MP_ARRAY_SIZE(allowed),allowed,v);
    for (size_t i=0;i<MP_ARRAY_SIZE(allowed);i++) {
        if (v[i].u_int<0 || v[i].u_int>65534)
            mp_raise_ValueError(MP_ERROR_TEXT("configuration value out of range"));
    }
    if (!v[ARG_queue].u_int || !v[ARG_gap_bits].u_int)
        mp_raise_ValueError(MP_ERROR_TEXT("queue and gap_bits must be positive"));
    oof_default_config(&c);
    c.width=v[ARG_width].u_int;c.height=v[ARG_height].u_int;
    c.capture_capacity=v[ARG_pool].u_int;c.bit_us=v[ARG_bit_us].u_int;
    c.tolerance_us=v[ARG_tol_us].u_int;c.refractory_us=v[ARG_refr_us].u_int;
    c.min_gap_us=(uint32_t)v[ARG_gap_bits].u_int*c.bit_us;
    size_t bytes=oof_workspace_bytes(&c);
    if (!bytes) mp_raise_ValueError(MP_ERROR_TEXT("invalid receiver config"));
    receiver_obj_t *o=mp_obj_malloc(receiver_obj_t,type);
    o->workspace=NULL;o->out=NULL;o->receiver=NULL;
    o->cap=v[ARG_queue].u_int;o->head=o->tail=o->count=0;o->dropped=0;
    o->workspace_bytes=bytes;
    o->workspace=m_new(uint64_t,(bytes+7u)/8u);
    o->out=m_new(OofSample,o->cap);
    o->receiver=oof_init(o->workspace,bytes,&c,receive_sample,o);
    if (!o->receiver) mp_raise_ValueError(MP_ERROR_TEXT("workspace alignment/init failed"));
    return MP_OBJ_FROM_PTR(o);
}
static mp_obj_t receiver_feed(mp_obj_t self,mp_obj_t data,mp_obj_t nobj) {
    receiver_obj_t *o=MP_OBJ_TO_PTR(self);
    mp_int_t n=mp_obj_get_int(nobj);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data,&b,MP_BUFFER_READ);
    if (n<0 || (size_t)n>b.len/12u)
        mp_raise_ValueError(MP_ERROR_TEXT("valid row count exceeds buffer length"));
    uint64_t before=oof_stats(o->receiver)->samples;
    size_t done=0;
    int rc=oof_feed_rows_le(o->receiver,b.buf,(size_t)n,&done);
    if (rc!=OOF_OK)
        mp_raise_msg_varg(&mp_type_ValueError,MP_ERROR_TEXT("front-end rc=%d after %u rows"),rc,(unsigned)done);
    return mp_obj_new_int_from_ull(oof_stats(o->receiver)->samples-before);
}
static MP_DEFINE_CONST_FUN_OBJ_3(receiver_feed_obj,receiver_feed);

static mp_obj_t receiver_pop(mp_obj_t self) {
    receiver_obj_t *o=MP_OBJ_TO_PTR(self);
    if (!o->count) return mp_const_none;
    const OofSample *s=&o->out[o->tail];
    mp_obj_t ts[OOF_POST_CAP];
    for (uint16_t i=0;i<s->n_events;i++) ts[i]=mp_obj_new_int(s->dt_us[i]);
    mp_obj_t p[9]={mp_obj_new_int_from_uint(s->t0_us),mp_obj_new_int(s->x),mp_obj_new_int(s->y),
        mp_obj_new_int(s->word),mp_obj_new_int(s->flags),mp_obj_new_int(s->header_last_error_us),
        mp_obj_new_int(s->max_post_error_us),mp_obj_new_int(s->offgrid_events),
        mp_obj_new_tuple(s->n_events,ts)};
    mp_obj_t result=mp_obj_new_tuple(9,p);
    o->tail=(uint16_t)((o->tail+1u)%o->cap);o->count--;
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(receiver_pop_obj,receiver_pop);
static mp_obj_t receiver_reset(mp_obj_t self) {
    receiver_obj_t *o=MP_OBJ_TO_PTR(self);
    oof_reset(o->receiver);o->head=o->tail=o->count=0;o->dropped=0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(receiver_reset_obj,receiver_reset);
static mp_obj_t receiver_stats(mp_obj_t self) {
    receiver_obj_t *o=MP_OBJ_TO_PTR(self);
    const OofStats *s=oof_stats(o->receiver);
    mp_obj_t d=mp_obj_new_dict(0);
#define PUT(name,value) mp_obj_dict_store(d,MP_OBJ_NEW_QSTR(MP_QSTR_##name),mp_obj_new_int_from_ull(value))
#define S(name) PUT(name,s->name)
    S(rows);S(on_events);S(off_events);S(other_events);S(invalid_xy);
    S(debounce_rejected);S(cold_first_on);S(run_starts);S(run_breaks);
    S(header_locks);S(pool_exhausted);S(samples);S(crc_ok);S(crc_bad);
    S(offgrid_events);S(time_overflows);S(samples_no_post);S(order_errors);
    S(invalid_rows);S(max_stream_gap_us);S(active);S(peak_active);S(last_event_us);
    PUT(results_pending,o->count);PUT(results_dropped,o->dropped);
    PUT(workspace_bytes,o->workspace_bytes);PUT(queue_bytes,(size_t)o->cap*sizeof(OofSample));
    PUT(pixel_state_bytes,oof_pixel_state_bytes());PUT(capture_state_bytes,oof_capture_state_bytes());
#undef S
#undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(receiver_stats_obj,receiver_stats);
static const mp_rom_map_elem_t receiver_locals_table[]={
    {MP_ROM_QSTR(MP_QSTR_feed),MP_ROM_PTR(&receiver_feed_obj)},
    {MP_ROM_QSTR(MP_QSTR_pop),MP_ROM_PTR(&receiver_pop_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats),MP_ROM_PTR(&receiver_stats_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset),MP_ROM_PTR(&receiver_reset_obj)},
};
static MP_DEFINE_CONST_DICT(receiver_locals,receiver_locals_table);
MP_DEFINE_CONST_OBJ_TYPE(receiver_type,MP_QSTR_Receiver,MP_TYPE_FLAG_NONE,
                        make_new,receiver_make_new,locals_dict,&receiver_locals);
static const mp_rom_map_elem_t module_table[]={
    {MP_ROM_QSTR(MP_QSTR___name__),MP_ROM_QSTR(MP_QSTR_ook_frontend)},
    {MP_ROM_QSTR(MP_QSTR_Receiver),MP_ROM_PTR(&receiver_type)},
};
static MP_DEFINE_CONST_DICT(module_globals,module_table);
const mp_obj_module_t ook_frontend_module={
    .base={&mp_type_module},.globals=(mp_obj_dict_t *)&module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_ook_frontend,ook_frontend_module);
