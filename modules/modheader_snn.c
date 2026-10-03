#include "py/runtime.h"
#include "py/mphal.h"
#include "hs_core.h"
#include <string.h>
typedef struct { mp_obj_base_t base; hs_engine engine; uint32_t last_us; } detector_obj;
static void num(mp_obj_t d, qstr k, uint64_t v) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), mp_obj_new_int_from_ull(v));
}
static mp_obj_t detector_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { W, TB, TOL, GAP, REFR, BITS };
    static const mp_arg_t allowed[]={
        {MP_QSTR_width,MP_ARG_INT,{.u_int=160}},
        {MP_QSTR_tb_us,MP_ARG_INT,{.u_int=1956}},
        {MP_QSTR_tol_us,MP_ARG_INT,{.u_int=489}},
        {MP_QSTR_gap_us,MP_ARG_INT,{.u_int=15648}},
        {MP_QSTR_refr_us,MP_ARG_INT,{.u_int=489}},
        {MP_QSTR_frame_bits,MP_ARG_INT,{.u_int=16}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args,n_kw,args,MP_ARRAY_SIZE(allowed),allowed,a);
    int width=a[W].u_int;
    hs_model m={(uint32_t)a[TB].u_int,(uint32_t)a[TOL].u_int,(uint32_t)a[GAP].u_int,
                (uint32_t)a[REFR].u_int,(uint32_t)a[BITS].u_int};
    if (width<20 || width>320 || (width&1) || !hs_valid_model(&m))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid width or temporal model"));
    detector_obj *o=mp_obj_malloc(detector_obj,type);
    o->engine.pixels=NULL; o->last_us=0;
    hs_pixel *pixels=m_new0(hs_pixel,(size_t)width*width);
    hs_init(&o->engine,pixels,width,m);
    return MP_OBJ_FROM_PTR(o);
}
static mp_obj_t detector_feed(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    mp_obj_t self=args[0], data=args[1], count=args[2], output=args[3];
    detector_obj *o=MP_OBJ_TO_PTR(self);
    mp_buffer_info_t src,dst;
    mp_get_buffer_raise(data,&src,MP_BUFFER_READ);
    mp_get_buffer_raise(output,&dst,MP_BUFFER_WRITE);
    mp_int_t n=mp_obj_get_int(count);
    if (n<0 || (size_t)n>src.len/12 || dst.len%12)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid row count or output length"));
    uintptr_t s=(uintptr_t)src.buf,d=(uintptr_t)dst.buf;
    if (s<d+dst.len && d<s+(size_t)n*12)
        mp_raise_ValueError(MP_ERROR_TEXT("input and output must not overlap"));
    uint32_t start=mp_hal_ticks_us();
    size_t found=hs_feed(&o->engine,src.buf,(size_t)n,dst.buf,dst.len/12);
    o->last_us=(uint32_t)mp_hal_ticks_us()-start;
    return mp_obj_new_int_from_uint(found);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(detector_feed_obj,4,4,detector_feed);
static mp_obj_t detector_last_us(mp_obj_t self) { return mp_obj_new_int_from_uint(((detector_obj*)MP_OBJ_TO_PTR(self))->last_us); }
static MP_DEFINE_CONST_FUN_OBJ_1(detector_last_us_obj,detector_last_us);
static mp_obj_t detector_reset(mp_obj_t self) {
    detector_obj *o=MP_OBJ_TO_PTR(self); hs_reset(&o->engine); o->last_us=0; return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(detector_reset_obj,detector_reset);
static mp_obj_t detector_roi(mp_obj_t self, mp_obj_t width) {
    detector_obj *o=MP_OBJ_TO_PTR(self);
    mp_int_t w=mp_obj_get_int(width);
    if (w<20 || w>320 || !hs_set_roi(&o->engine,(unsigned)w))
        mp_raise_ValueError(MP_ERROR_TEXT("ROI exceeds allocated width or is not even"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(detector_roi_obj,detector_roi);
static mp_obj_t detector_stats(mp_obj_t self) {
    detector_obj *o=MP_OBJ_TO_PTR(self); hs_engine *e=&o->engine;
    mp_obj_t d=mp_obj_new_dict(0);
#define STAT(k) num(d,MP_QSTR_##k,e->stats.k)
    STAT(rows); STAT(on); STAT(off); STAT(triggers); STAT(invalid); STAT(outside_roi);
    STAT(accepted_on); STAT(refractory_rejects); STAT(seeds); STAT(neuron_spikes);
    STAT(headers); STAT(output_overflow); STAT(timestamp_resets);
#undef STAT
    num(d,MP_QSTR_width,e->width); num(d,MP_QSTR_roi_origin,e->lo);
    num(d,MP_QSTR_state_bytes,(uint64_t)e->allocation_width*e->allocation_width*sizeof(hs_pixel));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(detector_stats_obj,detector_stats);
static const mp_rom_map_elem_t detector_locals_table[]={
    {MP_ROM_QSTR(MP_QSTR_feed),MP_ROM_PTR(&detector_feed_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_us),MP_ROM_PTR(&detector_last_us_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset),MP_ROM_PTR(&detector_reset_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_roi),MP_ROM_PTR(&detector_roi_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats),MP_ROM_PTR(&detector_stats_obj)},
};
static MP_DEFINE_CONST_DICT(detector_locals,detector_locals_table);
MP_DEFINE_CONST_OBJ_TYPE(detector_type,MP_QSTR_Detector,MP_TYPE_FLAG_NONE,
    make_new,detector_make,locals_dict,&detector_locals);
static const mp_rom_map_elem_t globals_table[]={
    {MP_ROM_QSTR(MP_QSTR___name__),MP_ROM_QSTR(MP_QSTR_header_snn)},
    {MP_ROM_QSTR(MP_QSTR_Detector),MP_ROM_PTR(&detector_type)},
};
static MP_DEFINE_CONST_DICT(globals,globals_table);
const mp_obj_module_t header_snn_module={.base={&mp_type_module},.globals=(mp_obj_dict_t*)&globals};
MP_REGISTER_MODULE(MP_QSTR_header_snn,header_snn_module);
