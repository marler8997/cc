#include "codegen.h"

#include "../ur/arena.h"
#include "../ur/log.h"
#include "../ur/processexit.h"
#include "cursor.h"
#include "ir.h"
#include "program.h"

// x86_64 register numbers (eax=0, ecx=1, edx=2, ..., r8=8, r9=9); r8+ need a REX prefix
#define REG_EAX 0
#define REG_ECX 1
#define REG_EDX 2
#define REG_ESI 6
#define REG_EDI 7

static u32 abi_shadow(const Abi* abi)   { return abi->kind == ABI_WIN64 ? 0x20 : 0; }

// the registers integer arguments arrive in, in order (used by place_args)
static const u8 win64_argreg[4] = {1, 2, 8, 9};        // ecx, edx, r8d, r9d
static const u8 sysv_argreg[6]  = {7, 6, 2, 1, 8, 9};  // edi, esi, edx, ecx, r8d, r9d

// REX prefix for a [rsp+disp] memory op with register `rc` in ModRM.reg; `wide` adds REX.W (64-bit)
static void rex_mem(Cursor* c, u8 rc, Bool wide)
{
    u8 rex = (u8)(0x40 | (wide ? 8 : 0) | (rc >= 8 ? 4 : 0));   // W bit, R bit
    if (rex != 0x40) put8(c, rex);
}

// mov reg, [rsp + disp32]  (4- or 8-byte)
static void load_mem(Cursor* c, u8 rc, u32 disp, Bool wide)
{
    rex_mem(c, rc, wide);
    put8(c, 0x8B); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24);
    put32(c, disp);
}

// mov [rsp + disp32], reg  (4- or 8-byte)
static void store_mem(Cursor* c, u8 rc, u32 disp, Bool wide)
{
    rex_mem(c, rc, wide);
    put8(c, 0x89); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24);
    put32(c, disp);
}

// REX for a byte register op: rc>=8 needs REX.R (r8b-r15b); rc 4-7 needs any REX to select the
// low-byte regs spl/bpl/sil/dil instead of the legacy ah/ch/dh/bh. al/cl/dl/bl (rc<4) need none.
static void rex_mem_byte(Cursor* c, u8 rc) { if (rc >= 4) put8(c, (u8)(0x40 | (rc >= 8 ? 4 : 0))); }

// mov r8, [rsp + disp32] / mov [rsp + disp32], r8  (1-byte)
static void load_mem_byte(Cursor* c, u8 rc, u32 disp)
{
    rex_mem_byte(c, rc);
    put8(c, 0x8A); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}
static void store_mem_byte(Cursor* c, u8 rc, u32 disp)
{
    rex_mem_byte(c, rc);
    put8(c, 0x88); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}

// mov r16, [rsp + disp32] / mov [rsp + disp32], r16  (2-byte, via the 0x66 operand-size prefix)
static void load_mem_word(Cursor* c, u8 rc, u32 disp)
{
    put8(c, 0x66); if (rc >= 8) put8(c, 0x44);
    put8(c, 0x8B); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}
static void store_mem_word(Cursor* c, u8 rc, u32 disp)
{
    put8(c, 0x66); if (rc >= 8) put8(c, 0x44);
    put8(c, 0x89); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}

// lea reg, [rsp + disp32]  (computes a 64-bit address)
static void lea_local(Cursor* c, u8 rc, u32 disp)
{
    rex_mem(c, rc, 1);
    put8(c, 0x8D); put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24);
    put32(c, disp);
}

// every temp gets an 8-byte slot at [rsp + base + idx*8]; a temp's mov width comes from its recorded
// size (4 for int, 8 for pointer; via ir_temp_size). int values just use the low dword of the slot.
static void load_reg(Cursor* c, u32 base, u8 rc, IrVal v, const IrFunc* fn)
{
    if (v.kind == IR_CONSTANT) {
        if (v.size == 8 && ((i64)v.value < -0x80000000LL || (i64)v.value > 0x7FFFFFFFLL)) {
            put8(c, rc >= 8 ? 0x49 : 0x48);                  // REX.W (+B)
            put8(c, (u8)(0xB8 + (rc & 7))); put64(c, v.value);   // movabs reg, imm64
        } else if (v.size == 8) {
            put8(c, rc >= 8 ? 0x49 : 0x48);                  // REX.W (+B)
            put8(c, 0xC7); put8(c, (u8)(0xC0 | (rc & 7))); put32(c, (u32)v.value);  // mov r64, imm32 (sign-extended)
        } else {
            if (rc >= 8) put8(c, 0x41);                      // REX.B
            put8(c, (u8)(0xB8 + (rc & 7))); put32(c, (u32)v.value);   // mov r32, imm32 (zero-extends to 64-bit)
        }
    } else {
        u32 t = ir_temp(v);
        u8 sz = ir_temp_size(fn, t);
        if (sz == 1)      load_mem_byte(c, rc, base + t * 8);
        else if (sz == 2) load_mem_word(c, rc, base + t * 8);
        else              load_mem(c, rc, base + t * 8, sz == 8);
    }
}

// the byte width of a IrVal: a constant carries its own size; a temp's is its recorded size
static u8 val_size(IrVal v, const IrFunc* fn) { return v.kind == IR_CONSTANT ? v.size : ir_temp_size(fn, ir_temp(v)); }

// store register `rc` into a temporary's stack slot, at the temp's width
static void store_reg(Cursor* c, u32 base, u8 rc, u32 dst, const IrFunc* fn)
{
    u8 sz = ir_temp_size(fn, dst);
    if (sz == 1)      store_mem_byte(c, rc, base + dst * 8);
    else if (sz == 2) store_mem_word(c, rc, base + dst * 8);
    else              store_mem(c, rc, base + dst * 8, sz == 8);
}

// --- SSE / double support: xmm-class values live in the same 8-byte stack slots, moved with movsd ---

// is this value a `double` (xmm class)? a constant carries its own flag; a temp's class governs.
static Bool val_is_double(IrVal v, const IrFunc* fn)
{
    return v.kind == IR_CONSTANT ? v.flt : ir_temp_is_double(fn, ir_temp(v));
}

// movsd xmm_rc, [rsp+disp32]  /  movsd [rsp+disp32], xmm_rc   (rc: xmm register 0..15)
static void load_xmm_mem(Cursor* c, u8 rc, u32 disp)
{
    put8(c, 0xF2); if (rc >= 8) put8(c, 0x44); put8(c, 0x0F); put8(c, 0x10);
    put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}
static void store_xmm_mem(Cursor* c, u8 rc, u32 disp)
{
    put8(c, 0xF2); if (rc >= 8) put8(c, 0x44); put8(c, 0x0F); put8(c, 0x11);
    put8(c, (u8)(0x84 | ((rc & 7) << 3))); put8(c, 0x24); put32(c, disp);
}
// movq xmm_x, r64_g  (66 REX.W 0F 6E) / movq r64_g, xmm_x  (66 REX.W 0F 7E)
static void movq_gpr_to_xmm(Cursor* c, u8 x, u8 g)
{
    put8(c, 0x66); put8(c, (u8)(0x48 | (x >= 8 ? 4 : 0) | (g >= 8 ? 1 : 0)));
    put8(c, 0x0F); put8(c, 0x6E); put8(c, (u8)(0xC0 | ((x & 7) << 3) | (g & 7)));
}
// load a value into xmm `x` (constants go via movabs rax + movq, so rax is clobbered)
static void load_xmm(Cursor* c, u32 base, u8 x, IrVal v)
{
    if (v.kind == IR_CONSTANT) {
        put8(c, 0x48); put8(c, 0xB8); put64(c, v.value);   // movabs rax, bits
        movq_gpr_to_xmm(c, x, REG_EAX);
    } else {
        load_xmm_mem(c, x, base + ir_temp(v) * 8);
    }
}
// an xmm-xmm SSE op with the given (already-prefixed) opcode byte: <pfx> 0F <opc> /r, dst=x0 rm=x1
static void sse_op(Cursor* c, u8 pfx, u8 opc, u8 x0, u8 x1)
{
    if (pfx) put8(c, pfx);
    if (x0 >= 8 || x1 >= 8) put8(c, (u8)(0x40 | (x0 >= 8 ? 4 : 0) | (x1 >= 8 ? 1 : 0)));
    put8(c, 0x0F); put8(c, opc); put8(c, (u8)(0xC0 | ((x0 & 7) << 3) | (x1 & 7)));
}

// the value to exit with is in eax; emit the process exit per the ABI (used by the entry stub)
static void emit_exit(Cursor* c, const Abi* abi)
{
    if (abi->kind == ABI_WIN64) {
        put8(c, 0x89); put8(c, 0xC1);                        // mov ecx, eax
        size_t k = c->pos;
        put8(c, 0xFF); put8(c, 0x15);                        // call qword ptr [rip+disp32]  (exit is import 0)
        put32(c, (u32)(abi->iat_base_rva - (abi->code_base_rva + (u32)k + 6)));
        put8(c, 0xCC);                                       // int3 (unreachable)
    } else {
        put8(c, 0x89); put8(c, 0xC7);                        // mov edi, eax
        put8(c, 0xB8); put32(c, abi->exit_syscall);          // mov eax, <exit syscall>
        put8(c, 0x0F); put8(c, 0x05);                        // syscall
    }
}

typedef struct { size_t site; u32 label; }  Fixup;      // a jump's rel32, within a function
typedef struct { size_t site; u32 callee; } CallFixup;  // a call's rel32, to a function

// fixups is an arena array sized to an upper bound (one jump fixup per instruction at most)
static void add_fixup(Fixup* fx, size_t* count, size_t site, u32 label)
{
    fx[*count].site = site; fx[*count].label = label; *count += 1;
}

static void add_call_fixup(StableList* fx, size_t site, u32 callee)
{
    CallFixup cf = { site, callee };
    STABLE_LIST_APPEND(CallFixup, fx, cf);
}

// where one argument/parameter is passed: a register (gp or xmm) or an outgoing stack slot
typedef struct { Bool on_stack; Bool is_xmm; u8 reg; u32 stack_slot; } ArgPlace;

// Assign ABI placements for `n` arguments whose double-ness is is_dbl[i]. Win64 is positional (slot i
// is gp_i or xmm_i, 4 slots); SysV fills the gp (6) and sse (8) register files independently. Overflow
// goes to outgoing stack slots in order. Returns the number of stack slots used.
static u32 place_args(const Abi* abi, const Bool* is_dbl, u32 n, ArgPlace* out)
{
    u32 stack = 0;
    if (abi->kind == ABI_WIN64) {
        for (u32 i = 0; i < n; i++) {
            out[i].is_xmm = is_dbl[i];
            if (i < 4) { out[i].on_stack = 0; out[i].reg = is_dbl[i] ? (u8)i : win64_argreg[i]; }
            else       { out[i].on_stack = 1; out[i].stack_slot = stack++; }
        }
    } else {
        u32 gp = 0, sse = 0;
        for (u32 i = 0; i < n; i++) {
            out[i].is_xmm = is_dbl[i];
            if (is_dbl[i] && sse < 8)      { out[i].on_stack = 0; out[i].reg = (u8)sse++; }
            else if (!is_dbl[i] && gp < 6) { out[i].on_stack = 0; out[i].reg = sysv_argreg[gp++]; }
            else                           { out[i].on_stack = 1; out[i].stack_slot = stack++; }
        }
    }
    return stack;
}

// Emit one function at the cursor's current position. Inter-function calls record their site
// in `call_fixups` (patched by gen_code once every function's offset is known).
static void gen_function(Cursor* c, const Abi* abi, const Program* prog, const IrFunc* fn,
                         StableList* call_fixups)
{
    u32 shadow = abi_shadow(abi);
    // scratch for ABI placement, sized to the widest argument list (this function's params or any call)
    u32 max_args = fn->param_count;
    for (u32 ii = 0; ii < fn->instr_count; ii++) {
        IrInstr* in = ir_instr_at(fn, ii);
        if ((in->kind == IR_FUNCALL || in->kind == IR_CALL_PTR) && in->u.fun.arg_count > max_args) max_args = in->u.fun.arg_count;
    }
    Bool* is_dbl = ARENA_ALLOC(&global_arena, (max_args ? max_args : 1) * sizeof(Bool));
    ArgPlace* place = ARENA_ALLOC(&global_arena, (max_args ? max_args : 1) * sizeof(ArgPlace));

    // reserve outgoing space at the frame bottom: shadow space plus room for the most stack arguments
    // any call here passes (per the ABI classification). Temporaries sit above it, one 8-byte slot each.
    u32 max_stack_args = 0;
    for (u32 ii = 0; ii < fn->instr_count; ii++) {
        IrInstr* in = ir_instr_at(fn, ii);
        if (in->kind != IR_FUNCALL && in->kind != IR_CALL_PTR) continue;
        for (u32 a = 0; a < in->u.fun.arg_count; a++) is_dbl[a] = val_is_double(ir_arg_at(fn, in->u.fun.arg_start + a), fn);
        u32 s = place_args(abi, is_dbl, in->u.fun.arg_count, place);
        if (s > max_stack_args) max_stack_args = s;
    }
    // 16-align the temp area so arrays (allocated on even slot indices) land on 16-byte boundaries
    u32 base = (shadow + max_stack_args * 8 + 15u) & ~15u;

    // prologue: reserve the frame, keeping the stack 16-aligned for calls
    u32 frame = ((base + fn->temp_count * 8 + 15) & ~15u) + 8;
    if (abi->kind == ABI_WIN64 && frame > 0x1000) {
        // Windows commits stack lazily behind a single guard page; a frame larger than one page would
        // `sub rsp, frame` and access [rsp+x], jumping past the guard page into uncommitted memory. So
        // probe down a page at a time, touching each so the guard page commits and steps down (the same
        // job MSVC's __chkstk does). r11 is volatile and not a parameter register, so it's free here.
        put8(c, 0x41); put8(c, 0xBB); put32(c, frame);                         // mov  r11d, frame
        put8(c, 0x49); put8(c, 0x81); put8(c, 0xFB); put32(c, 0x1000);         // cmp  r11, 0x1000   (loop)
        put8(c, 0x76); put8(c, 0x14);                                          // jbe  +0x14 -> last
        put8(c, 0x48); put8(c, 0x81); put8(c, 0xEC); put32(c, 0x1000);         // sub  rsp, 0x1000
        put8(c, 0x80); put8(c, 0x0C); put8(c, 0x24); put8(c, 0x00);            // or   byte [rsp], 0  (touch)
        put8(c, 0x49); put8(c, 0x81); put8(c, 0xEB); put32(c, 0x1000);         // sub  r11, 0x1000
        put8(c, 0xEB); put8(c, 0xE3);                                          // jmp  -0x1d -> loop
        put8(c, 0x4C); put8(c, 0x29); put8(c, 0xDC);                           // sub  rsp, r11       (last)
    } else {
        put8(c, 0x48); put8(c, 0x81); put8(c, 0xEC); put32(c, frame);          // sub  rsp, frame
    }

    // move parameters into their temp slots from their ABI placement: a gp/xmm register, or the
    // incoming stack-argument area (above this frame's return address and shadow space)
    for (u32 i = 0; i < fn->param_count; i++) is_dbl[i] = ir_temp_is_double(fn, i);
    place_args(abi, is_dbl, fn->param_count, place);
    for (u32 i = 0; i < fn->param_count; i++) {
        if (place[i].on_stack)     { load_mem(c, REG_EAX, frame + 8 + shadow + place[i].stack_slot * 8, ir_temp_size(fn, i) == 8);
                                     store_reg(c, base, REG_EAX, i, fn); }
        else if (place[i].is_xmm)  store_xmm_mem(c, place[i].reg, base + i * 8);
        else                       store_reg(c, base, place[i].reg, i, fn);
    }

    size_t* label_pos = 0;
    if (fn->label_count) label_pos = ARENA_ALLOC(&global_arena, fn->label_count * sizeof(size_t));
    Fixup* fixups = ARENA_ALLOC(&global_arena, (fn->instr_count ? fn->instr_count : 1) * sizeof(Fixup));
    size_t fixup_count = 0;

    for (u32 ii = 0; ii < fn->instr_count; ii++) {
        IrInstr* in = ir_instr_at(fn, ii);
        switch (in->kind) {
            case IR_UNARY: {
                if (val_is_double(in->a, fn)) {
                    if (in->op == IR_NEGATE) {                 // flip the sign bit (xorpd with 1<<63)
                        load_xmm(c, base, 0, in->a);
                        put8(c, 0x48); put8(c, 0xB8); put64(c, 0x8000000000000000ull);   // movabs rax, signbit
                        movq_gpr_to_xmm(c, 1, REG_EAX);
                        sse_op(c, 0x66, 0x57, 0, 1);              // xorpd xmm0, xmm1
                        store_xmm_mem(c, 0, base + in->dst * 8);
                    } else {                                      // IR_NOT: !x == (x == 0.0), ordered
                        load_xmm(c, base, 0, in->a);
                        sse_op(c, 0x66, 0x57, 1, 1);              // xorpd xmm1, xmm1  (xmm1 = 0.0)
                        sse_op(c, 0x66, 0x2F, 0, 1);              // comisd xmm0, xmm1
                        put8(c, 0x0F); put8(c, 0x94); put8(c, 0xC0);   // sete  al
                        put8(c, 0x0F); put8(c, 0x9B); put8(c, 0xC1);   // setnp cl
                        put8(c, 0x20); put8(c, 0xC8);                  // and al, cl
                        put8(c, 0x0F); put8(c, 0xB6); put8(c, 0xC0);   // movzx eax, al
                        store_reg(c, base, REG_EAX, in->dst, fn);
                    }
                    break;
                }
                load_reg(c, base, REG_EAX, in->a, fn);
                Bool wide = val_size(in->a, fn) == 8;
                if (in->op == IR_NEGATE)          { if (wide) put8(c, 0x48); put8(c, 0xF7); put8(c, 0xD8); }   // neg eax/rax
                else if (in->op == IR_COMPLEMENT) { if (wide) put8(c, 0x48); put8(c, 0xF7); put8(c, 0xD0); }   // not eax/rax
                else {                                                                    // IR_NOT -> int 0/1
                    if (val_size(in->a, fn) == 1) { put8(c, 0x84); put8(c, 0xC0); }        // test al, al
                    else if (val_size(in->a, fn) == 2) { put8(c, 0x66); put8(c, 0x85); put8(c, 0xC0); }   // test ax, ax
                    else { if (wide) put8(c, 0x48); put8(c, 0x85); put8(c, 0xC0); }        // test eax/rax
                    put8(c, 0x0F); put8(c, 0x94); put8(c, 0xC0);
                    put8(c, 0x0F); put8(c, 0xB6); put8(c, 0xC0);
                }
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_BINARY: {
                if (val_is_double(in->a, fn)) {                  // operands share the common type
                    load_xmm(c, base, 0, in->a);             // xmm0 = a
                    load_xmm(c, base, 1, in->u.b);                 // xmm1 = b
                    u8 arith = in->binop == IR_ADD ? 0x58 : in->binop == IR_SUBTRACT ? 0x5C
                             : in->binop == IR_MULTIPLY ? 0x59 : in->binop == IR_DIVIDE ? 0x5E : 0;
                    if (arith) {
                        sse_op(c, 0xF2, arith, 0, 1);
                        store_xmm_mem(c, 0, base + in->dst * 8);
                        break;
                    }
                    // a comparison -> int 0/1, NaN-aware. For < and <=, swapping operands lets a single
                    // unsigned setcc give the right answer (unordered -> false).
                    Bool swap = in->binop == IR_LESS || in->binop == IR_LESS_EQUAL;
                    sse_op(c, 0x66, 0x2F, swap ? 1 : 0, swap ? 0 : 1);   // comisd
                    if (in->binop == IR_EQUAL) {
                        put8(c, 0x0F); put8(c, 0x94); put8(c, 0xC0);     // sete  al  (equal)
                        put8(c, 0x0F); put8(c, 0x9B); put8(c, 0xC1);     // setnp cl  (ordered)
                        put8(c, 0x20); put8(c, 0xC8);                    // and al, cl
                    } else if (in->binop == IR_NOT_EQUAL) {
                        put8(c, 0x0F); put8(c, 0x95); put8(c, 0xC0);     // setne al
                        put8(c, 0x0F); put8(c, 0x9A); put8(c, 0xC1);     // setp  cl  (unordered)
                        put8(c, 0x08); put8(c, 0xC8);                    // or al, cl
                    } else {
                        u8 setcc = (in->binop == IR_LESS || in->binop == IR_GREATER) ? 0x97 : 0x93;  // seta : setae
                        put8(c, 0x0F); put8(c, setcc); put8(c, 0xC0);
                    }
                    put8(c, 0x0F); put8(c, 0xB6); put8(c, 0xC0);         // movzx eax, al
                    store_reg(c, base, REG_EAX, in->dst, fn);
                    break;
                }
                load_reg(c, base, REG_EAX, in->a, fn);
                load_reg(c, base, REG_ECX, in->u.b, fn);
                Bool wide = val_size(in->a, fn) == 8;   // both operands share the common type
                u8 setcc = 0;
                u8 result = REG_EAX;
                switch (in->binop) {
                    case IR_ADD:           if (wide) put8(c, 0x48); put8(c, 0x01); put8(c, 0xC8); break;
                    case IR_SUBTRACT:      if (wide) put8(c, 0x48); put8(c, 0x29); put8(c, 0xC8); break;
                    case IR_MULTIPLY:      if (wide) put8(c, 0x48); put8(c, 0x0F); put8(c, 0xAF); put8(c, 0xC1); break;
                    case IR_BIT_AND:       if (wide) put8(c, 0x48); put8(c, 0x21); put8(c, 0xC8); break;
                    case IR_BIT_OR:        if (wide) put8(c, 0x48); put8(c, 0x09); put8(c, 0xC8); break;
                    case IR_BIT_XOR:       if (wide) put8(c, 0x48); put8(c, 0x31); put8(c, 0xC8); break;
                    case IR_LSHIFT:        if (wide) put8(c, 0x48); put8(c, 0xD3); put8(c, 0xE0); break;
                    case IR_RSHIFT:        if (wide) put8(c, 0x48); put8(c, 0xD3); put8(c, in->is_unsigned ? 0xE8 : 0xF8); break;  // shr : sar
                    case IR_DIVIDE:
                    case IR_REMAINDER:
                        if (in->is_unsigned) { put8(c, 0x31); put8(c, 0xD2);                              // xor edx, edx (rdx = 0)

                            if (wide) { put8(c, 0x48); } put8(c, 0xF7); put8(c, 0xF1); }   // div ecx/rcx
                        else { if (wide) { put8(c, 0x48); put8(c, 0x99); put8(c, 0x48); put8(c, 0xF7); put8(c, 0xF9); }   // cqo; idiv rcx
                               else      { put8(c, 0x99); put8(c, 0xF7); put8(c, 0xF9); } }                              // cdq; idiv ecx
                        if (in->binop == IR_REMAINDER) result = REG_EDX;
                        break;
                    case IR_EQUAL:         setcc = 0x94; break;
                    case IR_NOT_EQUAL:     setcc = 0x95; break;
                    case IR_LESS:          setcc = in->is_unsigned ? 0x92 : 0x9C; break;  // setb  : setl
                    case IR_LESS_EQUAL:    setcc = in->is_unsigned ? 0x96 : 0x9E; break;  // setbe : setle
                    case IR_GREATER:       setcc = in->is_unsigned ? 0x97 : 0x9F; break;  // seta  : setg
                    case IR_GREATER_EQUAL: setcc = in->is_unsigned ? 0x93 : 0x9D; break;  // setae : setge
                }
                if (setcc) {
                    if (wide) put8(c, 0x48);
                    put8(c, 0x39); put8(c, 0xC8);            // cmp eax/rax, ecx/rcx
                    put8(c, 0x0F); put8(c, setcc); put8(c, 0xC0);
                    put8(c, 0x0F); put8(c, 0xB6); put8(c, 0xC0);
                }
                store_reg(c, base, result, in->dst, fn);
                break;
            }
            case IR_COPY:
                load_reg(c, base, REG_EAX, in->a, fn);
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            case IR_JUMP:
                put8(c, 0xE9);
                add_fixup(fixups, &fixup_count, c->pos, in->u.label);
                put32(c, 0);
                break;
            case IR_JUMP_IF_ZERO:
            case IR_JUMP_IF_NOT_ZERO:
                if (val_is_double(in->a, fn)) {                  // truthy = (a != 0.0) || NaN -> al
                    load_xmm(c, base, 0, in->a);
                    sse_op(c, 0x66, 0x57, 1, 1);                 // xorpd xmm1, xmm1
                    sse_op(c, 0x66, 0x2F, 0, 1);                 // comisd xmm0, xmm1
                    put8(c, 0x0F); put8(c, 0x95); put8(c, 0xC0); // setne al
                    put8(c, 0x0F); put8(c, 0x9A); put8(c, 0xC1); // setp  cl
                    put8(c, 0x08); put8(c, 0xC8);                // or al, cl
                    put8(c, 0x84); put8(c, 0xC0);                // test al, al
                } else {
                    load_reg(c, base, REG_EAX, in->a, fn);
                    if (val_size(in->a, fn) == 1) { put8(c, 0x84); put8(c, 0xC0); }   // test al, al
                    else if (val_size(in->a, fn) == 2) { put8(c, 0x66); put8(c, 0x85); put8(c, 0xC0); }   // test ax, ax
                    else { if (val_size(in->a, fn) == 8) put8(c, 0x48); put8(c, 0x85); put8(c, 0xC0); }
                }
                put8(c, 0x0F); put8(c, in->kind == IR_JUMP_IF_ZERO ? 0x84 : 0x85);   // jz : jnz
                add_fixup(fixups, &fixup_count, c->pos, in->u.label);
                put32(c, 0);
                break;
            case IR_LABEL:
                label_pos[in->u.label] = c->pos;
                break;
            case IR_FUNCALL: {
                // classify each argument, then place: stack arguments first (rax/xmm0 are free scratch),
                // then register arguments. Each outgoing stack slot is 8 bytes (a full store covers int,
                // pointer, and double alike). A double goes in an xmm register/slot, an int in a gp one.
                for (u32 a = 0; a < in->u.fun.arg_count; a++) is_dbl[a] = val_is_double(ir_arg_at(fn, in->u.fun.arg_start + a), fn);
                place_args(abi, is_dbl, in->u.fun.arg_count, place);
                for (u32 a = 0; a < in->u.fun.arg_count; a++) {
                    if (!place[a].on_stack) continue;
                    IrVal v = ir_arg_at(fn, in->u.fun.arg_start + a);
                    if (is_dbl[a]) { load_xmm(c, base, 0, v); store_xmm_mem(c, 0, shadow + place[a].stack_slot * 8); }
                    else { load_reg(c, base, REG_EAX, v, fn); store_mem(c, REG_EAX, shadow + place[a].stack_slot * 8, 1); }
                }
                for (u32 a = 0; a < in->u.fun.arg_count; a++) {
                    if (place[a].on_stack) continue;
                    IrVal v = ir_arg_at(fn, in->u.fun.arg_start + a);
                    if (place[a].is_xmm) load_xmm(c, base, place[a].reg, v);
                    else                 load_reg(c, base, place[a].reg, v, fn);
                }
                u32 imp = program_import_of(prog, in->u.fun.callee);
                if (imp != PROGRAM_NO_IMPORT) {                  // call an imported function via its IAT slot
                    size_t k = c->pos;
                    put8(c, 0xFF); put8(c, 0x15);                // call qword ptr [rip+disp32]
                    u32 slot = abi->iat_base_rva + abi->import_iat_slot[imp] * 8;
                    put32(c, (u32)(slot - (abi->code_base_rva + (u32)k + 6)));
                } else {
                    put8(c, 0xE8);                               // call rel32 (patched once all functions are laid out)
                    add_call_fixup(call_fixups, c->pos, in->u.fun.callee);
                    put32(c, 0);
                }
                if (ir_temp_is_double(fn, in->dst)) store_xmm_mem(c, 0, base + in->dst * 8);  // double result in xmm0
                else                                   store_reg(c, base, REG_EAX, in->dst, fn);  // result in eax
                break;
            }
            case IR_CALL_PTR: {
                // identical argument placement to IR_FUNCALL, but the target is a runtime pointer
                // value (in->a) -- loaded into r11 (volatile, never an argument register) and called.
                for (u32 a = 0; a < in->u.fun.arg_count; a++) is_dbl[a] = val_is_double(ir_arg_at(fn, in->u.fun.arg_start + a), fn);
                place_args(abi, is_dbl, in->u.fun.arg_count, place);
                for (u32 a = 0; a < in->u.fun.arg_count; a++) {
                    if (!place[a].on_stack) continue;
                    IrVal v = ir_arg_at(fn, in->u.fun.arg_start + a);
                    if (is_dbl[a]) { load_xmm(c, base, 0, v); store_xmm_mem(c, 0, shadow + place[a].stack_slot * 8); }
                    else { load_reg(c, base, REG_EAX, v, fn); store_mem(c, REG_EAX, shadow + place[a].stack_slot * 8, 1); }
                }
                for (u32 a = 0; a < in->u.fun.arg_count; a++) {
                    if (place[a].on_stack) continue;
                    IrVal v = ir_arg_at(fn, in->u.fun.arg_start + a);
                    if (place[a].is_xmm) load_xmm(c, base, place[a].reg, v);
                    else                 load_reg(c, base, place[a].reg, v, fn);
                }
                load_reg(c, base, 11, in->a, fn);               // r11 = the function-pointer value
                put8(c, 0x41); put8(c, 0xFF); put8(c, 0xD3);    // call r11
                if (ir_temp_is_double(fn, in->dst)) store_xmm_mem(c, 0, base + in->dst * 8);
                else                                   store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_ADDROF_FUNC: {
                // rax = the function's code address (lea rax, [rip+disp32]); the rel32 is patched once
                // all functions are laid out, exactly like a direct call's target.
                put8(c, 0x48); put8(c, 0x8D); put8(c, 0x05);    // lea rax, [rip+disp32]
                add_call_fixup(call_fixups, c->pos, in->u.fun.callee);
                put32(c, 0);
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_RETURN:
                if (val_is_double(in->a, fn)) load_xmm(c, base, 0, in->a);  // double result in xmm0
                else                          load_reg(c, base, REG_EAX, in->a, fn);
                put8(c, 0x48); put8(c, 0x81); put8(c, 0xC4); put32(c, frame);   // add rsp, frame
                put8(c, 0xC3);                                                  // ret
                break;
            case IR_LOAD_STATIC: {
                u32 addr = abi->data_base_rva + program_static_of(prog, in->u.fun.callee);
                if (ir_temp_size(fn, in->dst) == 8) put8(c, 0x48);        // REX.W for an 8-byte (long) static
                size_t k = c->pos;
                put8(c, 0x8B); put8(c, 0x05);                // mov eax/rax, [rip+disp32]
                put32(c, (u32)(addr - (abi->code_base_rva + (u32)k + 6)));
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_STORE_STATIC: {
                load_reg(c, base, REG_EAX, in->a, fn);
                u32 addr = abi->data_base_rva + program_static_of(prog, in->u.fun.callee);
                if (val_size(in->a, fn) == 8) put8(c, 0x48);   // REX.W for an 8-byte (long) static
                size_t k = c->pos;
                put8(c, 0x89); put8(c, 0x05);                // mov [rip+disp32], eax/rax
                put32(c, (u32)(addr - (abi->code_base_rva + (u32)k + 6)));
                break;
            }
            case IR_ADDROF:
                lea_local(c, REG_EAX, base + ir_temp(in->a) * 8);   // rax = &(local temp a)'s slot
                store_reg(c, base, REG_EAX, in->dst, fn);       // dst is a pointer temp (8-byte)
                break;
            case IR_ADDROF_STATIC: {
                u32 addr = abi->data_base_rva + program_static_of(prog, in->u.fun.callee);
                put8(c, 0x48);
                size_t k = c->pos;
                put8(c, 0x8D); put8(c, 0x05);                // lea rax, [rip+disp32]
                put32(c, (u32)(addr - (abi->code_base_rva + (u32)k + 6)));
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_CLZ:                                  // __builtin_clzll: 63 - bsr(a), result int
                load_reg(c, base, REG_EAX, in->a, fn);       // rax = operand (8-byte)
                put8(c, 0x48); put8(c, 0x0F); put8(c, 0xBD); put8(c, 0xC0);   // bsr rax, rax (index of MSB)
                put8(c, 0xB9); put32(c, 63);                 // mov ecx, 63
                put8(c, 0x29); put8(c, 0xC1);                // sub ecx, eax
                store_reg(c, base, REG_ECX, in->dst, fn);    // dst (int) = 63 - bsr
                break;
            case IR_LOAD: {
                load_reg(c, base, REG_EAX, in->a, fn);                  // rax = pointer
                u8 sz = ir_temp_size(fn, in->dst);
                if (sz == 1) { put8(c, 0x8A); put8(c, 0x00); }          // mov al, [rax]
                else if (sz == 2) { put8(c, 0x66); put8(c, 0x8B); put8(c, 0x00); }   // mov ax, [rax]
                else { if (sz == 8) put8(c, 0x48); put8(c, 0x8B); put8(c, 0x00); }   // mov eax/rax, [rax]
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_STORE: {
                load_reg(c, base, REG_EAX, in->a, fn);                  // rax = pointer
                load_reg(c, base, REG_ECX, in->u.b, fn);                  // ecx/rcx = value
                u8 sz = val_size(in->u.b, fn);
                if (sz == 1) { put8(c, 0x88); put8(c, 0x08); }          // mov [rax], cl
                else if (sz == 2) { put8(c, 0x66); put8(c, 0x89); put8(c, 0x08); }   // mov [rax], cx
                else { if (sz == 8) put8(c, 0x48); put8(c, 0x89); put8(c, 0x08); }   // mov [rax], ecx/rcx
                break;
            }
            case IR_COPY_BLOCK:                                      // memcpy(a, b, dst-bytes) via rep movsb
                load_reg(c, base, REG_EDI, in->a, fn);                  // rdi = dst address
                load_reg(c, base, REG_ESI, in->u.b, fn);                  // rsi = src address
                put8(c, 0xB9); put32(c, in->dst);                     // mov ecx, byte count (in dst; zero-extends to rcx)
                put8(c, 0xF3); put8(c, 0xA4);                          // rep movsb
                break;
            case IR_SIGN_EXTEND: {
                load_reg(c, base, REG_EAX, in->a, fn);              // al (char), ax (short) or eax (int)
                u8 ssz = val_size(in->a, fn);
                if (ssz == 1 || ssz == 2) {                        // char/short -> wider: movsx eax/rax, al/ax
                    if (ir_temp_size(fn, in->dst) == 8) put8(c, 0x48);
                    put8(c, 0x0F); put8(c, ssz == 1 ? 0xBE : 0xBF); put8(c, 0xC0);
                } else { put8(c, 0x48); put8(c, 0x63); put8(c, 0xC0); }   // movsxd rax, eax
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            }
            case IR_ZERO_EXTEND: {
                load_reg(c, base, REG_EAX, in->a, fn);
                u8 zsz = val_size(in->a, fn);
                if (zsz == 1)      { put8(c, 0x0F); put8(c, 0xB6); put8(c, 0xC0); }   // movzx eax, al (zeroes rax)
                else if (zsz == 2) { put8(c, 0x0F); put8(c, 0xB7); put8(c, 0xC0); }   // movzx eax, ax (zeroes rax)
                store_reg(c, base, REG_EAX, in->dst, fn);          // 4-byte load already zeroed the high half
                break;
            }
            case IR_TRUNCATE:       // store_reg writes only the dst's width (1 byte for char, 4 for int)
                load_reg(c, base, REG_EAX, in->a, fn);
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            case IR_INT_TO_DOUBLE:                  // signed int/long -> double: cvtsi2sd xmm0, eax/rax
                load_reg(c, base, REG_EAX, in->a, fn);
                put8(c, 0xF2); if (val_size(in->a, fn) == 8) put8(c, 0x48);
                put8(c, 0x0F); put8(c, 0x2A); put8(c, 0xC0);
                store_xmm_mem(c, 0, base + in->dst * 8);
                break;
            case IR_DOUBLE_TO_INT:                  // double -> signed int/long: cvttsd2si eax/rax, xmm0
                load_xmm(c, base, 0, in->a);
                put8(c, 0xF2); if (ir_temp_size(fn, in->dst) == 8) put8(c, 0x48);
                put8(c, 0x0F); put8(c, 0x2C); put8(c, 0xC0);
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            case IR_UINT_TO_DOUBLE:
                load_reg(c, base, REG_EAX, in->a, fn);   // rax = source (a 4-byte load zero-extends)
                if (val_size(in->a, fn) == 4) {          // uint fits in signed 64: cvtsi2sd xmm0, rax
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2A); put8(c, 0xC0);
                } else {                                 // ulong: halve-and-double when the top bit is set
                    put8(c, 0x48); put8(c, 0x85); put8(c, 0xC0);                 // test rax, rax
                    put8(c, 0x79); size_t js = c->pos; put8(c, 0);               // jns small
                    put8(c, 0x48); put8(c, 0x89); put8(c, 0xC1);                 // mov rcx, rax
                    put8(c, 0x48); put8(c, 0xD1); put8(c, 0xE9);                 // shr rcx, 1
                    put8(c, 0x48); put8(c, 0x83); put8(c, 0xE0); put8(c, 0x01);  // and rax, 1
                    put8(c, 0x48); put8(c, 0x09); put8(c, 0xC1);                 // or rcx, rax
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2A); put8(c, 0xC1);  // cvtsi2sd xmm0, rcx
                    sse_op(c, 0xF2, 0x58, 0, 0);                                 // addsd xmm0, xmm0
                    put8(c, 0xEB); size_t jd = c->pos; put8(c, 0);               // jmp done
                    size_t small = c->pos;
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2A); put8(c, 0xC0);  // cvtsi2sd xmm0, rax
                    size_t done = c->pos;
                    seek(c, js); put8(c, (u8)(small - (js + 1)));
                    seek(c, jd); put8(c, (u8)(done - (jd + 1)));
                    seek(c, done);
                }
                store_xmm_mem(c, 0, base + in->dst * 8);
                break;
            case IR_DOUBLE_TO_UINT:
                load_xmm(c, base, 0, in->a);
                if (ir_temp_size(fn, in->dst) == 4) { // uint: cvttsd2si to 64-bit, store low 32
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2C); put8(c, 0xC0);   // cvttsd2si rax, xmm0
                } else {                                 // ulong: subtract 2^63 when >= 2^63
                    put8(c, 0x48); put8(c, 0xB8); put64(c, 0x43E0000000000000ull);   // movabs rax, (double)2^63
                    movq_gpr_to_xmm(c, 1, REG_EAX);                              // xmm1 = 2^63
                    sse_op(c, 0x66, 0x2F, 0, 1);                                 // comisd xmm0, xmm1
                    put8(c, 0x73); size_t jb = c->pos; put8(c, 0);               // jae big
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2C); put8(c, 0xC0);   // cvttsd2si rax, xmm0
                    put8(c, 0xEB); size_t jd = c->pos; put8(c, 0);               // jmp done
                    size_t big = c->pos;
                    sse_op(c, 0xF2, 0x5C, 0, 1);                                 // subsd xmm0, xmm1
                    put8(c, 0xF2); put8(c, 0x48); put8(c, 0x0F); put8(c, 0x2C); put8(c, 0xC0);   // cvttsd2si rax, xmm0
                    put8(c, 0x48); put8(c, 0xB9); put64(c, 0x8000000000000000ull);   // movabs rcx, 2^63
                    put8(c, 0x48); put8(c, 0x01); put8(c, 0xC8);                 // add rax, rcx
                    size_t done = c->pos;
                    seek(c, jb); put8(c, (u8)(big - (jb + 1)));
                    seek(c, jd); put8(c, (u8)(done - (jd + 1)));
                    seek(c, done);
                }
                store_reg(c, base, REG_EAX, in->dst, fn);
                break;
            case IR_ASM:
                for (u32 i = 0; i < in->u.asm_.op_count; i++) {     // load inputs into their registers
                    AsmOperand op = ir_asm_op_at(fn, in->u.asm_.op_start + i);
                    if (!op.is_out) load_reg(c, base, op.reg, op.val, fn);
                }
                for (u32 i = 0; i < in->u.asm_.byte_count; i++)     // emit the assembled instruction bytes
                    put8(c, ir_asm_byte_at(fn, in->u.asm_.byte_start + i));
                for (u32 i = 0; i < in->u.asm_.op_count; i++) {     // store outputs from their registers
                    AsmOperand op = ir_asm_op_at(fn, in->u.asm_.op_start + i);
                    if (op.is_out) store_reg(c, base, op.reg, op.temp, fn);
                }
                break;
        }
    }

    // patch this function's jumps (positions are absolute in the code buffer), then resume at the end
    size_t end = c->pos;
    for (size_t i = 0; i < fixup_count; i++) {
        Fixup* f = &fixups[i];
        seek(c, f->site);
        put32(c, (u32)(label_pos[f->label] - (f->site + 4)));
    }
    seek(c, end);
    // label_pos lives in the caller's scratch arena; gen_code resets it after this function
}

size_t gen_code_x86_64(const Abi* abi, const Program* prog, Pages* code, u32* func_offsets)
{
    Cursor c = { code, 0 };

    // borrow the global arena: func_off lives the whole pass; each function's label_pos is freed after it
    ArenaPosition arena_entry = arena_position(&global_arena);

    // func_id -> code offset (SENTINEL until emitted); call sites patched once all are known
    size_t map_n = prog->func_id_count ? prog->func_id_count : 1;
    size_t* func_off = ARENA_ALLOC(&global_arena, map_n * sizeof(size_t));
    for (u32 i = 0; i < prog->func_id_count; i++) func_off[i] = (size_t)-1;
    ArenaPosition after_func_off = arena_position(&global_arena);

    StableList call_fixups = STABLE_LIST_INIT(CallFixup);

    // entry stub at offset 0: align the stack, call main, then exit with its return value. At process
    // entry Win64 has rsp%16==8 (as if called) while SysV's _start has rsp%16==0; in both cases set rsp
    // so the `call` enters main with rsp%16==8 -- the alignment gen_function's frame math assumes (so
    // 16-aligned locals like arrays land correctly). Win64 also leaves 0x20 of shadow for the callee.
    if (abi->kind == ABI_WIN64) {
        put8(&c, 0x48); put8(&c, 0x83); put8(&c, 0xEC); put8(&c, 0x28);   // sub rsp, 0x28 (shadow + align)
    } else {
        // SysV _start: argc is at [rsp] and argv begins at rsp+8 (on the stack, not in registers --
        // the kernel zeroes the GPRs). Hand them to main as the first two args (rdi=argc, rsi=argv)
        // BEFORE aligning rsp, which would otherwise lose the layout.
        put8(&c, 0x48); put8(&c, 0x8B); put8(&c, 0x3C); put8(&c, 0x24);              // mov  rdi, [rsp]   (argc)
        put8(&c, 0x48); put8(&c, 0x8D); put8(&c, 0x74); put8(&c, 0x24); put8(&c, 0x08);  // lea rsi, [rsp+8] (argv)
        put8(&c, 0x48); put8(&c, 0x83); put8(&c, 0xE4); put8(&c, 0xF0);              // and  rsp, -16    (align to 16)
    }
    put8(&c, 0xE8);                                                   // call main (rel32)
    add_call_fixup(&call_fixups, c.pos, prog->main_func_id);
    put32(&c, 0);
    emit_exit(&c, abi);

    for (size_t fi = 0; fi < prog->func_count; fi++) {
        const IrFunc* fn = program_func_at(prog, fi);
        if (!program_used(prog, fn->func_id)) continue;   // DCE: skip unreachable functions
        func_off[fn->func_id] = c.pos;
        gen_function(&c, abi, prog, fn, &call_fixups);
        arena_reset(&global_arena, after_func_off);   // free this function's label_pos, keep func_off
    }

    size_t code_len = c.pos;

    // patch every call's rel32 now that each function's offset is known
    STABLE_LIST_FOREACH(it, &call_fixups) {
        CallFixup* f = it.elem;
        size_t target = func_off[f->callee];
        if (target == (size_t)-1) { LOG_STRING("codegen: call to an undefined function"); process_exit(72); }
        seek(&c, f->site);
        put32(&c, (u32)(target - (f->site + 4)));
    }
    stablelist_deinit(&call_fixups);

    if (func_offsets)   // hand each function's code offset back (for .data->code relocations)
        for (u32 i = 0; i < prog->func_id_count; i++) func_offsets[i] = (u32)func_off[i];

    arena_reset(&global_arena, arena_entry);   // return all codegen scratch to the global arena
    return code_len;
}
