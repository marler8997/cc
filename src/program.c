#include "program.h"

#include "../ur/abortmacros.h"
#include "../ur/pages.h"
#include "ir.h"

void program_init(Program* p, u32 func_id_count)
{
    p->funcs = SEG_LIST_INIT(IrFunc);
    p->func_count = 0;
    size_t n = func_id_count ? func_id_count : 1;
    MUST(pages_alloc_fixed((void**)&p->used, n * sizeof(Bool)));   // fresh-committed pages are zero
    p->func_id_count = func_id_count;
    p->main_func_id = 0;
    p->imports = SEG_LIST_INIT(Import);
    p->import_count = 0;
    MUST(pages_alloc_fixed((void**)&p->import_of, n * sizeof(u32)));
    for (size_t i = 0; i < n; i++) p->import_of[i] = PROGRAM_NO_IMPORT;
    p->static_data = SEG_LIST_INIT(u8);
    p->static_size = 0;
    MUST(pages_alloc_fixed((void**)&p->static_of, n * sizeof(u32)));
    for (size_t i = 0; i < n; i++) p->static_of[i] = PROGRAM_NO_STATIC;
    p->static_relocs = STABLE_LIST_INIT(ProgramReloc);
}

void program_add(Program* p, IrFunc f)
{
    SEG_LIST_APPEND(IrFunc, &p->funcs, f);
    p->func_count += 1;
}

IrFunc* program_func_at(const Program* p, size_t index)
{
    return SEG_LIST_REF(IrFunc, &p->funcs, index);
}

Bool program_used(const Program* p, u32 func_id)
{
    return p->used[func_id];
}

void program_set_used(Program* p, u32 func_id, Bool used)
{
    p->used[func_id] = used;
}

u32 program_add_import(Program* p, Import imp)
{
    SEG_LIST_APPEND(Import, &p->imports, imp);
    return (u32)p->import_count++;
}

Import program_import_at(const Program* p, size_t index)
{
    return SEG_LIST_VAL(Import, &p->imports, index);
}

u32 program_import_of(const Program* p, u32 func_id)
{
    return p->import_of[func_id];
}

void program_set_import_of(Program* p, u32 func_id, u32 import_index)
{
    p->import_of[func_id] = import_index;
}

u32 program_add_static_bytes(Program* p, const u8* bytes, u32 size)
{
    u32 off = (u32)p->static_size;
    u8 zero = 0;
    for (u32 i = 0; i < size; i++) { u8 b = bytes[i]; SEG_LIST_APPEND(u8, &p->static_data, b); }
    while (p->static_data.count % 8) SEG_LIST_APPEND(u8, &p->static_data, zero);   // pad each static to 8 bytes
    p->static_size = p->static_data.count;
    return off;
}

u32 program_add_static_zeros(Program* p, u32 size)
{
    u32 off = (u32)p->static_size;
    u8 zero = 0;
    for (u32 i = 0; i < size; i++) SEG_LIST_APPEND(u8, &p->static_data, zero);
    while (p->static_data.count % 8) SEG_LIST_APPEND(u8, &p->static_data, zero);   // pad to 8 bytes
    p->static_size = p->static_data.count;
    return off;
}

u8 program_static_byte(const Program* p, u32 offset) { return SEG_LIST_VAL(u8, &p->static_data, offset); }
u32 program_static_size(const Program* p) { return (u32)p->static_size; }

u32 program_static_of(const Program* p, u32 func_id)
{
    return p->static_of[func_id];
}

void program_set_static_of(Program* p, u32 func_id, u32 static_index)
{
    p->static_of[func_id] = static_index;
}

void program_add_static_reloc(Program* p, u32 data_offset, u32 target, Bool to_static)
{
    ProgramReloc r = { data_offset, target, to_static };
    STABLE_LIST_APPEND(ProgramReloc, &p->static_relocs, r);
}
