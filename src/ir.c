#include "ir.h"

// A IrFunc's IR lives in its owning TU's shared lists (f->lists), indexed by [start, count).
// Building a function appends to those lists and allocates nothing per-function -- one dense list
// per TU, not 8000 tiny page-segments. Functions are built sequentially, so each range is a run.

void ir_func_init(IrFunc* f, u32 func_id, IrLists* lists)
{
    f->func_id = func_id;
    f->lists = lists;
    f->param_count = 0;
    f->instr_start = (u32)lists->instrs.count;
    f->instr_count = 0;
    f->temp_start = (u32)lists->temp_sizes.count;
    f->temp_count = 0;
    f->label_count = 0;
    f->call_arg_start = (u32)lists->call_args.count;
    f->call_arg_count = 0;
    f->asm_byte_start = (u32)lists->asm_bytes.count;
    f->asm_byte_count = 0;
    f->asm_op_start = (u32)lists->asm_ops.count;
    f->asm_op_count = 0;
}

static u32 new_temp_sized(IrFunc* f, u8 size)
{
    SEG_LIST_APPEND(u8, &f->lists->temp_sizes, size);
    return f->temp_count++;
}

#define TEMP_DOUBLE 0x80   // OR'd into the stored class byte: an 8-byte xmm-class (double) temp

u32 ir_new_temp(IrFunc* f)       { return new_temp_sized(f, 4); }
u32 ir_new_byte_temp(IrFunc* f)  { return new_temp_sized(f, 1); }
u32 ir_new_short_temp(IrFunc* f) { return new_temp_sized(f, 2); }
u32 ir_new_ptr_temp(IrFunc* f)   { return new_temp_sized(f, 8); }
u32 ir_new_double_temp(IrFunc* f){ return new_temp_sized(f, TEMP_DOUBLE | 8); }
u32 ir_new_array_temp(IrFunc* f, u32 byte_size)
{
    u32 slots = byte_size ? (byte_size + 7) / 8 : 1;
    // arrays >= 16 bytes must be 16-byte aligned. Codegen 16-aligns the temp `base` and slots are 8
    // bytes, so an even starting slot index lands the array on a 16-byte boundary; pad if it's odd.
    if (byte_size >= 16 && (f->temp_count & 1)) new_temp_sized(f, 8);
    u32 first = f->temp_count;
    for (u32 i = 0; i < slots; i++) new_temp_sized(f, 8);
    return first;
}

u8 ir_temp_class(const IrFunc* f, u32 temp)
{
    return SEG_LIST_VAL(u8, &f->lists->temp_sizes, f->temp_start + temp);
}
u8 ir_temp_size(const IrFunc* f, u32 temp)
{
    return ir_temp_class(f, temp) & ~TEMP_DOUBLE;   // 8 for a double temp
}
Bool ir_temp_is_double(const IrFunc* f, u32 temp)
{
    return (ir_temp_class(f, temp) & TEMP_DOUBLE) != 0;
}

u32 ir_new_label(IrFunc* f)
{
    return f->label_count++;
}

void ir_emit(IrFunc* f, IrInstr instr)
{
    SEG_LIST_APPEND(IrInstr, &f->lists->instrs, instr);
    f->instr_count++;
}

IrInstr* ir_instr_at(const IrFunc* f, u32 i)
{
    return SEG_LIST_REF(IrInstr, &f->lists->instrs, f->instr_start + i);
}

u32 ir_add_arg(IrFunc* f, IrVal v)
{
    SEG_LIST_APPEND(IrVal, &f->lists->call_args, v);
    return f->call_arg_count++;
}

IrVal ir_arg_at(const IrFunc* f, size_t index)
{
    return SEG_LIST_VAL(IrVal, &f->lists->call_args, f->call_arg_start + index);
}

u32 ir_add_asm_byte(IrFunc* f, u8 byte)
{
    SEG_LIST_APPEND(u8, &f->lists->asm_bytes, byte);
    return f->asm_byte_count++;
}

u8 ir_asm_byte_at(const IrFunc* f, size_t index)
{
    return SEG_LIST_VAL(u8, &f->lists->asm_bytes, f->asm_byte_start + index);
}

u32 ir_add_asm_op(IrFunc* f, AsmOperand op)
{
    SEG_LIST_APPEND(AsmOperand, &f->lists->asm_ops, op);
    return f->asm_op_count++;
}

AsmOperand ir_asm_op_at(const IrFunc* f, size_t index)
{
    return SEG_LIST_VAL(AsmOperand, &f->lists->asm_ops, f->asm_op_start + index);
}

void tu_init(TranslationUnit* p, const u8* ppd_source)
{
    p->ppd_source = ppd_source;
    p->lists.instrs     = SEG_LIST_INIT(IrInstr);
    p->lists.temp_sizes = SEG_LIST_INIT(u8);
    p->lists.call_args  = SEG_LIST_INIT(IrVal);
    p->lists.asm_bytes  = SEG_LIST_INIT(u8);
    p->lists.asm_ops    = SEG_LIST_INIT(AsmOperand);
    p->funcs = SEG_LIST_INIT(IrFunc);
    p->func_count = 0;
    p->globals = SEG_LIST_INIT(IrGlobal);
    p->global_count = 0;
    p->func_id_count = 0;
    p->main_func_id = 0;
}

void tu_add_global(TranslationUnit* p, IrGlobal sym)
{
    SEG_LIST_APPEND(IrGlobal, &p->globals, sym);
    p->global_count += 1;
}

IrGlobal* tu_global_at(const TranslationUnit* p, size_t index)
{
    return SEG_LIST_REF(IrGlobal, &p->globals, index);
}

void tu_add(TranslationUnit* p, IrFunc f)
{
    SEG_LIST_APPEND(IrFunc, &p->funcs, f);
    p->func_count += 1;
}

IrFunc* tu_at(const TranslationUnit* p, size_t index)
{
    return SEG_LIST_REF(IrFunc, &p->funcs, index);
}
