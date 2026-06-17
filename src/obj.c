#include "obj.h"

#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "../ur/log.h"
#include "../ur/processexit.h"
#include "cursor.h"
#include "ir.h"

// Format (all little-endian): magic, then the symbol table (one entry per global id, names inlined),
// then the functions (each a flat dump of its IR). Fields are written individually rather than as
// raw structs so the format doesn't depend on struct layout/padding.

#define OBJ_MAGIC 0x314A424F   // "OBJ1"

typedef struct { const u8* p; size_t len; size_t pos; } Reader;

static u8  r8(Reader* r)  { ASSERT(r->pos < r->len); return r->p[r->pos++]; }
static u32 r32(Reader* r) { u32 v = r8(r); v |= (u32)r8(r) << 8; v |= (u32)r8(r) << 16; v |= (u32)r8(r) << 24; return v; }
static u64 r64(Reader* r) { u64 lo = r32(r); u64 hi = r32(r); return lo | (hi << 32); }

static void put_val(Cursor* c, IrVal v) { put32(c, (u32)v.kind); put8(c, v.size); put8(c, v.flt); put64(c, v.value); }
static IrVal get_val(Reader* r) { IrVal v; v.kind = (IrValKind)r32(r); v.size = r8(r); v.flt = r8(r); v.value = r64(r); return v; }

size_t obj_write(const TranslationUnit* tu, Pages* out)
{
    Cursor c = { out, 0 };
    put32(&c, OBJ_MAGIC);

    // symbol table: one global per id (global_count == func_id_count). Names are inlined.
    put32(&c, (u32)tu->global_count);
    put32(&c, tu->main_func_id);
    for (u32 i = 0; i < tu->global_count; i++) {
        IrGlobal* g = tu_global_at(tu, i);
        if (g->reloc_count) { LOG_STRING("obj: cannot serialize a static with function-address relocations yet"); process_exit(94); }
        put32(&c, (u32)g->kind);
        put32(&c, g->param_count);
        put32(&c, g->init_len);
        put8(&c, (u8)(g->init != 0));
        if (g->init) put_bytes(&c, (const char*)g->init, g->init_len);
        put8(&c, (u8)g->defined);
        put8(&c, (u8)g->internal);
        put32(&c, (u32)g->name_len);
        put_bytes(&c, (const char*)(tu->ppd_source + g->name_off), g->name_len);
    }

    // defined functions, each a flat dump of its IR
    put32(&c, (u32)tu->func_count);
    for (size_t f = 0; f < tu->func_count; f++) {
        IrFunc* fn = tu_at(tu, f);
        put32(&c, fn->func_id);
        put32(&c, fn->param_count);
        put32(&c, fn->temp_count);
        put32(&c, fn->label_count);
        put64(&c, (u64)fn->instr_count);
        for (u32 i = 0; i < fn->instr_count; i++) {
            IrInstr* in = ir_instr_at(fn, i);
            if (in->kind == IR_ASM) { LOG_STRING("obj: cannot serialize inline asm to an object file"); process_exit(94); }
            // common fields (a/dst sit outside the union), then only the live union payload by kind
            put32(&c, (u32)in->kind);  put32(&c, (u32)in->op);  put32(&c, (u32)in->binop);
            put8(&c, in->is_unsigned);
            put_val(&c, in->a);
            put32(&c, in->dst);
            switch (in->kind) {
                case IR_BINARY: case IR_STORE: case IR_COPY_BLOCK:
                    put_val(&c, in->u.b);
                    break;
                case IR_JUMP: case IR_JUMP_IF_ZERO: case IR_JUMP_IF_NOT_ZERO: case IR_LABEL:
                    put32(&c, in->u.label);
                    break;
                case IR_LOAD_STATIC: case IR_STORE_STATIC: case IR_ADDROF_STATIC: case IR_ADDROF_FUNC:
                    put32(&c, in->u.fun.callee);
                    break;
                case IR_FUNCALL:
                    put32(&c, in->u.fun.callee); put32(&c, in->u.fun.arg_start); put32(&c, in->u.fun.arg_count);
                    break;
                case IR_CALL_PTR:
                    put32(&c, in->u.fun.arg_start); put32(&c, in->u.fun.arg_count);
                    break;
                default: break;   // a/dst already cover the rest
            }
        }
        put64(&c, (u64)fn->call_arg_count);
        for (size_t i = 0; i < fn->call_arg_count; i++) {
            IrVal v = ir_arg_at(fn, i);
            put_val(&c, v);
        }
        for (u32 i = 0; i < fn->temp_count; i++) put8(&c, ir_temp_class(fn, i));
    }
    return c.pos;
}

void obj_read(const u8* bytes, size_t len, TranslationUnit* out)
{
    Reader r = { bytes, len, 0 };
    if (r32(&r) != OBJ_MAGIC) { LOG_STRING("not a cc object file"); process_exit(1); }

    u32 global_count = r32(&r);
    u32 main_func_id = r32(&r);

    // read globals into a temp array while accumulating their names AND static initializer images
    // into one blob that becomes the TU's `ppd_source` (each name/init is an offset within it). The
    // blob may move as it grows, so init pointers are resolved against the final base afterward.
    ArenaPosition arena_pos = arena_position(&global_arena);
    IrGlobal* gs = ARENA_ALLOC(&global_arena, (global_count ? global_count : 1) * sizeof(IrGlobal));
    size_t* init_off = ARENA_ALLOC(&global_arena, (global_count ? global_count : 1) * sizeof(size_t));
    Pages blob = PAGES_INIT();
    Cursor bc = { &blob, 0 };
    for (u32 i = 0; i < global_count; i++) {
        gs[i].kind = (GlobalKind)r32(&r);
        gs[i].param_count = r32(&r);
        gs[i].init_len = r32(&r);
        gs[i].init = 0;
        gs[i].relocs = 0;
        gs[i].reloc_count = 0;
        if (r8(&r)) {
            init_off[i] = bc.pos;
            for (u32 k = 0; k < gs[i].init_len; k++) put8(&bc, r8(&r));
        } else {
            init_off[i] = (size_t)-1;        // no image: zero-filled
        }
        gs[i].defined = r8(&r);
        gs[i].internal = r8(&r);
        u32 nl = r32(&r);
        gs[i].name_off = bc.pos;
        gs[i].name_len = nl;
        for (u32 k = 0; k < nl; k++) put8(&bc, r8(&r));
    }

    tu_init(out, blob.ptr);
    for (u32 i = 0; i < global_count; i++) {
        if (init_off[i] != (size_t)-1) gs[i].init = blob.ptr + init_off[i];
        tu_add_global(out, gs[i]);
    }
    out->func_id_count = global_count;
    out->main_func_id = main_func_id;
    arena_reset(&global_arena, arena_pos);   // gs + init_off are spent

    u32 func_count = r32(&r);
    for (u32 f = 0; f < func_count; f++) {
        u32 func_id = r32(&r);
        IrFunc fn;
        ir_func_init(&fn, func_id, &out->lists);   // records the starts into this TU's lists
        fn.param_count = r32(&r);
        u32 temp_count = r32(&r);
        fn.label_count = r32(&r);
        u64 instr_count = r64(&r);
        for (u64 i = 0; i < instr_count; i++) {
            IrInstr in;
            in.kind = (u8)r32(&r); in.op = (u8)r32(&r); in.binop = (u8)r32(&r);
            in.is_unsigned = r8(&r);
            in.a = get_val(&r);
            in.dst = r32(&r);
            switch (in.kind) {
                case IR_BINARY: case IR_STORE: case IR_COPY_BLOCK:
                    in.u.b = get_val(&r);
                    break;
                case IR_JUMP: case IR_JUMP_IF_ZERO: case IR_JUMP_IF_NOT_ZERO: case IR_LABEL:
                    in.u.label = r32(&r);
                    break;
                case IR_LOAD_STATIC: case IR_STORE_STATIC: case IR_ADDROF_STATIC: case IR_ADDROF_FUNC:
                    in.u.fun.callee = r32(&r);
                    break;
                case IR_FUNCALL:
                    in.u.fun.callee = r32(&r); in.u.fun.arg_start = r32(&r); in.u.fun.arg_count = r32(&r);
                    break;
                case IR_CALL_PTR:
                    in.u.fun.arg_start = r32(&r); in.u.fun.arg_count = r32(&r);
                    break;
                default: break;
            }
            ir_emit(&fn, in);
        }
        u64 call_arg_count = r64(&r);
        for (u64 i = 0; i < call_arg_count; i++) { IrVal v = get_val(&r); ir_add_arg(&fn, v); }
        for (u32 i = 0; i < temp_count; i++) {
            u8 cls = r8(&r);
            if (cls & 0x80)      ir_new_double_temp(&fn);
            else if (cls == 8)   ir_new_ptr_temp(&fn);
            else if (cls == 1)   ir_new_byte_temp(&fn);
            else                 ir_new_temp(&fn);
        }
        tu_add(out, fn);
    }
}
