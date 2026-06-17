#ifndef _IR_H
#define _IR_H

#include "../ur/size_t.h"
#include "../ur/bool.h"
#include "../ur/int.h"
#include "../ur/seglist.h"
#include "../ur/stablelist.h"

// IR: the size-agnostic IR that is the seam between the front end (parse) and the
// backend (emit). The front end emits a flat instruction stream of three-address ops
// over constants and temporaries; the backend lowers it to a target's machine code.

typedef enum {
    IR_CONSTANT,
    IR_VAR,          // a temporary, identified by index
} IrValKind;

typedef struct {
    IrValKind kind;
    u8 size;            // operand width in bytes (4=int, 8=long/pointer/double). Authoritative for a
                        // CONSTANT; for a VAR the temp's own size (IrFunc.temp_sizes) governs.
    u8 flt;             // CONSTANT: 1 if this is a `double` (value holds its IEEE-754 bit pattern); for
                        // a VAR the temp's class (ir_temp_is_double) governs.
    u64 value;          // CONSTANT: the integer value (or double bit pattern); VAR: the temp index
} IrVal;

typedef enum {
    IR_NEGATE,
    IR_COMPLEMENT,
    IR_NOT,             // logical not: dst = (a == 0)
} IrUnaryOp;

// One operand of an inline-asm statement, resolved from its constraint to a concrete register and
// direction. Inputs are loaded into `reg` before the assembled bytes run; outputs are stored from
// `reg` into `temp` after. The register/direction come from parsing the GCC constraint string.
typedef struct {
    u8 reg;                // x86-64 register the constraint binds to (0=rax, 1=rcx, 2=rdx, ... 7=rdi)
    Bool is_out;           // 1: store reg -> temp (output); 0: load val -> reg (input)
    IrVal val;          // input value (is_out == 0)
    u32 temp;              // output destination temp (is_out == 1)
} AsmOperand;

typedef enum {
    IR_ADD,
    IR_SUBTRACT,
    IR_MULTIPLY,
    IR_DIVIDE,
    IR_REMAINDER,
    IR_BIT_AND,
    IR_BIT_OR,
    IR_BIT_XOR,
    IR_LSHIFT,
    IR_RSHIFT,
    IR_EQUAL,           // dst = (a == b)  -- these yield 0/1
    IR_NOT_EQUAL,
    IR_LESS,
    IR_LESS_EQUAL,
    IR_GREATER,
    IR_GREATER_EQUAL,
} IrBinaryOp;

typedef enum {
    IR_RETURN,           // return a
    IR_UNARY,            // dst = op a
    IR_BINARY,           // dst = a binop b
    IR_COPY,             // dst = a
    IR_JUMP,             // goto label
    IR_JUMP_IF_ZERO,     // if a == 0 goto label
    IR_JUMP_IF_NOT_ZERO, // if a != 0 goto label
    IR_LABEL,            // label:
    IR_FUNCALL,          // dst = callee(args...)
    IR_CALL_PTR,         // dst = (*a)(args...)  -- a is a function-pointer value (indirect call)
    IR_ADDROF_FUNC,      // dst = &function[callee]  -- the code address of a function (8-byte temp)
    IR_CLZ,              // dst (int) = count-leading-zeros of a (8-byte) -- __builtin_clzll; UB if a==0
    IR_LOAD_STATIC,      // dst = the static variable `callee`
    IR_STORE_STATIC,     // the static variable `callee` = a
    IR_ADDROF,           // dst = &a  (a is a local temp; dst is a pointer (8-byte) temp)
    IR_ADDROF_STATIC,    // dst = &static[callee]
    IR_LOAD,             // dst = *a   (a is a pointer)
    IR_STORE,            // *a = b     (a is a pointer)
    IR_COPY_BLOCK,       // memcpy(a, b, callee)  -- a/b are address temps, callee is the byte count (struct copy)
    IR_SIGN_EXTEND,      // dst (wider) = (signed)a      -- movsxd
    IR_ZERO_EXTEND,      // dst (wider) = (unsigned)a    -- a 32-bit load zero-fills the high half
    IR_TRUNCATE,         // dst (narrower) = a           -- keep the low 32 bits
    IR_INT_TO_DOUBLE,    // dst (double) = (double)a     -- signed int/long -> double (cvtsi2sd)
    IR_DOUBLE_TO_INT,    // dst (int/long) = (T)a        -- double -> signed int/long (cvttsd2si)
    IR_UINT_TO_DOUBLE,   // dst (double) = (double)a     -- unsigned int/long -> double
    IR_DOUBLE_TO_UINT,   // dst (uint/ulong) = (T)a      -- double -> unsigned int/long
    IR_ASM,              // inline assembly: load input operands into their registers, emit the
                            // assembled bytes (asm_byte_*), store output operands. asm_op_* index
                            // the operand pool; the bytes/ops live in the owning IrFunc.
} IrInstrKind;

// 40 bytes, laid out data-oriented (cf. Andrew Kelley's "Practical DOD"): the kind-specific fields
// share one anonymous union keyed by `kind`. `a` and `dst` sit outside the union because some kinds
// need them alongside a union member (COPY_BLOCK uses a=dst-addr, b=src-addr, dst=byte-count). The
// anonymous union/structs keep every field name flat (in->b, in->callee, in->asm_byte_start, ...).
typedef struct {
    IrVal a;            // UNARY/COPY source / BINARY left / RETURN value / JUMP_IF_* test / STORE ptr
                           // / STORE_STATIC value / COPY_BLOCK dst-addr / CALL_PTR function pointer
    union {
        IrVal b;        // BINARY right / STORE value / COPY_BLOCK src-addr
        u32 label;         // JUMP / JUMP_IF_* / LABEL target
        struct {           // FUNCALL: callee + arg range; CALL_PTR: arg range; *_STATIC/ADDROF_*: callee
            u32 callee;    //   referenced symbol id
            u32 arg_start; //   index into the function's call-arg pool
            u32 arg_count; //   number of arguments
        } fun;
        struct {           // IR_ASM
            u32 byte_start;   // index into the function's asm-byte pool
            u32 byte_count;   // number of assembled bytes
            u32 op_start;     // index into the function's asm-operand pool
            u32 op_count;     // number of operands
        } asm_;
    } u;
    u32 dst;               // destination temp (UNARY/BINARY/COPY/FUNCALL/LOAD/...); COPY_BLOCK: byte count
    u8 kind;               // IrInstrKind
    u8 op;                 // IR_UNARY: IrUnaryOp
    u8 binop;              // IR_BINARY: IrBinaryOp
    u8 is_unsigned;        // IR_BINARY: operands are unsigned -- div/rem/compare/>>; picks the unsigned op
} IrInstr;

// One function's IR: its instruction stream, its temporary/label counts, and the pool of
// call arguments referenced by FUNCALL instructions. The first `param_count` temps are the
// parameters (the backend fills them from the calling convention's argument registers).
// The IR of every function in a TranslationUnit shares these lists (owned by that TU, so two TUs
// never touch the same memory -- parsing stays parallelizable). A IrFunc holds only a
// [start, count) range into each, so a function is a handful of u32s -- no per-function allocation.
typedef struct struct_IrLists {
    SegList instrs;      // IrInstr
    SegList temp_sizes;  // u8 per temp
    SegList call_args;   // IrVal
    SegList asm_bytes;   // u8
    SegList asm_ops;     // AsmOperand
} IrLists;

typedef struct struct_IrFunc {
    u32 func_id;        // stable id (matches the parser's function table)
    IrLists* lists;  // the owning TU's shared lists; the ranges below index into these
    u32 param_count;
    u32 instr_start;     // range in the shared instruction list (IrInstr)
    u32 instr_count;
    u32 temp_start;      // range in the shared temp-size list (u8 per temp: 4 int / 8 pointer)
    u32 temp_count;
    u32 label_count;
    u32 call_arg_start;  // range in the shared call-arg list (IrVal); FUNCALL.arg_start is relative
    u32 call_arg_count;
    u32 asm_byte_start;  // range in the shared asm-byte list (assembled machine code for IR_ASM)
    u32 asm_byte_count;
    u32 asm_op_start;    // range in the shared asm-operand list (AsmOperand)
    u32 asm_op_count;
} IrFunc;

typedef enum {
    GLOBAL_FUNC,           // a function
    GLOBAL_STATIC,         // a file-scope (static) variable
} GlobalKind;

// A relocation inside a static's initializer image: write the 8-byte runtime address of function
// `target` at byte `offset` in the static's .data (e.g. a vtable slot `{ some_func }`). `target` is a
// global id (TU-local at parse time; the linker rewrites it to the merged id, like FUNCALL.callee).
typedef struct {
    u32 offset;   // byte offset within the static's image
    u32 target;   // global id of the function whose address goes here
} StaticReloc;

// A symbol in the program's namespace, by id. Functions and statics share one namespace, so the
// linker errors on a clash. `defined` distinguishes a definition from a bare declaration.
typedef struct {
    size_t name_off;    // into the source
    size_t name_len;
    GlobalKind kind;
    u32 param_count;    // GLOBAL_FUNC
    const u8* init;     // GLOBAL_STATIC: materialized initializer image (init_len bytes); NULL => all zeros
    u32 init_len;       // GLOBAL_STATIC: the .data slot byte size (== type_size of the variable)
    const StaticReloc* relocs;   // GLOBAL_STATIC: function-address fixups in the image (NULL if none)
    u32 reloc_count;
    Bool defined;
    Bool internal;   // TU-local (internal or no linkage): the linker never merges it across objects
} IrGlobal;

// A whole program: the defined functions, the symbol table (one entry per id, in id order), and
// which function is `main`.
typedef struct struct_TranslationUnit {
    const u8* ppd_source;     // the preprocessed bytes this TU was parsed from; name_offs index it
    IrLists lists;  // this TU's shared IR lists; its IrFuncs index into these
    SegList funcs;     // array of completed IrFunc
    size_t func_count;
    SegList globals;      // array of IrGlobal, one per id
    size_t global_count;
    u32 func_id_count;  // number of symbol ids in use (size of the backend's id->offset map)
    u32 main_func_id;
} TranslationUnit;

void ir_func_init(IrFunc*, u32 func_id, IrLists*);
u32 ir_new_temp(IrFunc*);       // a 4-byte (int) temp
u32 ir_new_byte_temp(IrFunc*);  // a 1-byte (char) temp
u32 ir_new_short_temp(IrFunc*); // a 2-byte (short) temp
u32 ir_new_ptr_temp(IrFunc*);   // an 8-byte (pointer) temp
u32 ir_new_double_temp(IrFunc*);// an 8-byte (double, xmm-class) temp
u32 ir_new_array_temp(IrFunc*, u32 byte_size);   // ceil(size/8) consecutive 8-byte slots; returns the first
u8 ir_temp_size(const IrFunc*, u32 temp);        // byte width (8 for a double temp)
Bool ir_temp_is_double(const IrFunc*, u32 temp); // true for an xmm-class (double) temp
u8 ir_temp_class(const IrFunc*, u32 temp);       // the raw stored class byte (for serialization)
u32 ir_new_label(IrFunc*);
void ir_emit(IrFunc*, IrInstr);
IrInstr* ir_instr_at(const IrFunc*, u32 i);   // i in [0, instr_count); stable pointer
u32 ir_add_arg(IrFunc*, IrVal);           // append a call argument; returns its pool index
IrVal ir_arg_at(const IrFunc*, size_t index);
u32 ir_add_asm_byte(IrFunc*, u8);            // append an assembled byte; returns its pool index
u8 ir_asm_byte_at(const IrFunc*, size_t index);
u32 ir_add_asm_op(IrFunc*, AsmOperand);      // append an asm operand; returns its pool index
AsmOperand ir_asm_op_at(const IrFunc*, size_t index);

void tu_init(TranslationUnit*, const u8* ppd_source);
void tu_add(TranslationUnit*, IrFunc);  // append a completed function
IrFunc* tu_at(const TranslationUnit*, size_t index);
void tu_add_global(TranslationUnit*, IrGlobal);
IrGlobal* tu_global_at(const TranslationUnit*, size_t index);

static inline IrVal ir_constant_sized(u64 v, u8 size) { IrVal x; x.kind = IR_CONSTANT; x.size = size; x.flt = 0; x.value = v;  return x; }
static inline IrVal ir_constant(u64 v) { return ir_constant_sized(v, 4); }     // an int constant
static inline IrVal ir_double_const(u64 bits) { IrVal x; x.kind = IR_CONSTANT; x.size = 8; x.flt = 1; x.value = bits; return x; }
static inline IrVal ir_var(u32 id)     { IrVal x; x.kind = IR_VAR; x.size = 0; x.flt = 0; x.value = id; return x; }
static inline u32 ir_temp(IrVal v)     { return (u32)v.value; }   // a VAR's temp index (always small)

#endif // _IR_H
