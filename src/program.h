#ifndef _PROGRAM_H
#define _PROGRAM_H

#include "../ur/size_t.h"
#include "../ur/bool.h"
#include "../ur/int.h"
#include "../ur/seglist.h"
#include "../ur/stablelist.h"
#include "import.h"

typedef struct struct_IrFunc IrFunc;

#define PROGRAM_NO_IMPORT 0xFFFFFFFFu
#define PROGRAM_NO_STATIC 0xFFFFFFFFu


// a .data fixup: place an absolute runtime address at `data_offset` in .data. The address is a
// function's code address (to_static=0, target is a func_id) or another static's .data address
// (to_static=1, target is the target static's .data byte offset).
typedef struct {
    u32 data_offset;
    u32 target;
    Bool to_static;
} ProgramReloc;

// The linked, codegen-ready image: one merged set of functions over a single global func_id
// space, the entry point, and which func_ids survived mark-sweep. Unlike a TranslationUnit it
// has no source and no names -- linking has resolved every reference to a global func_id.
typedef struct struct_Program {
    SegList funcs;    // array of IrFunc; global func_ids, callees rewritten
    size_t func_count;
    Bool* used;       // Bool per func_id (mark-sweep result); sized to func_id_count
    u32 func_id_count;
    u32 main_func_id;
    SegList imports;  // ImportRef per used import, in IAT order
    size_t import_count;
    u32* import_of;   // u32 per func_id: its index into `imports`, or PROGRAM_NO_IMPORT
    SegList static_data;  // .data bytes for used statics (8-aligned per static), addressed by byte offset
    size_t static_size;   // total .data byte size
    u32* static_of;   // u32 per func_id: its byte offset into .data, or PROGRAM_NO_STATIC
    StableList static_relocs;  // ProgramReloc per .data->code fixup; append (link) + iterate (exe)
} Program;

void program_init(Program*, u32 func_id_count);     // allocates `used`/`import_of` for func_id_count ids
void program_add(Program*, IrFunc);              // append a function
IrFunc* program_func_at(const Program*, size_t index);
Bool program_used(const Program*, u32 func_id);
void program_set_used(Program*, u32 func_id, Bool used);
u32 program_add_import(Program*, Import imp);   // append; returns its index
Import program_import_at(const Program*, size_t index);
u32 program_import_of(const Program*, u32 func_id);
void program_set_import_of(Program*, u32 func_id, u32 import_index);
u32 program_add_static_bytes(Program*, const u8* bytes, u32 size);   // append a sized static (8-aligned); returns offset
u32 program_add_static_zeros(Program*, u32 size);      // append a zero-filled static (8-aligned); returns offset
u8  program_static_byte(const Program*, u32 offset);   // a byte of the .data image
u32 program_static_size(const Program*);               // total .data byte size
u32 program_static_of(const Program*, u32 func_id);    // its .data byte offset
void program_set_static_of(Program*, u32 func_id, u32 offset);
void program_add_static_reloc(Program*, u32 data_offset, u32 target, Bool to_static);   // a .data fixup
// to read them, iterate program->static_relocs with STABLE_LIST_FOREACH (a ProgramReloc per element)

#endif // _PROGRAM_H
