#include "parse.h"

#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "ir.h"
#include "lex.h"
#include "namemap.h"

// the type kinds, as an X-macro so TYPE_KIND_COUNT (below) tracks the list automatically
#define X_TYPE_KINDS \
    X(VOID) X(INT) X(UINT) X(LONG) X(ULONG) X(SHORT) X(USHORT) X(CHAR) X(SCHAR) X(UCHAR) \
    X(DOUBLE) X(POINTER) X(FUNPTR) X(ARRAY) X(STRUCT) X(UNION)

typedef enum {
    #define X(name) TYK_##name,
    X_TYPE_KINDS
    #undef X
} TypeKind;

enum {   // a parallel tally enum (like target.h's), so TYPE_KIND_COUNT == number of kinds
    #define X(name) TYK_TALLY_##name,
    X_TYPE_KINDS
    #undef X
    TYPE_KIND_COUNT
};
typedef const struct struct_TypeNode* Type;
typedef struct struct_TypeNode {
    TypeKind kind;
    Type referenced;
    u64 count;
    const struct struct_StructDef* sdef;
} TypeNode;   // count: TYK_ARRAY length; sdef: TYK_STRUCT/UNION

// A struct/union definition: its tag, members (name/type/byte offset), total size and alignment. It is
// mutable and stably allocated, and interned by tag, so a forward declaration completes in place (every
// TypeNode for `struct Tag` points at the same StructDef and then sees the completed layout).
typedef struct { size_t name_off, name_len; Type type; u64 offset; } Member;
typedef struct struct_StructDef {
    size_t tag_off, tag_len;   // tag name span in source (0 len = anonymous)
    Bool is_union;
    Bool complete;             // members/size/align valid once the `{ ... }` body is parsed
    Bool in_scope;             // its declaring block is still open (TypeNodes outlive this; we only stop finding it)
    u64 size;
    u32 align;
    Member* members;           // arena array, member_count entries
    u32 member_count;
} StructDef;

static const TypeNode type_node_void   = { TYK_VOID,   0, 0, 0 };
static const TypeNode type_node_int    = { TYK_INT,    0, 0, 0 };
static const TypeNode type_node_uint   = { TYK_UINT,   0, 0, 0 };
static const TypeNode type_node_long   = { TYK_LONG,   0, 0, 0 };
static const TypeNode type_node_ulong  = { TYK_ULONG,  0, 0, 0 };
static const TypeNode type_node_short  = { TYK_SHORT,  0, 0, 0 };
static const TypeNode type_node_ushort = { TYK_USHORT, 0, 0, 0 };
static const TypeNode type_node_char   = { TYK_CHAR,   0, 0, 0 };
static const TypeNode type_node_schar  = { TYK_SCHAR,  0, 0, 0 };
static const TypeNode type_node_uchar  = { TYK_UCHAR,  0, 0, 0 };
static const TypeNode type_node_double = { TYK_DOUBLE, 0, 0, 0 };
#define TY_VOID   (&type_node_void)
#define TY_INT    (&type_node_int)
#define TY_UINT   (&type_node_uint)
#define TY_LONG   (&type_node_long)
#define TY_ULONG  (&type_node_ulong)
#define TY_SHORT  (&type_node_short)
#define TY_USHORT (&type_node_ushort)
#define TY_CHAR   (&type_node_char)
#define TY_SCHAR  (&type_node_schar)
#define TY_UCHAR  (&type_node_uchar)
#define TY_DOUBLE (&type_node_double)

static u64 type_size(Type t)
{
    switch (t->kind) {
        case TYK_POINTER: case TYK_FUNPTR: case TYK_LONG: case TYK_ULONG: case TYK_DOUBLE: return 8;
        case TYK_INT:   case TYK_UINT:   return 4;
        case TYK_SHORT: case TYK_USHORT: return 2;
        case TYK_CHAR:  case TYK_SCHAR:  case TYK_UCHAR: return 1;
        case TYK_VOID:  return 0;                                  // incomplete; callers reject sizeof(void)
        case TYK_ARRAY: return t->count * type_size(t->referenced);   // u64: a huge array's sizeof can exceed 2^32
        case TYK_STRUCT: case TYK_UNION: return t->sdef->size;
    }
    UNREACHABLE();
}
static u32 type_align(Type t)
{
    if (t->kind == TYK_ARRAY)  return type_align(t->referenced);
    if (t->kind == TYK_STRUCT || t->kind == TYK_UNION) return t->sdef->align;
    return (u32)type_size(t);   // scalars are aligned to their size (1/4/8); void's 0 never reached
}
// per-TypeKind trait bits, so a "is this kind one of a set" test is a single table lookup. CHAR/SHORT
// kinds carry NARROW (integer-promote to int); the char kinds additionally carry CHAR.
#define TYPE_UNSIGNED  0x01
#define TYPE_AGGREGATE 0x02   // address-valued (array/struct/union); not loaded into a register
#define TYPE_CHAR      0x04
#define TYPE_NARROW    0x08
static const u8 type_kind_traits[TYPE_KIND_COUNT] = {
    [TYK_UINT]  = TYPE_UNSIGNED,
    [TYK_ULONG] = TYPE_UNSIGNED,
    [TYK_SHORT]  = TYPE_NARROW,
    [TYK_USHORT] = TYPE_UNSIGNED | TYPE_NARROW,
    [TYK_CHAR]  = TYPE_CHAR | TYPE_NARROW,
    [TYK_SCHAR] = TYPE_CHAR | TYPE_NARROW,
    [TYK_UCHAR] = TYPE_UNSIGNED | TYPE_CHAR | TYPE_NARROW,
    [TYK_ARRAY]  = TYPE_AGGREGATE,
    [TYK_STRUCT] = TYPE_AGGREGATE,
    [TYK_UNION]  = TYPE_AGGREGATE,
};
static Bool type_unsigned(Type t)   { return (type_kind_traits[t->kind] & TYPE_UNSIGNED) != 0; }
static Bool type_is_double(Type t)  { return t->kind == TYK_DOUBLE; }
static Bool type_is_pointer(Type t) { return t->kind == TYK_POINTER; }
static Bool type_is_array(Type t)   { return t->kind == TYK_ARRAY; }
static Bool type_is_void(Type t)    { return t->kind == TYK_VOID; }
static Bool type_is_struct(Type t)  { return t->kind == TYK_STRUCT || t->kind == TYK_UNION; }
static Bool type_is_aggregate(Type t){ return (type_kind_traits[t->kind] & TYPE_AGGREGATE) != 0; }
static Bool type_is_scalar(Type t)  { return !type_is_void(t) && !type_is_aggregate(t); }   // an arithmetic or pointer value (after array decay)
static Bool type_is_incomplete(Type t) { return type_is_void(t) || (type_is_struct(t) && !t->sdef->complete); }   // no known size/layout
static Bool type_is_void_ptr(Type t){ return t->kind == TYK_POINTER && t->referenced->kind == TYK_VOID; }
static Type referenced(Type t)      { return t->referenced; }
static Type pointer_to(Type referee)
{
    TypeNode* n = ARENA_ALLOC(&global_arena, sizeof(TypeNode));
    n->kind = TYK_POINTER; n->referenced = referee; n->count = 0; n->sdef = 0;
    return n;
}
// a pointer-to-function value: an 8-byte pointer whose `referenced` is the function's return type.
// cc doesn't model a function's parameter types, so indirect calls aren't argument-type-checked.
static Type funptr_type(Type ret)
{
    TypeNode* n = ARENA_ALLOC(&global_arena, sizeof(TypeNode));
    n->kind = TYK_FUNPTR; n->referenced = ret; n->count = 0; n->sdef = 0;
    return n;
}
static Bool type_is_funptr(Type t) { return t->kind == TYK_FUNPTR; }
static Type array_of(Type element, u64 count)
{
    TypeNode* n = ARENA_ALLOC(&global_arena, sizeof(TypeNode));
    n->kind = TYK_ARRAY; n->referenced = element; n->count = count; n->sdef = 0;
    return n;
}
static Type struct_type(const StructDef* sd)
{
    TypeNode* n = ARENA_ALLOC(&global_arena, sizeof(TypeNode));
    n->kind = sd->is_union ? TYK_UNION : TYK_STRUCT; n->referenced = 0; n->count = 0; n->sdef = sd;
    return n;
}
static Bool same_type(Type a, Type b)
{
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    if (a->kind == TYK_POINTER) return same_type(a->referenced, b->referenced);
    if (a->kind == TYK_FUNPTR)  return same_type(a->referenced, b->referenced);   // lenient: return type only
    if (a->kind == TYK_ARRAY)   // an unspecified-size array (`T a[]`) is compatible with a sized one
        return (a->count == b->count || a->count == 0 || b->count == 0) && same_type(a->referenced, b->referenced);
    if (a->kind == TYK_STRUCT || a->kind == TYK_UNION) return a->sdef == b->sdef;   // interned by tag
    return 0;
}
static Type make_int_type(Bool is_long, Bool is_unsigned)
{
    return is_long ? (is_unsigned ? TY_ULONG : TY_LONG) : (is_unsigned ? TY_UINT : TY_INT);
}
static Bool type_is_char(Type t) { return (type_kind_traits[t->kind] & TYPE_CHAR) != 0; }
// a sub-int integer type (char or short): integer-promotes to int in arithmetic, and can't feed the
// SSE convert ops directly (they need a >= 32-bit operand), so char/short <-> double route through int.
static Bool type_is_narrow(Type t) { return (type_kind_traits[t->kind] & TYPE_NARROW) != 0; }

// usual arithmetic conversions (C 6.3.1.8): if either is double -> double; same size -> the unsigned
// one wins; else the larger. (Operands are integer-promoted first, so chars never reach here.)
static Type common_type(Type a, Type b)
{
    if (a == b) return a;
    if (type_is_double(a) || type_is_double(b)) return TY_DOUBLE;
    if (type_size(a) == type_size(b)) return type_unsigned(a) ? a : b;
    return type_size(a) > type_size(b) ? a : b;
}

// a parsed name with its type (a parameter, or any declared identifier)
typedef struct { size_t off; size_t len; Type type; } NameRef;

// a name declared in a block scope: a variable (its IR temp) or a block-scope function
// declaration (is_func; no temp). Both occupy the scope so a redeclaration as the other kind
// is caught.
typedef struct {
    size_t name_off;
    size_t name_len;
    u32 temp;
    Type type;           // the variable's type (int/long)
    Bool is_func;
    Bool is_static;   // the name binds to a static object (a block-scope `extern` or a local static)
    Bool no_linkage;  // is_static binding with no linkage (a local static), vs a block-scope extern
    Bool by_ref;      // a by-value aggregate parameter: `temp` HOLDS the struct's address (vs an inline
                      // array temp whose slot address IS the struct) -- reference it without an ADDROF
    u32 static_id;
    u8 asm_reg;       // a GCC local register variable (`__asm__("r10")`) is pinned to this register; 0xFF if none
} Var;

// a goto target: its source name, its IR label, and whether the labeled statement was seen
typedef struct {
    size_t name_off;
    size_t name_len;
    u32 ir_label;
    Bool defined;
} Label;

// one entry of a switch's case table
typedef struct {
    u32 value;
    u32 label;
} SwitchCase;

// the switch currently being parsed: cases are collected while parsing the body, then the
// dispatch (compare-and-jump for each case) is emitted afterward
typedef struct struct_SwitchCtx {
    SegList cases;
    size_t case_count;
    u32 default_label;
    Bool has_default;
} SwitchCtx;

// a declared identifier's linkage (C 6.2.2). Local statics have no linkage and are never named here.
typedef enum { LINK_INTERNAL, LINK_EXTERNAL } Linkage;

// a file-scope symbol (function or static variable); its id is its index in the parser's `funcs`
// table -- functions and statics share one namespace
typedef struct {
    size_t name_off;
    size_t name_len;
    GlobalKind kind;
    Type type;          // GLOBAL_STATIC: the variable's type; GLOBAL_FUNC: the return type
    u32 param_count;    // GLOBAL_FUNC
    u32 param_type_start;  // GLOBAL_FUNC: index of its parameter types in the parser's param_types pool
    const u8* init;     // GLOBAL_STATIC: materialized initializer image (init_len bytes); NULL => all zeros
    u32 init_len;       // GLOBAL_STATIC: the .data slot byte size (== type_size of the variable)
    const StaticReloc* relocs;   // GLOBAL_STATIC: function-address fixups in the image (NULL if none)
    u32 reloc_count;
    Bool defined;
    Bool internal;   // no linkage (a local static); never merged across declarations
    Linkage linkage;
    Bool file_scope_seen;  // a file-scope declaration of this name has been parsed (so it is
                              // name-resolvable); a block-scope `extern` alone does not set this
    Bool has_initializer;  // GLOBAL_STATIC: an initialized definition has been seen
} Global;

// a function declared with an empty `()` parameter list has K&R "unspecified" parameters: its calls are
// not argument-checked. param_count holds this sentinel until (if ever) a real prototype refines it.
#define PARAM_COUNT_UNSPECIFIED 0xFFFFFFFFu

typedef struct {
    const u8* src;
    size_t len;
    size_t off;          // where to lex the next token from
    Token cur;           // current lookahead token
    Token next;          // caches the token from peek() so it isn't re-lexed; saves ~65% of lex time
    Bool has_next;       // whether `next` holds a valid cached token
    TranslationUnit* program;
    SegList funcs;      // global function table (persists across functions)
    size_t func_count;
    NameMap global_index;  // name -> funcs id, for the name-resolvable (internal==0) entries -- O(1) lookup
    IrFunc cur_func;  // the function currently being built
    IrFunc* fn;       // == &cur_func; IR is emitted here
    SegList vars;       // declared variables, as a stack of scopes (per function)
    size_t var_count;
    size_t scope_start;  // index in `vars` where the innermost scope begins
    SegList labels;     // goto labels (separate namespace; function-scoped)
    size_t label_count;
    SegList break_targets;     // stack of break labels (innermost loop or switch)
    size_t break_depth;
    SegList continue_targets;  // stack of continue labels (innermost loop)
    size_t continue_depth;
    SwitchCtx* sw;       // the switch being parsed, or NULL
    SegList params;      // reused scratch for the parameter list being parsed (NameRef[])
    Bool params_unspecified;  // the just-parsed list was an empty `()` -- K&R unspecified params (calls unchecked)
    Type ret_type;       // the current function's return type
    SegList param_types;     // flat pool of parameter types (Type), indexed by Global.param_type_start
    size_t param_type_count;
    SegList struct_defs;     // StructDef[], interned by tag (stable: TypeNodes hold StructDef pointers)
    size_t struct_def_count;
    size_t struct_scope_start;  // index in struct_defs where the innermost block's tags begin
    SegList typedefs;        // TypedefName[], a stack of scopes (truncated on block exit, like vars)
    size_t typedef_count;
    SegList enum_consts;     // EnumConst[], a stack of scopes (truncated on block exit, like vars)
    size_t enum_const_count;
    SegList static_relocs;   // scratch: function-address fixups for the static definition being parsed
    size_t static_reloc_count;
    const char* err_msg; // first error (when failed)
    size_t err_off;
    Bool failed;
} Parser;

// the result of parsing an expression: its value, and whether it is an assignable lvalue
typedef struct {
    IrVal val;
    Bool is_lvalue;
    u32 lvalue_temp;     // local-variable lvalue: the variable's temp
    Bool is_static;   // file-scope static lvalue (val is a temp holding its loaded value)
    u32 static_id;
    Bool is_deref;       // pointer-target lvalue (val is the loaded value; deref_ptr is the address)
    IrVal deref_ptr;
    Type type;           // the expression's type
} ExpResult;

static ExpResult res_value_typed(IrVal v, Type t) { ExpResult r; r.val = v; r.is_lvalue = 0; r.lvalue_temp = 0; r.is_static = 0; r.static_id = 0; r.is_deref = 0; r.type = t; return r; }
static ExpResult res_value(IrVal v)  { return res_value_typed(v, TY_INT); }   // an int rvalue (or an error placeholder)
static ExpResult res_lvalue(u32 temp, Type t)   { ExpResult r; r.val = ir_var(temp); r.is_lvalue = 1; r.lvalue_temp = temp; r.is_static = 0; r.static_id = 0; r.is_deref = 0; r.type = t; return r; }
static ExpResult res_static(u32 temp, u32 id, Type t) { ExpResult r; r.val = ir_var(temp); r.is_lvalue = 1; r.lvalue_temp = temp; r.is_static = 1; r.static_id = id; r.is_deref = 0; r.type = t; return r; }
static ExpResult res_deref(IrVal ptr, u32 loaded, Type pointee) { ExpResult r; r.val = ir_var(loaded); r.is_lvalue = 1; r.lvalue_temp = 0; r.is_static = 0; r.static_id = 0; r.is_deref = 1; r.deref_ptr = ptr; r.type = pointee; return r; }

static Bool is_null_constant(ExpResult e) { return e.val.kind == IR_CONSTANT && e.val.value == 0 && !type_is_pointer(e.type); }

// an array value carries its base address in .val; using it as a value decays to a pointer-to-element
static ExpResult decay(ExpResult e) { return type_is_array(e.type) ? res_value_typed(e.val, pointer_to(referenced(e.type))) : e; }

// a fresh temp sized for type `t` (long/pointer -> 8-byte slot, int -> 4-byte)
static u32 temp_for(IrFunc* fn, Type t) { if (type_is_double(t)) return ir_new_double_temp(fn); u32 s = type_size(t); return s == 8 ? ir_new_ptr_temp(fn) : s == 2 ? ir_new_short_temp(fn) : s == 1 ? ir_new_byte_temp(fn) : ir_new_temp(fn); }

static void advance(Parser* p)
{
    if (p->has_next) { p->cur = p->next; p->has_next = 0; }   // reuse the token peek() already lexed
    else             p->cur = lex(p->src, p->len, p->off);
    p->off = p->cur.limit;
}

// the token after the current one, without consuming it (cached so advance() doesn't re-lex it)
static Token peek(Parser* p)
{
    if (!p->has_next) { p->next = lex(p->src, p->len, p->off); p->has_next = 1; }
    return p->next;
}

// record the first error; later calls are no-ops so the original site wins
static void fail(Parser* p, const char* msg)
{
    if (!p->failed) {
        p->failed = 1;
        p->err_msg = msg;
        p->err_off = p->cur.start;
    }
}

// an enumeration constant: an ordinary identifier bound to an int value, scoped like a variable
typedef struct { size_t name_off, name_len; i64 value; } EnumConst;

// a name introduced by `typedef`: an alias bound to a type, scoped like a variable
typedef struct { size_t name_off, name_len; Type type; } TypedefName;

// the visible typedef-name `off..len`, or 0 if that name isn't a typedef (most-recent declaration wins)
static Type typedef_find(const Parser* p, size_t off, size_t len)
{
    for (size_t i = p->typedef_count; i > 0; i--) {
        const TypedefName* td = SEG_LIST_REF(TypedefName, &p->typedefs, i - 1);
        if (td->name_len != len) continue;
        Bool match = 1;
        for (size_t k = 0; k < len; k++) if (p->src[td->name_off + k] != p->src[off + k]) { match = 0; break; }
        if (match) return td->type;
    }
    return 0;
}

// look up the enumeration constant `off..len`; on a hit, store its value in *out and return 1
static Bool enum_const_find(const Parser* p, size_t off, size_t len, i64* out)
{
    for (size_t i = p->enum_const_count; i > 0; i--) {
        const EnumConst* ec = SEG_LIST_REF(EnumConst, &p->enum_consts, i - 1);
        if (ec->name_len != len) continue;
        Bool match = 1;
        for (size_t k = 0; k < len; k++) if (p->src[ec->name_off + k] != p->src[off + k]) { match = 0; break; }
        if (match) { *out = ec->value; return 1; }
    }
    return 0;
}

// per-token-tag trait bits, so "is this tag one of a set" is a single table lookup (cf. lex.c's
// char_traits). TOK_STARTS_TYPE: a type-specifier keyword that begins a type name. TOK_ASSIGN: a
// plain or compound assignment operator. TOK_STORAGE: a storage-class specifier keyword.
#define TOK_STARTS_TYPE 0x01
#define TOK_ASSIGN      0x02
#define TOK_STORAGE     0x04
static const u8 token_traits[256] = {
    [TOKEN_KW_INT] = TOK_STARTS_TYPE,       [TOKEN_KW_LONG]   = TOK_STARTS_TYPE,
    [TOKEN_KW_SHORT] = TOK_STARTS_TYPE,     [TOKEN_KW_CHAR]   = TOK_STARTS_TYPE,
    [TOKEN_KW_DOUBLE] = TOK_STARTS_TYPE,    [TOKEN_KW_VOID]   = TOK_STARTS_TYPE,
    [TOKEN_KW_SIGNED] = TOK_STARTS_TYPE,    [TOKEN_KW_UNSIGNED] = TOK_STARTS_TYPE,
    [TOKEN_KW_STRUCT] = TOK_STARTS_TYPE,    [TOKEN_KW_UNION]  = TOK_STARTS_TYPE,
    [TOKEN_KW_ENUM] = TOK_STARTS_TYPE,      [TOKEN_KW_CONST]  = TOK_STARTS_TYPE,
    [TOKEN_KW_VOLATILE] = TOK_STARTS_TYPE,

    [TOKEN_ASSIGN] = TOK_ASSIGN,            [TOKEN_PLUS_ASSIGN]  = TOK_ASSIGN,
    [TOKEN_MINUS_ASSIGN] = TOK_ASSIGN,      [TOKEN_STAR_ASSIGN]  = TOK_ASSIGN,
    [TOKEN_SLASH_ASSIGN] = TOK_ASSIGN,      [TOKEN_PERCENT_ASSIGN] = TOK_ASSIGN,
    [TOKEN_AMP_ASSIGN] = TOK_ASSIGN,        [TOKEN_PIPE_ASSIGN]  = TOK_ASSIGN,
    [TOKEN_CARET_ASSIGN] = TOK_ASSIGN,      [TOKEN_LSHIFT_ASSIGN] = TOK_ASSIGN,
    [TOKEN_RSHIFT_ASSIGN] = TOK_ASSIGN,

    [TOKEN_KW_STATIC] = TOK_STORAGE,        [TOKEN_KW_EXTERN]   = TOK_STORAGE,
    [TOKEN_KW_REGISTER] = TOK_STORAGE,      [TOKEN_KW_TYPEDEF]  = TOK_STORAGE,
};

// does token `t` begin a type name (a cast or declaration)? -- a type-specifier keyword or a typedef-name
static Bool token_starts_type(const Parser* p, Token t) {
    if (t.tag == TOKEN_IDENTIFIER) return typedef_find(p, t.start, t.limit - t.start) != 0;
    return (token_traits[t.tag] & TOK_STARTS_TYPE) != 0;
}

// a reserved word can't be used as a variable or label name
static Bool is_reserved(const Parser* p) { return p->cur.tag >= TOKEN_KW_FIRST && p->cur.tag <= TOKEN_KW_LAST; }

// a declaration's storage class
typedef enum { SC_NONE, SC_STATIC, SC_EXTERN, SC_TYPEDEF } StorageClass;

// parse an optional storage-class specifier and the type, in any order ("static int", "int long",
// "long static a"). Type keywords: int and long (long/long int/int long/long long all == long).
// Consumes through the specifiers; sets *type; returns 0 on error.
static Type parse_struct_specifier(Parser* p);   // defined below; parses `struct/union [Tag] [{ ... }]`
static Type parse_enum_specifier(Parser* p);     // defined below; parses `enum [Tag] [{ ... }]`, type int

static Bool parse_specifiers(Parser* p, StorageClass* sc, Type* type)
{
    *sc = SC_NONE;
    Type struct_ty = 0, typedef_ty = 0;
    int int_count = 0, long_count = 0, short_count = 0, unsigned_count = 0, signed_count = 0, char_count = 0, double_count = 0, void_count = 0;
    for (;;) {
        u8 tag = p->cur.tag;
        switch (tag) {
        case TOKEN_KW_STRUCT: case TOKEN_KW_UNION:
            if (struct_ty) { fail(p, "two struct/union type specifiers"); return 0; }
            struct_ty = parse_struct_specifier(p);
            if (p->failed) return 0;
            break;
        case TOKEN_KW_ENUM:
            if (struct_ty) { fail(p, "two type specifiers"); return 0; }
            struct_ty = parse_enum_specifier(p);   // an enum is int-typed; reuse the struct_ty slot to forbid combining
            if (p->failed) return 0;
            break;
        case TOKEN_IDENTIFIER: {
            // a typedef-name is a type specifier, but only as the first/only one: once any other type
            // specifier is seen, a following identifier is the declarator name, not part of the type.
            if (struct_ty || typedef_ty || int_count || long_count || short_count || unsigned_count || signed_count || char_count || double_count || void_count) goto done;
            Type td = typedef_find(p, p->cur.start, p->cur.limit - p->cur.start);
            if (!td) goto done;
            typedef_ty = td; advance(p);
            break;
        }
        case TOKEN_KW_TYPEDEF:
            if (*sc != SC_NONE) { fail(p, "conflicting storage-class specifiers"); return 0; }
            *sc = SC_TYPEDEF; advance(p);
            break;
        case TOKEN_KW_INT:      int_count++;      advance(p); break;
        case TOKEN_KW_LONG:     long_count++;     advance(p); break;
        case TOKEN_KW_SHORT:    short_count++;    advance(p); break;
        case TOKEN_KW_UNSIGNED: unsigned_count++; advance(p); break;
        case TOKEN_KW_SIGNED:   signed_count++;   advance(p); break;
        case TOKEN_KW_CHAR:     char_count++;     advance(p); break;
        case TOKEN_KW_DOUBLE:   double_count++;   advance(p); break;
        case TOKEN_KW_VOID:     void_count++;     advance(p); break;
        case TOKEN_KW_STATIC: case TOKEN_KW_EXTERN: {
            StorageClass next = tag == TOKEN_KW_STATIC ? SC_STATIC : SC_EXTERN;
            if (*sc != SC_NONE) { fail(p, "conflicting storage-class specifiers"); return 0; }
            *sc = next; advance(p);
            break;
        }
        case TOKEN_KW_REGISTER:                         // `register` is only a hint; accept and ignore it
        case TOKEN_KW_INLINE:                           // `inline` is a hint cc doesn't act on
        case TOKEN_KW_CONST: case TOKEN_KW_VOLATILE:    // type qualifiers don't affect cc's codegen
            advance(p);
            break;
        default:
            goto done;
        }
    }
done:;
    if (typedef_ty) {
        *type = typedef_ty;   // a typedef-name is the sole type specifier (the loop guarantees no others)
        return 1;
    }
    if (struct_ty) {
        if (int_count || long_count || unsigned_count || signed_count || char_count || double_count || void_count) { fail(p, "cannot combine struct/union with another type specifier"); return 0; }
        *type = struct_ty;
        return 1;
    }
    if (int_count > 1 || long_count > 2 || short_count > 1 || unsigned_count > 1 || signed_count > 1 || char_count > 1 || double_count > 1 || void_count > 1) { fail(p, "invalid type specifier"); return 0; }
    if (unsigned_count && signed_count) { fail(p, "conflicting signed/unsigned specifiers"); return 0; }
    if (char_count && (int_count || long_count || short_count)) { fail(p, "invalid type specifier"); return 0; }
    if (short_count && (long_count || char_count)) { fail(p, "invalid type specifier"); return 0; }   // short combines only with int/signed/unsigned
    // double does not combine with any integer specifier (no `long double` support)
    if (double_count && (int_count || long_count || short_count || unsigned_count || signed_count || char_count)) { fail(p, "invalid type specifier"); return 0; }
    if (void_count && (int_count || long_count || short_count || unsigned_count || signed_count || char_count || double_count)) { fail(p, "invalid type specifier"); return 0; }
    if (int_count == 0 && long_count == 0 && short_count == 0 && unsigned_count == 0 && signed_count == 0 && char_count == 0 && double_count == 0 && void_count == 0) { fail(p, "expected a type"); return 0; }
    *type = void_count   ? TY_VOID
          : double_count ? TY_DOUBLE
          : char_count   ? (signed_count ? TY_SCHAR : unsigned_count ? TY_UCHAR : TY_CHAR)
          : short_count  ? (unsigned_count ? TY_USHORT : TY_SHORT)
                         : make_int_type(long_count > 0, unsigned_count > 0);
    return 1;
}


// the innermost-scope declaration of this name, or 0 if it isn't declared in the current block
// (redeclaration is only an error within the same block; an inner block may shadow an outer name)
static const Var* scope_lookup(const Parser* p, size_t start, size_t len)
{
    for (size_t i = p->var_count; i > p->scope_start; i--) {
        const Var* v = SEG_LIST_REF(Var, &p->vars, i - 1);
        if (v->name_len != len) continue;
        Bool match = 1;
        for (size_t k = 0; k < len; k++) {
            if (p->src[v->name_off + k] != p->src[start + k]) { match = 0; break; }
        }
        if (match) return v;
    }
    return 0;
}

static Bool var_in_current_scope(const Parser* p, size_t start, size_t len)
{
    return scope_lookup(p, start, len) != 0;
}

// the writable Var slot at var_count: the scope stack reuses storage, so overwrite an existing
// slot (popped by an earlier scope exit) or append a fresh one
static Var* vars_slot(Parser* p)
{
    if (p->var_count < p->vars.count) return SEG_LIST_REF(Var, &p->vars, p->var_count);
    Var zero = {0};
    return SEG_LIST_APPEND(Var, &p->vars, zero);
}

// declare a new variable of type `ty` (caller has checked it isn't already declared); returns its temp.
// `by_ref`: a by-value aggregate parameter, passed as the address of the caller's copy -- hold that
// address in a single pointer temp (keeping it one param temp) rather than an inline array temp.
static u32 var_add(Parser* p, size_t start, size_t len, Type ty, Bool by_ref)
{
    Var* v = vars_slot(p);
    v->name_off = start;
    v->name_len = len;
    u32 t = type_is_aggregate(ty) ? (by_ref ? ir_new_ptr_temp(p->fn) : ir_new_array_temp(p->fn, (u32)type_size(ty)))
                                  : temp_for(p->fn, ty);
    v->temp = t;
    v->type = ty;
    v->is_func = 0;
    v->is_static = 0;
    v->no_linkage = 1;
    v->by_ref = by_ref && type_is_aggregate(ty);
    v->asm_reg = 0xFF;
    p->var_count += 1;
    return t;
}

// bind a name in the current scope to a static object: a block-scope `extern` (linkage, no_linkage=0)
// or a local static (no linkage, no_linkage=1)
static void scope_add_static(Parser* p, size_t start, size_t len, u32 static_id, Bool no_linkage)
{
    Var* v = vars_slot(p);
    v->name_off = start;
    v->name_len = len;
    v->temp = 0;
    v->is_func = 0;
    v->is_static = 1;
    v->no_linkage = no_linkage;
    v->static_id = static_id;
    p->var_count += 1;
}

// bind an enumeration constant in the current scope (reuses storage like the var stack)
static void enum_const_add(Parser* p, size_t start, size_t len, i64 value)
{
    EnumConst* ec;
    if (p->enum_const_count < p->enum_consts.count) ec = SEG_LIST_REF(EnumConst, &p->enum_consts, p->enum_const_count);
    else { EnumConst zero = {0}; ec = SEG_LIST_APPEND(EnumConst, &p->enum_consts, zero); }
    ec->name_off = start;
    ec->name_len = len;
    ec->value = value;
    p->enum_const_count += 1;
}

// bind a typedef-name in the current scope to `ty` (reuses storage like the var stack)
static void typedef_add(Parser* p, size_t start, size_t len, Type ty)
{
    TypedefName* td;
    if (p->typedef_count < p->typedefs.count) td = SEG_LIST_REF(TypedefName, &p->typedefs, p->typedef_count);
    else { TypedefName zero = {0}; td = SEG_LIST_APPEND(TypedefName, &p->typedefs, zero); }
    td->name_off = start;
    td->name_len = len;
    td->type = ty;
    p->typedef_count += 1;
}

// record a block-scope function declaration in the current scope (no temp); caller has checked
// it doesn't clash with a variable of the same name
static void scope_add_func(Parser* p, size_t start, size_t len)
{
    Var* v = vars_slot(p);
    v->name_off = start;
    v->name_len = len;
    v->temp = 0;
    v->is_func = 1;
    v->is_static = 0;
    v->no_linkage = 0;
    p->var_count += 1;
}

// do two source spans hold the same text?
static Bool src_eq(const Parser* p, size_t a_off, size_t a_len, size_t b_off, size_t b_len)
{
    if (a_len != b_len) return 0;
    for (size_t k = 0; k < a_len; k++) {
        if (p->src[a_off + k] != p->src[b_off + k]) return 0;
    }
    return 1;
}
static Bool name_is(const Parser* p, size_t off, size_t len, const char* s);   // does the span spell exactly `s`?

// resolve a function name to its id (and parameter count)
static Bool func_find(const Parser* p, size_t off, size_t len, u32* out_id, u32* out_params)
{
    u32 id;
    if (!name_map_get(&p->global_index, p->src + off, len, &id)) return 0;
    const Global* g = SEG_LIST_REF(Global, &p->funcs, id);
    *out_id = id;
    *out_params = g->param_count;
    return 1;
}

// declare a function (a redeclaration must agree on the parameter count); returns its id. A function
// has internal linkage iff declared `static`; a non-static (extern or plain) declaration adopts the
// linkage of a prior declaration, defaulting to external (C 6.2.2p4-5).
// append a parameter type to the parser's pool
static void param_types_push(Parser* p, Type t)
{
    SEG_LIST_APPEND(Type, &p->param_types, t);
    p->param_type_count++;
}

static u32 func_declare(Parser* p, size_t off, size_t len, u32 param_count, StorageClass sc, Bool file_scope, Type ret, const SegList* params)
{
    u32 existing;
    if (name_map_get(&p->global_index, p->src + off, len, &existing)) {
        Global* e = SEG_LIST_REF(Global, &p->funcs, existing);
        if (e->kind != GLOBAL_FUNC) { fail(p, "redeclared as a different kind of symbol"); return existing; }
        Bool e_unspec = e->param_count == PARAM_COUNT_UNSPECIFIED;
        Bool n_unspec = param_count == PARAM_COUNT_UNSPECIFIED;
        if (!e_unspec && !n_unspec) {                                 // two prototypes: they must agree
            if (e->param_count != param_count) { fail(p, "conflicting declarations for function"); return existing; }
            for (u32 k = 0; k < param_count; k++)
                if (!same_type(SEG_LIST_VAL(Type, &p->param_types, e->param_type_start + k), SEG_LIST_VAL(NameRef, params, k).type)) { fail(p, "conflicting types for function"); return existing; }
        } else if (e_unspec && !n_unspec) {                          // a real prototype refines a prior `()`
            e->param_count = param_count;
            e->param_type_start = (u32)p->param_type_count;
            for (u32 k = 0; k < param_count; k++) param_types_push(p, SEG_LIST_VAL(NameRef, params, k).type);
        }                                                            // else (n_unspec): an `()` redecl adds no constraint
        if (!same_type(e->type, ret)) { fail(p, "conflicting return type for function"); return existing; }
        Linkage want = (sc == SC_STATIC) ? LINK_INTERNAL : e->linkage;
        if (want != e->linkage) { fail(p, "conflicting linkage for function"); return existing; }
        if (file_scope) e->file_scope_seen = 1;
        return existing;
    }
    Global zero = {0};
    Global* g = SEG_LIST_APPEND(Global, &p->funcs, zero);   // stable address across param_types_push
    g->name_off = off;
    g->name_len = len;
    g->kind = GLOBAL_FUNC;
    g->type = ret;
    g->param_count = param_count;
    g->param_type_start = (u32)p->param_type_count;
    if (param_count != PARAM_COUNT_UNSPECIFIED)
        for (u32 k = 0; k < param_count; k++) param_types_push(p, SEG_LIST_VAL(NameRef, params, k).type);
    g->init = 0;
    g->defined = 0;
    g->internal = 0;
    g->linkage = (sc == SC_STATIC) ? LINK_INTERNAL : LINK_EXTERNAL;
    g->file_scope_seen = file_scope;
    g->has_initializer = 0;
    u32 id = (u32)p->func_count;
    p->func_count += 1;
    name_map_put(&p->global_index, p->src + off, len, id);   // name-resolvable (internal==0)
    return id;
}

static void func_define(Parser* p, u32 id)
{
    Global* g = SEG_LIST_REF(Global, &p->funcs, id);
    if (g->defined) { fail(p, "function defined more than once"); return; }
    g->defined = 1;
}

// find a file-scope symbol by source text; on success returns its id and kind
static Bool global_find(const Parser* p, size_t off, size_t len, u32* out_id, GlobalKind* out_kind)
{
    u32 id;
    if (!name_map_get(&p->global_index, p->src + off, len, &id)) return 0;
    const Global* g = SEG_LIST_REF(Global, &p->funcs, id);
    if (!g->file_scope_seen) return 0;  // a block-scope `extern` alone is not visible at file scope
    *out_id = id; *out_kind = g->kind;
    return 1;
}

// declare a block-scope `static` variable: a fresh GLOBAL_STATIC with no linkage, always appended
// (never merged), so two local statics of the same name -- or a same-named file-scope var -- stay
// distinct. Returns its id.
static u32 local_static_declare(Parser* p, size_t off, size_t len, const u8* init, u32 init_len,
                                const StaticReloc* relocs, u32 reloc_count, Type ty)
{
    Global zero = {0};
    Global* g = SEG_LIST_APPEND(Global, &p->funcs, zero);
    g->name_off = off;
    g->name_len = len;
    g->kind = GLOBAL_STATIC;
    g->type = ty;
    g->param_count = 0;
    g->init = init;
    g->init_len = init_len;
    g->relocs = relocs;
    g->reloc_count = reloc_count;
    g->defined = 1;
    g->internal = 1;
    g->linkage = LINK_INTERNAL;
    g->file_scope_seen = 0;
    g->has_initializer = 0;
    u32 id = (u32)p->func_count;
    p->func_count += 1;
    return id;
}

// declare a file-scope variable. `extern` without an initializer is just a declaration; any
// other form is a definition (a plain `int x;` is a tentative definition, initialized to 0).
// Re-declarations merge; the initialized definition (if any) wins. Linkage: `static` is internal,
// `extern` adopts a prior declaration's linkage (else external), and a plain `int x;` is external
// (C 6.2.2). A declaration whose linkage disagrees with a prior one is an error.
static u32 global_var_declare(Parser* p, size_t off, size_t len, StorageClass sc, const u8* init, u32 init_len,
                              const StaticReloc* relocs, u32 reloc_count, Bool file_scope, Type ty)
{
    Bool has_init = init != 0;
    Bool is_def = (sc != SC_EXTERN) || has_init;
    u32 existing;
    if (name_map_get(&p->global_index, p->src + off, len, &existing)) {
        Global* e = SEG_LIST_REF(Global, &p->funcs, existing);
        if (e->kind != GLOBAL_STATIC) { fail(p, "redeclared as a different kind of symbol"); return existing; }
        if (!same_type(e->type, ty)) { fail(p, "conflicting types for variable"); return existing; }
        Linkage want = (sc == SC_STATIC) ? LINK_INTERNAL : (sc == SC_EXTERN) ? e->linkage : LINK_EXTERNAL;
        if (want != e->linkage) { fail(p, "conflicting linkage for variable"); return existing; }
        if (has_init && e->has_initializer) { fail(p, "redefinition of variable"); return existing; }
        if (is_def) {
            // a sized definition completes a prior unsized `extern T a[]` -- adopt the sized type so a
            // later sizeof(a) sees the real length
            if (e->type->kind == TYK_ARRAY && e->type->count == 0 && ty->kind == TYK_ARRAY && ty->count != 0)
                e->type = ty;
            e->init_len = init_len;   // a definition (even a tentative one) has a slot of the variable's
                                      // size -- a prior bare `extern` left it 0, which must be corrected
            if (has_init) { e->init = init; e->relocs = relocs; e->reloc_count = reloc_count; e->has_initializer = 1; }
            e->defined = 1;
        }
        if (file_scope) e->file_scope_seen = 1;
        return existing;
    }
    Global zero = {0};
    Global* g = SEG_LIST_APPEND(Global, &p->funcs, zero);
    g->name_off = off;
    g->name_len = len;
    g->kind = GLOBAL_STATIC;
    g->type = ty;
    g->param_count = 0;
    g->init = has_init ? init : 0;
    g->init_len = init_len;
    g->relocs = relocs;
    g->reloc_count = reloc_count;
    g->defined = is_def;
    g->internal = 0;
    g->linkage = (sc == SC_STATIC) ? LINK_INTERNAL : LINK_EXTERNAL;
    g->file_scope_seen = file_scope;
    g->has_initializer = has_init;
    u32 id = (u32)p->func_count;
    p->func_count += 1;
    name_map_put(&p->global_index, p->src + off, len, id);   // name-resolvable (internal==0)
    return id;
}

// find a label by source text; on success returns 1 and writes its table index
static Bool label_find(const Parser* p, size_t start, size_t len, size_t* out_index)
{
    for (size_t i = 0; i < p->label_count; i++) {
        const Label* l = SEG_LIST_REF(Label, &p->labels, i);
        if (l->name_len != len) continue;
        Bool match = 1;
        for (size_t k = 0; k < len; k++) {
            if (p->src[l->name_off + k] != p->src[start + k]) { match = 0; break; }
        }
        if (match) { *out_index = i; return 1; }
    }
    return 0;
}

static u32 label_new(Parser* p, size_t start, size_t len, Bool defined)
{
    // labels reset per function (label_count -> 0); reuse the storage from a prior function
    Label* l;
    if (p->label_count < p->labels.count) l = SEG_LIST_REF(Label, &p->labels, p->label_count);
    else { Label zero = {0}; l = SEG_LIST_APPEND(Label, &p->labels, zero); }
    l->name_off = start;
    l->name_len = len;
    u32 t = ir_new_label(p->fn);
    l->ir_label = t;
    l->defined = defined;
    p->label_count += 1;
    return t;
}

// the IR label a `goto` targets (created on first reference; may be defined later)
static u32 label_ref(Parser* p, size_t start, size_t len)
{
    size_t i;
    if (label_find(p, start, len, &i)) return SEG_LIST_REF(Label, &p->labels, i)->ir_label;
    return label_new(p, start, len, 0);
}

// the IR label for a labeled statement; a second definition is an error
static u32 label_def(Parser* p, size_t start, size_t len)
{
    size_t i;
    if (label_find(p, start, len, &i)) {
        Label* l = SEG_LIST_REF(Label, &p->labels, i);
        if (l->defined) fail(p, "duplicate label");
        l->defined = 1;
        return l->ir_label;
    }
    return label_new(p, start, len, 1);
}

// push a label onto a u32 stack; the SegList reuses storage, so overwrite a popped slot or append
static void stack_push(SegList* s, size_t* depth, u32 label)
{
    if (*depth < s->count) *SEG_LIST_REF(u32, s, *depth) = label;
    else SEG_LIST_APPEND(u32, s, label);
    *depth += 1;
}

// a loop is both a break and a continue target; a switch is only a break target
static void break_push(Parser* p, u32 label)    { stack_push(&p->break_targets, &p->break_depth, label); }
static void break_pop(Parser* p)                { p->break_depth -= 1; }
static void continue_push(Parser* p, u32 label) { stack_push(&p->continue_targets, &p->continue_depth, label); }
static void continue_pop(Parser* p)             { p->continue_depth -= 1; }

static Bool break_current(const Parser* p, u32* out)
{
    if (p->break_depth == 0) return 0;
    *out = SEG_LIST_VAL(u32, &p->break_targets, p->break_depth - 1);
    return 1;
}
static Bool continue_current(const Parser* p, u32* out)
{
    if (p->continue_depth == 0) return 0;
    *out = SEG_LIST_VAL(u32, &p->continue_targets, p->continue_depth - 1);
    return 1;
}

static ExpResult parse_exp(Parser* p);
static ExpResult parse_full_exp(Parser* p);   // full expression (assignment-exprs joined by the comma operator)
static u8 parse_reg_binding(Parser* p);   // defined near parse_asm; parses a `__asm__("reg")` declarator binding
static ExpResult parse_call(Parser* p);
static Bool parse_type_only(Parser* p, Type* type);
static void parse_statement(Parser* p);
static void parse_block_item(Parser* p);
static void parse_block(Parser* p);

// binary operator precedence (higher binds tighter); -1 means "not a binary operator"
static int binary_prec(u8 tag)
{
    switch (tag) {
        case TOKEN_ASTERISK: case TOKEN_SLASH: case TOKEN_PERCENT:                 return 50;
        case TOKEN_PLUS:     case TOKEN_MINUS:                                     return 45;
        case TOKEN_LSHIFT:   case TOKEN_RSHIFT:                                    return 40;
        case TOKEN_LESS:     case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:  case TOKEN_GREATER_EQUAL:                             return 35;
        case TOKEN_EQUAL_EQUAL: case TOKEN_NOT_EQUAL:                              return 30;
        case TOKEN_AMP:                                                           return 25;
        case TOKEN_CARET:                                                         return 20;
        case TOKEN_PIPE:                                                          return 15;
        case TOKEN_AND:                                                           return 10;
        case TOKEN_OR:                                                            return 5;
        case TOKEN_QUESTION:                                                      return 3;
        case TOKEN_ASSIGN:
        case TOKEN_PLUS_ASSIGN:   case TOKEN_MINUS_ASSIGN:   case TOKEN_STAR_ASSIGN:
        case TOKEN_SLASH_ASSIGN:  case TOKEN_PERCENT_ASSIGN:
        case TOKEN_AMP_ASSIGN:    case TOKEN_PIPE_ASSIGN:    case TOKEN_CARET_ASSIGN:
        case TOKEN_LSHIFT_ASSIGN: case TOKEN_RSHIFT_ASSIGN:                        return 1;
        default:                                                                  return -1;
    }
}

static IrBinaryOp binary_op(u8 tag)
{
    switch (tag) {
        case TOKEN_PLUS:          return IR_ADD;
        case TOKEN_MINUS:         return IR_SUBTRACT;
        case TOKEN_ASTERISK:      return IR_MULTIPLY;
        case TOKEN_SLASH:         return IR_DIVIDE;
        case TOKEN_PERCENT:       return IR_REMAINDER;
        case TOKEN_AMP:           return IR_BIT_AND;
        case TOKEN_PIPE:          return IR_BIT_OR;
        case TOKEN_CARET:         return IR_BIT_XOR;
        case TOKEN_LSHIFT:        return IR_LSHIFT;
        case TOKEN_RSHIFT:        return IR_RSHIFT;
        case TOKEN_LESS:          return IR_LESS;
        case TOKEN_LESS_EQUAL:    return IR_LESS_EQUAL;
        case TOKEN_GREATER:       return IR_GREATER;
        case TOKEN_GREATER_EQUAL: return IR_GREATER_EQUAL;
        case TOKEN_EQUAL_EQUAL:   return IR_EQUAL;
        case TOKEN_NOT_EQUAL:     return IR_NOT_EQUAL;
        default:                  return IR_ADD;   // unreachable (guarded by binary_prec)
    }
}

// is `tag` a plain or compound assignment operator?
static Bool is_assign_tok(u8 tag) { return (token_traits[tag] & TOK_ASSIGN) != 0; }

// the arithmetic op a compound assignment applies (e.g. "+=" -> ADD)
static IrBinaryOp compound_op(u8 tag)
{
    switch (tag) {
        case TOKEN_PLUS_ASSIGN:    return IR_ADD;
        case TOKEN_MINUS_ASSIGN:   return IR_SUBTRACT;
        case TOKEN_STAR_ASSIGN:    return IR_MULTIPLY;
        case TOKEN_SLASH_ASSIGN:   return IR_DIVIDE;
        case TOKEN_PERCENT_ASSIGN: return IR_REMAINDER;
        case TOKEN_AMP_ASSIGN:     return IR_BIT_AND;
        case TOKEN_PIPE_ASSIGN:    return IR_BIT_OR;
        case TOKEN_CARET_ASSIGN:   return IR_BIT_XOR;
        case TOKEN_LSHIFT_ASSIGN:  return IR_LSHIFT;
        case TOKEN_RSHIFT_ASSIGN:  return IR_RSHIFT;
        default:                   return IR_ADD;   // unreachable
    }
}

static void emit_copy(Parser* p, IrVal src, u32 dst)
{
    IrInstr in; in.kind = IR_COPY; in.a = src; in.dst = dst; ir_emit(p->fn, in);
}
static void emit_store_static(Parser* p, u32 static_id, IrVal src)
{
    IrInstr in; in.kind = IR_STORE_STATIC; in.u.fun.callee = static_id; in.a = src; ir_emit(p->fn, in);
}
static void emit_store(Parser* p, ExpResult lv, IrVal src)
{
    if (lv.is_deref)       { IrInstr in; in.kind = IR_STORE; in.a = lv.deref_ptr; in.u.b = src; ir_emit(p->fn, in); }
    else if (lv.is_static) emit_store_static(p, lv.static_id, src);
    else                   emit_copy(p, src, lv.lvalue_temp);
}
static void emit_binary(Parser* p, IrBinaryOp op, IrVal a, IrVal b, u32 dst, Bool is_unsigned)
{
    IrInstr in; in.kind = IR_BINARY; in.binop = op; in.is_unsigned = is_unsigned;
    in.a = a; in.u.b = b; in.dst = dst; ir_emit(p->fn, in);
}
// addr + byte offset, as an 8-byte pointer (addr unchanged when offset is 0)
static IrVal addr_plus(Parser* p, IrVal addr, u64 offset)
{
    if (offset == 0) return addr;
    u32 a = ir_new_ptr_temp(p->fn);
    emit_binary(p, IR_ADD, addr, ir_constant_sized(offset, 8), a, 1);
    return ir_var(a);
}
// memcpy(dst, src, size) for a whole-aggregate (struct) copy
static void emit_block_copy(Parser* p, IrVal dst, IrVal src, u64 size)
{
    // a=dst-addr, b=src-addr, dst=byte count (callee aliases b in the union, so the size rides in dst)
    IrInstr in; in.kind = IR_COPY_BLOCK; in.a = dst; in.u.b = src; in.dst = (u32)size; ir_emit(p->fn, in);
}
// convert expression `e` to type `to`. Same size -> just reinterpret the bits (signed<->unsigned).
// Widening -> zero-extend if the source is unsigned, else sign-extend. Narrowing -> truncate.
static ExpResult convert(Parser* p, ExpResult e, Type to)
{
    if (e.type == to) return e;
    // a cast to void discards the value (its side effects already happened); the result is void
    if (type_is_void(to)) return res_value_typed(e.val, TY_VOID);
    if (type_is_void(e.type)) { fail(p, "a void value cannot be used where a value is required"); return e; }
    // a void* converts to/from any object pointer with no representation change
    if (type_is_pointer(to) && type_is_pointer(e.type) && (type_is_void_ptr(to) || type_is_void_ptr(e.type)))
        return res_value_typed(e.val, to);
    // a double and a pointer are never interconvertible (not even by an explicit cast)
    if ((type_is_double(e.type) && type_is_pointer(to)) || (type_is_pointer(e.type) && type_is_double(to))) {
        fail(p, "cannot convert between a double and a pointer");
        return e;
    }
    // char/short <-> double must route through int: the SSE convert ops (cvtsi2sd/cvttsd2si) need a
    // >= 32-bit integer operand, which a 1-/2-byte narrow temp can't supply or receive directly.
    if (type_is_narrow(e.type) && type_is_double(to)) return convert(p, convert(p, e, TY_INT), to);
    if (type_is_double(e.type) && type_is_narrow(to)) return convert(p, convert(p, e, TY_INT), to);
    // double <-> integer: a dedicated conversion op (exactly one side is double, since double==double
    // is caught above by the singleton compare)
    if (type_is_double(e.type) || type_is_double(to)) {
        u32 dst = temp_for(p->fn, to);
        IrInstr in;
        if (type_is_double(to)) in.kind = type_unsigned(e.type) ? IR_UINT_TO_DOUBLE : IR_INT_TO_DOUBLE;
        else                    in.kind = type_unsigned(to)     ? IR_DOUBLE_TO_UINT : IR_DOUBLE_TO_INT;
        in.a = e.val; in.dst = dst;
        ir_emit(p->fn, in);
        return res_value_typed(ir_var(dst), to);
    }
    if (type_size(e.type) == type_size(to)) return res_value_typed(e.val, to);
    u32 dst = temp_for(p->fn, to);
    IrInstr in;
    if (type_size(to) > type_size(e.type))
        in.kind = type_unsigned(e.type) ? IR_ZERO_EXTEND : IR_SIGN_EXTEND;
    else
        in.kind = IR_TRUNCATE;
    in.a = e.val; in.dst = dst;
    ir_emit(p->fn, in);
    return res_value_typed(ir_var(dst), to);
}
// integer promotion (C 6.3.1.1): char/short (signed or unsigned) promote to int in arithmetic.
static ExpResult promote(Parser* p, ExpResult e) { return type_is_narrow(e.type) ? convert(p, e, TY_INT) : e; }

// implicit conversion (assignment/init/arg/return): like a cast, but rejects incompatible pointers
static ExpResult convert_to(Parser* p, ExpResult e, Type to)
{
    e = decay(e);
    if (type_is_void(e.type) && !type_is_void(to)) { fail(p, "void value not ignored as it ought to be"); return e; }
    if (type_is_struct(to) || type_is_struct(e.type)) {   // structs convert only to the identical struct type
        if (!same_type(e.type, to)) { fail(p, "incompatible struct/union type"); return e; }
        return e;
    }
    Bool to_ptr = type_is_pointer(to) || type_is_funptr(to);
    Bool e_ptr  = type_is_pointer(e.type) || type_is_funptr(e.type);
    if (to_ptr) {
        // void* is implicitly compatible with any object OR function pointer, either direction (the latter
        // a common extension DOOM relies on -- it stores callbacks in void*); a null constant converts to
        // any pointer; otherwise the types must match structurally.
        Bool to_void = type_is_pointer(to) && type_is_void_ptr(to);
        Bool e_void  = type_is_pointer(e.type) && type_is_void_ptr(e.type);
        Bool void_compat = (to_void && e_ptr) || (e_void && to_ptr);
        if (!same_type(e.type, to) && !is_null_constant(e) && !void_compat) { fail(p, "incompatible type in conversion to pointer"); return e; }
    } else if (e_ptr) {
        fail(p, "cannot implicitly convert a pointer to a non-pointer");
        return e;
    }
    return convert(p, e, to);
}
static void emit_jump(Parser* p, u32 label)
{
    IrInstr in; in.kind = IR_JUMP; in.u.label = label; ir_emit(p->fn, in);
}
static void emit_jump_if_zero(Parser* p, IrVal a, u32 label)
{
    IrInstr in; in.kind = IR_JUMP_IF_ZERO; in.a = a; in.u.label = label; ir_emit(p->fn, in);
}
static void emit_jump_if_not_zero(Parser* p, IrVal a, u32 label)
{
    IrInstr in; in.kind = IR_JUMP_IF_NOT_ZERO; in.a = a; in.u.label = label; ir_emit(p->fn, in);
}
static void emit_label(Parser* p, u32 label)
{
    IrInstr in; in.kind = IR_LABEL; in.u.label = label; ir_emit(p->fn, in);
}

// <call> ::= <identifier> "(" [ <exp> { "," <exp> } ] ")"   (current token is the name)
static ExpResult parse_call(Parser* p)
{
    size_t name_off = p->cur.start;
    size_t name_len = p->cur.limit - p->cur.start;
    u32 func_id, param_count;
    if (!func_find(p, name_off, name_len, &func_id, &param_count)) {
        fail(p, "call to undeclared function");
        return res_value(ir_constant(0));
    }
    advance(p);   // function name
    advance(p);   // '('

    // evaluate the arguments into a scratch buffer first; a nested call would otherwise
    // interleave its arguments into the pool, so we append ours contiguously afterward
    StableList argbuf = STABLE_LIST_INIT(IrVal);
    size_t argn = 0;
    if (p->cur.tag != TOKEN_RPAREN) {
        u32 pts = SEG_LIST_REF(Global, &p->funcs, func_id)->param_type_start;
        for (;;) {
            ExpResult a = parse_exp(p);
            if (p->failed) return res_value(ir_constant(0));
            if (param_count != PARAM_COUNT_UNSPECIFIED && argn < param_count)   // convert the argument to its parameter's type
                a = convert_to(p, a, SEG_LIST_VAL(Type, &p->param_types, pts + argn));
            IrVal av = a.val;
            if (type_is_aggregate(a.type)) {   // by-value struct: pass the address of a fresh copy
                u32 copy = ir_new_array_temp(p->fn, (u32)type_size(a.type));
                u32 caddr = ir_new_ptr_temp(p->fn);
                IrInstr ad; ad.kind = IR_ADDROF; ad.a = ir_var(copy); ad.dst = caddr; ir_emit(p->fn, ad);
                emit_block_copy(p, ir_var(caddr), a.val, type_size(a.type));
                av = ir_var(caddr);
            }
            STABLE_LIST_APPEND(IrVal, &argbuf, av);
            argn += 1;
            if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
            break;
        }
    }
    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return res_value(ir_constant(0)); }
    advance(p);
    if (param_count != PARAM_COUNT_UNSPECIFIED && argn != param_count) { fail(p, "wrong number of arguments in call"); return res_value(ir_constant(0)); }

    Type ret = SEG_LIST_REF(Global, &p->funcs, func_id)->type;
    // a function returning a large struct uses sret: the caller allocates the result slot and passes
    // its address as a hidden first argument; the callee fills it (see parse_function / the return stmt).
    Bool sret = type_is_aggregate(ret);
    u32 sret_addr = 0;
    u32 arg_start = (u32)p->fn->call_arg_count;
    if (sret) {
        u32 slot = ir_new_array_temp(p->fn, (u32)type_size(ret));
        sret_addr = ir_new_ptr_temp(p->fn);
        IrInstr ad; ad.kind = IR_ADDROF; ad.a = ir_var(slot); ad.dst = sret_addr; ir_emit(p->fn, ad);
        ir_add_arg(p->fn, ir_var(sret_addr));   // hidden sret pointer = argument 0
    }
    STABLE_LIST_FOREACH(it, &argbuf) {
        ir_add_arg(p->fn, *(IrVal*)it.elem);
    }
    stablelist_deinit(&argbuf);
    u32 dst = temp_for(p->fn, sret ? TY_LONG : ret);   // holds the return register (the sret ptr, unused for sret)
    IrInstr in;
    in.kind = IR_FUNCALL;
    in.u.fun.callee = func_id;
    in.u.fun.arg_start = arg_start;
    in.u.fun.arg_count = (u32)argn + (sret ? 1u : 0u);
    in.dst = dst;
    ir_emit(p->fn, in);
    if (sret) return res_value_typed(ir_var(sret_addr), ret);   // result is the filled slot (its address)
    return res_value_typed(ir_var(dst), ret);
}

// --- a small big-integer, just enough for correctly-rounded decimal -> double (all pure integer, so
// it self-hosts cleanly). Little-endian base-2^32; n = used limbs. 64 limbs (2048 bits) covers any
// double-range literal (a ~50-digit mantissa scaled by up to 10^308 / 2^1074). ---
#define BN_LIMBS 64
typedef struct { u32 v[BN_LIMBS]; int n; } Bn;

static void bn_set_u64(Bn* a, u64 x) { a->v[0] = (u32)x; a->v[1] = (u32)(x >> 32); a->n = a->v[1] ? 2 : (a->v[0] ? 1 : 0); }
static void bn_copy(Bn* d, const Bn* s) { d->n = s->n; for (int i = 0; i < s->n; i++) d->v[i] = s->v[i]; }

static void bn_mul_add_small(Bn* a, u32 mul, u32 add)   // a = a*mul + add
{
    u64 carry = add;
    for (int i = 0; i < a->n; i++) { u64 t = (u64)a->v[i] * mul + carry; a->v[i] = (u32)t; carry = t >> 32; }
    while (carry && a->n < BN_LIMBS) { a->v[a->n++] = (u32)carry; carry >>= 32; }
}

static void bn_shl(Bn* a, int bits)   // a <<= bits
{
    if (a->n == 0 || bits == 0) return;
    int b = bits & 31, words = bits >> 5;
    if (b) {
        u32 carry = 0;
        for (int i = 0; i < a->n; i++) { u64 t = ((u64)a->v[i] << b) | carry; a->v[i] = (u32)t; carry = (u32)(t >> 32); }
        if (carry && a->n < BN_LIMBS) a->v[a->n++] = carry;
    }
    if (words) {
        for (int i = a->n - 1; i >= 0; i--) if (i + words < BN_LIMBS) a->v[i + words] = a->v[i];
        for (int i = 0; i < words; i++) a->v[i] = 0;
        a->n += words;
        if (a->n > BN_LIMBS) a->n = BN_LIMBS;
    }
}

static int bn_bitlen(const Bn* a)
{
    if (a->n == 0) return 0;
    u32 hi = a->v[a->n - 1]; int b = 0;
    while (hi) { hi >>= 1; b++; }
    return (a->n - 1) * 32 + b;
}

static int bn_cmp(const Bn* a, const Bn* b)   // -1 / 0 / 1
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}

static void bn_sub(Bn* a, const Bn* b)   // a -= b (requires a >= b)
{
    i64 borrow = 0;
    for (int i = 0; i < a->n; i++) {
        i64 t = (i64)a->v[i] - (i < b->n ? (i64)b->v[i] : 0) - borrow;
        if (t < 0) { t += (i64)1 << 32; borrow = 1; } else borrow = 0;
        a->v[i] = (u32)t;
    }
    while (a->n > 0 && a->v[a->n - 1] == 0) a->n--;
}

// q = floor(A / B), A becomes the remainder. Assumes the quotient fits in u64 (it does: we pre-scale
// so q is ~53 bits). Binary long division, high bit to low.
static u64 bn_divmod(Bn* A, const Bn* B)
{
    int sh = bn_bitlen(A) - bn_bitlen(B);
    u64 q = 0;
    Bn t;
    for (int bit = sh; bit >= 0; bit--) {
        bn_copy(&t, B); bn_shl(&t, bit);
        if (bn_cmp(A, &t) >= 0) { bn_sub(A, &t); q |= (u64)1 << bit; }
    }
    return q;
}

// Parse a decimal floating constant [start, limit) to its IEEE-754 double bit pattern, correctly
// rounded (round-to-nearest, ties to even). The literal is non-negative. value = digits * 10^dexp,
// computed exactly as num/den, then divided to a normalized 53-bit significand with one rounding.
static u64 parse_double_bits(const u8* s, size_t start, size_t limit)
{
    Bn digits; digits.n = 0;
    int dexp = 0, ndigits = 0;
    Bool dot = 0;
    size_t k = start;
    for (; k < limit; k++) {
        u8 c = s[k];
        if (c >= '0' && c <= '9') {
            if (ndigits < 600) { bn_mul_add_small(&digits, 10, (u32)(c - '0')); if (dot) dexp--; ndigits++; }
            else if (!dot) dexp++;           // far more digits than a double can distinguish: track scale only
        } else if (c == '.') { dot = 1; }
        else break;                          // an 'e'/'E' exponent
    }
    if (k < limit && (s[k] == 'e' || s[k] == 'E')) {
        k++;
        int esign = 1;
        if (k < limit && s[k] == '+') k++;
        else if (k < limit && s[k] == '-') { esign = -1; k++; }
        int e = 0;
        for (; k < limit && s[k] >= '0' && s[k] <= '9'; k++) e = e * 10 + (s[k] - '0');
        dexp += esign * e;
    }
    if (digits.n == 0) return 0;             // value is 0

    Bn num, den;
    bn_copy(&num, &digits);
    bn_set_u64(&den, 1);
    if (dexp > 0)      for (int i = 0; i < dexp; i++)  bn_mul_add_small(&num, 10, 0);
    else if (dexp < 0) for (int i = 0; i < -dexp; i++) bn_mul_add_small(&den, 10, 0);

    // choose binary exponent p so the quotient lands in [2^52, 2^53); divide, adjusting if off by one
    int p = bn_bitlen(&num) - bn_bitlen(&den) - 53;
    u64 q;
    Bn A, B;
    for (;;) {
        bn_copy(&A, &num); bn_copy(&B, &den);
        if (p >= 0) bn_shl(&B, p); else bn_shl(&A, -p);
        q = bn_divmod(&A, &B);               // A becomes the remainder, B the scaled divisor
        if (q >= ((u64)1 << 53)) { p++; continue; }
        if (q < ((u64)1 << 52)) { p--; continue; }
        break;
    }

    // round to nearest, ties to even: compare 2*remainder against the divisor
    Bn rem2; bn_copy(&rem2, &A); bn_shl(&rem2, 1);
    int cmp = bn_cmp(&rem2, &B);
    if (cmp > 0 || (cmp == 0 && (q & 1))) { q++; if (q == ((u64)1 << 53)) { q = (u64)1 << 52; p++; } }

    // assemble: value = q * 2^p, q in [2^52, 2^53). Unbiased exponent of the leading 1 is p + 52.
    int biased = p + 52 + 1023;
    if (biased >= 2047) return 0x7FF0000000000000ull;               // overflow -> +inf
    if (biased >= 1)    return ((u64)biased << 52) | (q & 0xFFFFFFFFFFFFFull);
    int rsh = 1 - biased;                                          // subnormal: drop to exponent 2^-1074
    if (rsh >= 64) return 0;                                       // underflow -> 0
    u64 mant = q >> rsh, lost = q & (((u64)1 << rsh) - 1), half = (u64)1 << (rsh - 1);
    if (lost > half || (lost == half && (mant & 1))) mant++;
    return mant;
}

// decode one escape sequence; `*pk` points at the char after the backslash and is advanced past it.
static u8 decode_escape(const u8* src, size_t* pk)
{
    size_t k = *pk;
    int ch;
    switch (src[k]) {
        case 'n': ch = '\n'; break; case 't': ch = '\t'; break; case 'r': ch = '\r'; break;
        case '0': ch = 0;    break; case 'a': ch = 7;    break; case 'b': ch = 8;    break;
        case 'f': ch = 12;   break; case 'v': ch = 11;   break;
        default:  ch = src[k]; break;                         // \\ \' \" \?  -> the char itself
    }
    *pk = k + 1;
    return (u8)ch;
}

// decode a character-constant token's value (`start` points at the opening quote). A char constant is
// int-valued; plain char is signed, so the byte is sign-extended.
static int char_constant_value(const u8* src, size_t start)
{
    size_t k = start + 1;
    u8 ch;
    if (src[k] == '\\') { k++; ch = decode_escape(src, &k); }
    else                  ch = src[k];
    return (int)(signed char)ch;
}

// Decode the current run of adjacent string-literal tokens (C concatenates them) into newly arena-
// allocated, null-terminated bytes; advances past the run and sets *out_len to the length sans null.
static const u8* decode_string_run(Parser* p, u32* out_len)
{
    // pass 1: measure the run's raw byte count (a safe upper bound on the decoded length -- it counts
    // the quotes/backslashes too), then rewind. No fixed cap on the number of adjacent literals.
    Token save_cur = p->cur; size_t save_off = p->off; Token save_next = p->next; Bool save_has = p->has_next;
    size_t raw = 0;
    while (p->cur.tag == TOKEN_STRING) { raw += p->cur.limit - p->cur.start; advance(p); }
    p->cur = save_cur; p->off = save_off; p->next = save_next; p->has_next = save_has;

    // pass 2: decode into the buffer, advancing past the run
    u8* buf = ARENA_ALLOC(&global_arena, raw + 1);
    u32 len = 0;
    while (p->cur.tag == TOKEN_STRING) {
        size_t k = p->cur.start + 1, end = p->cur.limit - 1;   // strip the surrounding quotes
        while (k < end) {
            if (p->src[k] == '\\') { k++; buf[len++] = decode_escape(p->src, &k); }
            else                     buf[len++] = p->src[k++];
        }
        advance(p);
    }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

// Decode a run of adjacent L"..." wide-string tokens into arena-allocated UTF-16 (2 bytes per wchar_t,
// null-terminated); advances past the run and sets *out_count to the count sans null. The token span is
// L"..." (start+2 strips the leading L"; limit-1 strips the closing ").
static const u8* decode_wide_string_run(Parser* p, u32* out_count)
{
    Token save_cur = p->cur; size_t save_off = p->off; Token save_next = p->next; Bool save_has = p->has_next;
    size_t raw = 0;
    while (p->cur.tag == TOKEN_WIDE_STRING) { raw += p->cur.limit - p->cur.start; advance(p); }
    p->cur = save_cur; p->off = save_off; p->next = save_next; p->has_next = save_has;

    u8* buf = ARENA_ALLOC(&global_arena, (raw + 1) * 2);   // 2 bytes per wchar_t, plus the null terminator
    u32 count = 0;
    while (p->cur.tag == TOKEN_WIDE_STRING) {
        size_t k = p->cur.start + 2, end = p->cur.limit - 1;
        while (k < end) {
            u16 wc;
            if (p->src[k] == '\\') { k++; wc = decode_escape(p->src, &k); }
            else                     wc = p->src[k++];
            buf[count * 2] = (u8)wc; buf[count * 2 + 1] = (u8)(wc >> 8);   // little-endian UTF-16
            count++;
        }
        advance(p);
    }
    buf[count * 2] = 0; buf[count * 2 + 1] = 0;
    *out_count = count;
    return buf;
}

// the numeric value of an integer-constant token (decimal, 0x-hex, or 0-octal), noting its u/l suffixes
static u64 int_token_value(const u8* src, size_t start, size_t limit, Bool* has_u, Bool* has_l)
{
    *has_u = 0; *has_l = 0;
    u64 value = 0;
    size_t k = start;
    if (limit - start >= 2 && src[start] == '0' && (src[start + 1] == 'b' || src[start + 1] == 'B')) {
        for (k = start + 2; k < limit && (src[k] == '0' || src[k] == '1'); k++) value = value * 2 + (u64)(src[k] - '0');
    } else if (limit - start >= 2 && src[start] == '0' && (src[start + 1] == 'x' || src[start + 1] == 'X')) {
        for (k = start + 2; k < limit; k++) {
            u8 c = src[k];
            if (c >= '0' && c <= '9')      value = value * 16 + (u64)(c - '0');
            else if (c >= 'a' && c <= 'f') value = value * 16 + (u64)(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') value = value * 16 + (u64)(10 + c - 'A');
            else break;
        }
    } else if (limit - start >= 2 && src[start] == '0' && src[start + 1] >= '0' && src[start + 1] <= '7') {
        // octal constant: a leading 0 followed by octal digits (e.g. 0755 file modes)
        for (k = start + 1; k < limit && src[k] >= '0' && src[k] <= '7'; k++) value = value * 8 + (u64)(src[k] - '0');
    } else {
        for (; k < limit && src[k] >= '0' && src[k] <= '9'; k++) value = value * 10 + (u64)(src[k] - '0');
    }
    for (; k < limit; k++) {
        u8 c = src[k];
        if (c == 'l' || c == 'L') *has_l = 1;
        else if (c == 'u' || c == 'U') *has_u = 1;
    }
    return value;
}

// <primary> ::= <int> | <double> | <identifier> | "(" <exp> ")"
static ExpResult parse_primary(Parser* p)
{
    switch (p->cur.tag) {
    case TOKEN_CONSTANT_INT: {
        if (p->src[p->cur.start] == '\'') {                  // character constant: int-valued
            u64 v = (u64)(i64)char_constant_value(p->src, p->cur.start);
            advance(p);
            return res_value_typed(ir_constant_sized(v, 4), TY_INT);
        }
        Bool has_l, has_u;
        u64 value = int_token_value(p->src, p->cur.start, p->cur.limit, &has_u, &has_l);
        // pick the first type from the suffix's candidate list that holds the value (C 6.4.4.1)
        Type ty;
        if (has_u && has_l)      ty = TY_ULONG;
        else if (has_u)          ty = (value > 0xFFFFFFFFu) ? TY_ULONG : TY_UINT;
        else if (has_l)          ty = TY_LONG;
        else                     ty = (value > 0x7FFFFFFF) ? TY_LONG : TY_INT;
        advance(p);
        return res_value_typed(ir_constant_sized(value, type_size(ty)), ty);
    }
    case TOKEN_CONSTANT_FLOAT: {
        u64 bits = parse_double_bits(p->src, p->cur.start, p->cur.limit);
        advance(p);
        return res_value_typed(ir_double_const(bits), TY_DOUBLE);
    }

    case TOKEN_STRING: {
        // a string literal: its bytes live in an anonymous read-only pool static; the expression is
        // that static's address, typed char[len+1] (incl. null), so it decays to char* where needed.
        u32 len;
        const u8* bytes = decode_string_run(p, &len);
        Type ty = array_of(TY_CHAR, (u64)len + 1);
        u32 id = local_static_declare(p, 0, 0, bytes, len + 1, 0, 0, ty);
        u32 a = ir_new_ptr_temp(p->fn);
        IrInstr in; in.kind = IR_ADDROF_STATIC; in.dst = a; in.u.fun.callee = id; ir_emit(p->fn, in);
        return res_value_typed(ir_var(a), ty);
    }

    case TOKEN_WIDE_STRING: {
        u32 count;
        const u8* bytes = decode_wide_string_run(p, &count);
        Type ty = array_of(TY_USHORT, (u64)count + 1);
        u32 id = local_static_declare(p, 0, 0, bytes, (count + 1) * 2, 0, 0, ty);
        u32 a = ir_new_ptr_temp(p->fn);
        IrInstr in; in.kind = IR_ADDROF_STATIC; in.dst = a; in.u.fun.callee = id; ir_emit(p->fn, in);
        return res_value_typed(ir_var(a), ty);
    }

    case TOKEN_IDENTIFIER: {
        size_t off = p->cur.start, len = p->cur.limit - p->cur.start;
        if (peek(p).tag == TOKEN_LPAREN && name_is(p, off, len, "__builtin_clzll")) {
            // __builtin_clzll(x): count leading zeros of a 64-bit value (UB at 0), result int
            advance(p);   // name
            advance(p);   // '('
            ExpResult a = convert(p, decay(parse_exp(p)), TY_ULONG);
            if (p->failed) return res_value(ir_constant(0));
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return res_value(ir_constant(0)); }
            advance(p);
            u32 dst = ir_new_temp(p->fn);
            IrInstr in; in.kind = IR_CLZ; in.a = a.val; in.dst = dst; ir_emit(p->fn, in);
            return res_value_typed(ir_var(dst), TY_INT);
        }
        if (peek(p).tag == TOKEN_LPAREN) {
            // `name(...)` is a direct call, UNLESS `name` is a variable holding a function pointer (then
            // it's an indirect call, handled as a postfix `(` suffix after this resolves to a value).
            Bool is_value = 0, found = 0;
            for (size_t i = p->var_count; i > 0; i--) {
                const Var* v = SEG_LIST_REF(Var, &p->vars, i - 1);
                if (src_eq(p, v->name_off, v->name_len, off, len)) { is_value = !v->is_func; found = 1; break; }
            }
            if (!found) {   // a file-scope static variable (e.g. a global function-pointer) is also a value
                u32 gid; GlobalKind gk;
                if (global_find(p, off, len, &gid, &gk) && gk == GLOBAL_STATIC) is_value = 1;
            }
            if (!is_value) return parse_call(p);
        }
        // innermost block binding wins: a local temp, or a static (block-scope `extern`)
        for (size_t i = p->var_count; i > 0; i--) {
            const Var* v = SEG_LIST_REF(Var, &p->vars, i - 1);
            if (v->is_func || !src_eq(p, v->name_off, v->name_len, off, len)) continue;
            advance(p);
            if (!v->is_static) {
                if (type_is_aggregate(v->type)) {              // array/struct value IS its address
                    if (v->by_ref) return res_value_typed(ir_var(v->temp), v->type);  // temp already holds it
                    u32 a = ir_new_ptr_temp(p->fn);
                    IrInstr in; in.kind = IR_ADDROF; in.a = ir_var(v->temp); in.dst = a; ir_emit(p->fn, in);
                    return res_value_typed(ir_var(a), v->type);
                }
                return res_lvalue(v->temp, v->type);
            }
            Type st = SEG_LIST_REF(Global, &p->funcs, v->static_id)->type;
            if (type_is_aggregate(st)) {
                u32 a = ir_new_ptr_temp(p->fn);
                IrInstr in; in.kind = IR_ADDROF_STATIC; in.dst = a; in.u.fun.callee = v->static_id; ir_emit(p->fn, in);
                return res_value_typed(ir_var(a), st);
            }
            u32 dst = temp_for(p->fn, st);
            IrInstr in; in.kind = IR_LOAD_STATIC; in.dst = dst; in.u.fun.callee = v->static_id;
            ir_emit(p->fn, in);
            return res_static(dst, v->static_id, st);
        }
        // otherwise a file-scope static variable
        u32 gid; GlobalKind gk;
        if (global_find(p, off, len, &gid, &gk) && gk == GLOBAL_STATIC) {
            advance(p);
            Type st = SEG_LIST_REF(Global, &p->funcs, gid)->type;
            if (type_is_aggregate(st)) {
                u32 a = ir_new_ptr_temp(p->fn);
                IrInstr in; in.kind = IR_ADDROF_STATIC; in.dst = a; in.u.fun.callee = gid; ir_emit(p->fn, in);
                return res_value_typed(ir_var(a), st);
            }
            u32 dst = temp_for(p->fn, st);
            IrInstr in; in.kind = IR_LOAD_STATIC; in.dst = dst; in.u.fun.callee = gid;
            ir_emit(p->fn, in);
            return res_static(dst, gid, st);
        }
        // an enumeration constant: an int-valued identifier (checked after variables, which can shadow it)
        i64 ev;
        if (enum_const_find(p, off, len, &ev)) {
            advance(p);
            return res_value_typed(ir_constant((u32)ev), TY_INT);
        }
        // a function name used as a value decays to a function pointer (its code address)
        u32 fid; GlobalKind fk;
        if (global_find(p, off, len, &fid, &fk) && fk == GLOBAL_FUNC) {
            advance(p);
            Type ret = SEG_LIST_REF(Global, &p->funcs, fid)->type;
            u32 a = ir_new_ptr_temp(p->fn);
            IrInstr in; in.kind = IR_ADDROF_FUNC; in.dst = a; in.u.fun.callee = fid; ir_emit(p->fn, in);
            return res_value_typed(ir_var(a), funptr_type(ret));
        }
        fail(p, "undeclared variable");
        return res_value(ir_constant(0));
    }

    case TOKEN_LPAREN: {
        advance(p);
        ExpResult r = parse_full_exp(p);
        if (p->failed) return res_value(ir_constant(0));
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return res_value(ir_constant(0)); }
        advance(p);
        return r;   // a parenthesized lvalue is still an lvalue
    }

    default:
        fail(p, "expected an expression");
        return res_value(ir_constant(0));
    }
}

// ptr + index*sizeof(pointee), as an 8-byte pointer (index decayed/converted to long first)
static IrVal pointer_offset(Parser* p, IrVal ptr, Type pointee, ExpResult index)
{
    if (type_is_void(pointee)) { fail(p, "arithmetic on a pointer to void"); return ptr; }
    ExpResult idx = convert(p, decay(index), TY_LONG);
    u32 scaled = ir_new_ptr_temp(p->fn);
    emit_binary(p, IR_MULTIPLY, idx.val, ir_constant_sized(type_size(pointee), 8), scaled, 1);
    u32 res = ir_new_ptr_temp(p->fn);
    emit_binary(p, IR_ADD, ptr, ir_var(scaled), res, 1);
    return ir_var(res);
}

// <postfix> ::= <primary> { "[" <exp> "]" | "++" | "--" }
static ExpResult parse_postfix(Parser* p)
{
    ExpResult e = parse_primary(p);
    if (p->failed) return e;

    for (;;) {
        switch (p->cur.tag) {
        case TOKEN_LBRACKET: {
            advance(p);
            ExpResult index = parse_exp(p);
            if (p->failed) return e;
            if (p->cur.tag != TOKEN_RBRACKET) { fail(p, "expected ']'"); return e; }
            advance(p);
            // subscript is commutative: a[i] == i[a]. Exactly one operand must be a pointer (after
            // decay), the other an integer.
            ExpResult lhs = decay(e), rhs = decay(index);
            ExpResult ptr_e, idx_e;
            if (type_is_pointer(lhs.type) && !type_is_pointer(rhs.type))      { ptr_e = lhs; idx_e = rhs; }
            else if (type_is_pointer(rhs.type) && !type_is_pointer(lhs.type)) { ptr_e = rhs; idx_e = lhs; }
            else { fail(p, "subscript requires one pointer and one integer operand"); return e; }
            if (type_is_double(idx_e.type)) { fail(p, "array subscript is not an integer"); return e; }
            Type elem = referenced(ptr_e.type);
            IrVal addr = pointer_offset(p, ptr_e.val, elem, idx_e);
            if (type_is_aggregate(elem)) { e = res_value_typed(addr, elem); continue; }   // aggregate element: its address
            u32 loaded = temp_for(p->fn, elem);
            IrInstr in; in.kind = IR_LOAD; in.a = addr; in.dst = loaded; ir_emit(p->fn, in);
            e = res_deref(addr, loaded, elem);
            break;
        }
        case TOKEN_DOT: case TOKEN_ARROW: {
            // s.m / p->m : member at base_addr + offset(m). `s.m` uses the struct's address (structs are
            // address-valued); `p->m` == (*p).m uses the pointer value as the base.
            Bool arrow = (p->cur.tag == TOKEN_ARROW);
            advance(p);
            if (p->cur.tag != TOKEN_IDENTIFIER) { fail(p, "expected a member name"); return e; }
            size_t moff = p->cur.start, mlen = p->cur.limit - p->cur.start;
            advance(p);
            IrVal base_addr; Type sty;
            if (arrow) {
                ExpResult pe = decay(e);
                if (!type_is_pointer(pe.type) || !type_is_struct(referenced(pe.type))) { fail(p, "'->' requires a pointer to a struct or union"); return e; }
                base_addr = pe.val; sty = referenced(pe.type);
            } else {
                if (!type_is_struct(e.type)) { fail(p, "'.' requires a struct or union"); return e; }
                base_addr = e.val; sty = e.type;
            }
            const StructDef* sd = sty->sdef;
            if (!sd->complete) { fail(p, "member access on an incomplete type"); return e; }
            const Member* mem = 0;
            for (u32 i = 0; i < sd->member_count; i++)
                if (src_eq(p, sd->members[i].name_off, sd->members[i].name_len, moff, mlen)) { mem = &sd->members[i]; break; }
            if (!mem) { fail(p, "no such member"); return e; }
            IrVal maddr = base_addr;
            if (mem->offset != 0) {
                u32 a = ir_new_ptr_temp(p->fn);
                emit_binary(p, IR_ADD, base_addr, ir_constant_sized(mem->offset, 8), a, 1);
                maddr = ir_var(a);
            }
            if (type_is_aggregate(mem->type)) { e = res_value_typed(maddr, mem->type); }   // aggregate member: its address
            else {
                u32 loaded = temp_for(p->fn, mem->type);
                IrInstr in; in.kind = IR_LOAD; in.a = maddr; in.dst = loaded; ir_emit(p->fn, in);
                e = res_deref(maddr, loaded, mem->type);
            }
            break;
        }
        case TOKEN_LPAREN: {
            // an indirect call: `fp(args)` where `fp` is a function-pointer value. cc has no parameter
            // types for a function pointer, so arguments are passed by their natural (decayed) type.
            ExpResult fe = decay(e);
            if (!type_is_funptr(fe.type)) { fail(p, "called object is not a function or function pointer"); return e; }
            advance(p);   // '('
            StableList argbuf = STABLE_LIST_INIT(IrVal);
            size_t argn = 0;
            if (p->cur.tag != TOKEN_RPAREN) {
                for (;;) {
                    ExpResult a = decay(parse_exp(p));
                    if (p->failed) { stablelist_deinit(&argbuf); return e; }
                    STABLE_LIST_APPEND(IrVal, &argbuf, a.val);
                    argn += 1;
                    if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
                    break;
                }
            }
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); stablelist_deinit(&argbuf); return e; }
            advance(p);
            u32 arg_start = (u32)p->fn->call_arg_count;
            STABLE_LIST_FOREACH(it, &argbuf) ir_add_arg(p->fn, *(IrVal*)it.elem);
            stablelist_deinit(&argbuf);
            Type ret = referenced(fe.type);
            u32 dst = temp_for(p->fn, ret);
            IrInstr in;
            in.kind = IR_CALL_PTR;
            in.a = fe.val;
            in.u.fun.arg_start = arg_start;
            in.u.fun.arg_count = (u32)argn;
            in.dst = dst;
            ir_emit(p->fn, in);
            e = res_value_typed(ir_var(dst), ret);
            break;
        }
        case TOKEN_INCREMENT: case TOKEN_DECREMENT: {
            if (!e.is_lvalue) { fail(p, "expression is not assignable"); return e; }
            Bool inc = p->cur.tag == TOKEN_INCREMENT;
            advance(p);
            u32 old = temp_for(p->fn, e.type);
            emit_copy(p, e.val, old);                                        // the result is the OLD value
            IrVal updated;
            if (type_is_pointer(e.type)) {                                   // ptr++/-- steps by the element size
                updated = pointer_offset(p, e.val, referenced(e.type), res_value(ir_constant(inc ? 1 : -1)));
            } else {
                u32 t = temp_for(p->fn, e.type);
                emit_binary(p, inc ? IR_ADD : IR_SUBTRACT, e.val, ir_constant_sized(1, type_size(e.type)), t, type_unsigned(e.type));
                updated = ir_var(t);
            }
            emit_store(p, e, updated);                                       // x = x +/- 1
            e = res_value_typed(ir_var(old), e.type);
            break;
        }
        default:
            return e;
        }
    }
}

// A cast/type-name uses the same declarator grammar as a real declaration, but its declared name
// must be empty (abstract). parse_declarator is defined below; forward-declare it for the cast path.
static Type parse_declarator(Parser* p, Type base, size_t* noff, size_t* nlen, Bool* is_func);

// A snapshot of the function's IR output, to discard code emitted for a non-evaluated operand
// (sizeof's). The lists are append-only with stable elements, so a rewind just resets the counts.
typedef struct { u32 instr_count, temp_count, label_count, call_arg_count; size_t instrs_n, temps_n, args_n; } EmitMark;
static EmitMark emit_mark(const IrFunc* f)
{
    EmitMark m;
    m.instr_count = f->instr_count;       m.instrs_n = f->lists->instrs.count;
    m.temp_count  = f->temp_count;        m.temps_n  = f->lists->temp_sizes.count;
    m.label_count = f->label_count;
    m.call_arg_count = f->call_arg_count; m.args_n = f->lists->call_args.count;
    return m;
}
static void emit_rewind(IrFunc* f, EmitMark m)
{
    f->instr_count = m.instr_count;       f->lists->instrs.count = m.instrs_n;
    f->temp_count  = m.temp_count;        f->lists->temp_sizes.count = m.temps_n;
    f->label_count = m.label_count;
    f->call_arg_count = m.call_arg_count; f->lists->call_args.count = m.args_n;
}

static void parse_initializer(Parser* p, IrVal addr, Type ty);   // for compound literals (defined below)

// <factor> ::= "sizeof" ( "(" <type-name> ")" | <factor> ) | ( "-" | "~" | "!" | "++" | "--" | "&" ) <factor> | <postfix>
static ExpResult parse_factor(Parser* p)
{
    switch (p->cur.tag) {
    case TOKEN_KW_SIZEOF: {
        // sizeof yields the size as an unsigned long; the operand (a type-name or an expression) is
        // never evaluated, so an expression operand's emitted IR is rewound.
        advance(p);
        Type ty;
        if (p->cur.tag == TOKEN_LPAREN && token_starts_type(p, peek(p))) {
            advance(p);   // (
            if (!parse_type_only(p, &ty)) return res_value(ir_constant(0));
            size_t o, l; Bool isf = 0;
            ty = parse_declarator(p, ty, &o, &l, &isf);   // abstract declarator (pointers/arrays)
            if (p->failed) return res_value(ir_constant(0));
            if (isf || l != 0) { fail(p, "invalid type in sizeof"); return res_value(ir_constant(0)); }
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' after sizeof type"); return res_value(ir_constant(0)); }
            advance(p);   // )
        } else {
            EmitMark mk = emit_mark(p->fn);
            ExpResult e = parse_factor(p);                // sizeof binds like a unary operator
            if (p->failed) return res_value(ir_constant(0));
            emit_rewind(p->fn, mk);                       // the operand is unevaluated
            ty = e.type;                                  // NOT decayed: sizeof an array is its full size
        }
        if (type_is_incomplete(ty)) { fail(p, "sizeof applied to an incomplete type"); return res_value(ir_constant(0)); }
        return res_value_typed(ir_constant_sized(type_size(ty), 8), TY_ULONG);
    }

    case TOKEN_LPAREN:
        // a cast: "(" <type> ")" <factor>. A "(" that doesn't begin a type is a parenthesized
        // expression -- fall through to parse_postfix (via parse_primary).
        if (token_starts_type(p, peek(p))) {
            advance(p);   // (
            Type cast_ty;
            if (!parse_type_only(p, &cast_ty)) return res_value(ir_constant(0));
            size_t coff, clen; Bool cfunc = 0;
            cast_ty = parse_declarator(p, cast_ty, &coff, &clen, &cfunc);
            if (p->failed) return res_value(ir_constant(0));
            if (cfunc || clen != 0) { fail(p, "invalid abstract declarator in cast"); return res_value(ir_constant(0)); }
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' after cast type"); return res_value(ir_constant(0)); }
            advance(p);   // )
            if (p->cur.tag == TOKEN_LBRACE) {
                // compound literal `(type){ init }`: an unnamed object initialized in place. Allocate a
                // slot, initialize it like a local of that type, and yield it (aggregate value = address).
                u32 slot = ir_new_array_temp(p->fn, (u32)type_size(cast_ty));
                u32 addr = ir_new_ptr_temp(p->fn);
                IrInstr ad; ad.kind = IR_ADDROF; ad.a = ir_var(slot); ad.dst = addr; ir_emit(p->fn, ad);
                parse_initializer(p, ir_var(addr), cast_ty);
                if (p->failed) return res_value(ir_constant(0));
                if (type_is_aggregate(cast_ty)) return res_value_typed(ir_var(addr), cast_ty);
                u32 d = temp_for(p->fn, cast_ty);   // a scalar compound literal yields the loaded value
                IrInstr ld; ld.kind = IR_LOAD; ld.a = ir_var(addr); ld.dst = d; ir_emit(p->fn, ld);
                return res_value_typed(ir_var(d), cast_ty);
            }
            if (type_is_array(cast_ty)) { fail(p, "cannot cast to an array type"); return res_value(ir_constant(0)); }
            ExpResult e = parse_factor(p);
            if (p->failed) return e;
            return convert(p, decay(e), cast_ty);   // the cast operand undergoes array-to-pointer decay first
        }
        break;

    case TOKEN_AMP: {
        advance(p);
        if (p->cur.tag == TOKEN_ASTERISK) {
            // &*X is just X: cancel the &/* without performing the dereference (so &*null_ptr is valid).
            advance(p);
            ExpResult e = decay(parse_factor(p));
            if (p->failed) return res_value(ir_constant(0));
            if (!type_is_pointer(e.type)) { fail(p, "cannot dereference a non-pointer"); return res_value(ir_constant(0)); }
            return res_value_typed(e.val, e.type);
        }
        ExpResult e = parse_factor(p);
        if (p->failed) return res_value(ir_constant(0));
        if (type_is_aggregate(e.type)) return res_value_typed(e.val, pointer_to(e.type));   // &arr / &s: the aggregate's address
        if (!e.is_lvalue) { fail(p, "cannot take the address of a non-lvalue"); return res_value(ir_constant(0)); }
        if (e.is_deref) return res_value_typed(e.deref_ptr, pointer_to(e.type));   // &p[i] etc.
        u32 dst = ir_new_ptr_temp(p->fn);
        IrInstr in; in.dst = dst;
        if (e.is_static) { in.kind = IR_ADDROF_STATIC; in.u.fun.callee = e.static_id; }
        else             { in.kind = IR_ADDROF; in.a = ir_var(e.lvalue_temp); }
        ir_emit(p->fn, in);
        return res_value_typed(ir_var(dst), pointer_to(e.type));
    }

    case TOKEN_ASTERISK: {
        advance(p);
        ExpResult e = decay(parse_factor(p));
        if (p->failed) return res_value(ir_constant(0));
        if (type_is_funptr(e.type)) return e;   // *fp yields the function, which immediately decays right back to fp
        if (!type_is_pointer(e.type)) { fail(p, "cannot dereference a non-pointer"); return res_value(ir_constant(0)); }
        Type pointee = referenced(e.type);
        if (type_is_void(pointee)) { fail(p, "cannot dereference a void pointer"); return res_value(ir_constant(0)); }
        if (type_is_aggregate(pointee)) return res_value_typed(e.val, pointee);   // *(ptr-to-aggregate): the address itself
        u32 dst = temp_for(p->fn, pointee);
        IrInstr in; in.kind = IR_LOAD; in.a = e.val; in.dst = dst; ir_emit(p->fn, in);
        return res_deref(e.val, dst, pointee);
    }

    case TOKEN_MINUS: case TOKEN_TILDE: case TOKEN_BANG: {
        IrUnaryOp op = (p->cur.tag == TOKEN_MINUS) ? IR_NEGATE
                        : (p->cur.tag == TOKEN_TILDE) ? IR_COMPLEMENT
                        : IR_NOT;
        advance(p);
        ExpResult a = decay(parse_factor(p));
        if (p->failed) return res_value(ir_constant(0));
        if (!type_is_scalar(a.type)) { fail(p, "operand of a unary operator must be scalar"); return res_value(ir_constant(0)); }
        if (op != IR_NOT && type_is_pointer(a.type)) { fail(p, "invalid operand to unary operator"); return res_value(ir_constant(0)); }
        if (op == IR_COMPLEMENT && type_is_double(a.type)) { fail(p, "operand of ~ must be an integer"); return res_value(ir_constant(0)); }
        if (op != IR_NOT) a = promote(p, a);          // -x/~x integer-promote chars to int
        Type rt = (op == IR_NOT) ? TY_INT : a.type;   // !x is int; -x/~x keep the (promoted) operand's type
        u32 dst = temp_for(p->fn, rt);
        IrInstr instr;
        instr.kind = IR_UNARY;
        instr.op = op;
        instr.a = a.val;
        instr.dst = dst;
        ir_emit(p->fn, instr);
        return res_value_typed(ir_var(dst), rt);
    }

    case TOKEN_INCREMENT: case TOKEN_DECREMENT: {
        // prefix ++x / --x : x = x +/- 1 (scaled by element size for pointers), result is the NEW value
        Bool inc = p->cur.tag == TOKEN_INCREMENT;
        advance(p);
        ExpResult e = parse_factor(p);
        if (p->failed) return res_value(ir_constant(0));
        if (!e.is_lvalue) { fail(p, "expression is not assignable"); return res_value(ir_constant(0)); }
        IrVal updated;
        if (type_is_pointer(e.type)) {
            updated = pointer_offset(p, e.val, referenced(e.type), res_value(ir_constant(inc ? 1 : -1)));
        } else {
            u32 t = temp_for(p->fn, e.type);
            emit_binary(p, inc ? IR_ADD : IR_SUBTRACT, e.val, ir_constant_sized(1, type_size(e.type)), t, type_unsigned(e.type));
            updated = ir_var(t);
        }
        emit_store(p, e, updated);
        return res_value_typed(updated, e.type);
    }

    default:
        break;
    }

    return parse_postfix(p);
}

// <exp> ::= <factor> { <binop> <exp> }  via precedence climbing; assignment is right-associative
static ExpResult parse_exp_prec(Parser* p, int min_prec)
{
    ExpResult left = parse_factor(p);
    if (p->failed) return left;

    for (;;) {
        int prec = binary_prec(p->cur.tag);
        if (prec < min_prec) break;
        u8 tag = p->cur.tag;

        if (is_assign_tok(tag) && type_is_struct(left.type)) {
            // whole-struct assignment: byte copy (left.val is the destination address)
            if (tag != TOKEN_ASSIGN) { fail(p, "compound assignment on a struct/union"); return left; }
            advance(p);
            ExpResult right = parse_exp_prec(p, prec);
            if (p->failed) return left;
            if (!same_type(right.type, left.type)) { fail(p, "incompatible types in struct assignment"); return left; }
            emit_block_copy(p, left.val, right.val, type_size(left.type));
            left = res_value_typed(left.val, left.type);
        } else if (is_assign_tok(tag)) {
            if (!left.is_lvalue) { fail(p, "expression is not assignable"); return left; }
            advance(p);
            ExpResult right = parse_exp_prec(p, prec);   // same prec => right-associative
            if (p->failed) return left;
            IrVal stored;
            if (tag == TOKEN_ASSIGN) {
                stored = convert_to(p, right, left.type).val;   // convert RHS to the lvalue's type
            } else if (type_is_pointer(left.type)) {
                // pointer += / -= integer (scaled by the pointee size); other compound ops on a pointer are invalid
                if ((tag != TOKEN_PLUS_ASSIGN && tag != TOKEN_MINUS_ASSIGN) || !type_is_scalar(right.type) || type_is_pointer(right.type) || type_is_double(right.type)) {
                    fail(p, "invalid operands to compound assignment"); return left;
                }
                ExpResult intop = convert(p, right, TY_LONG);   // widen the index before negating
                if (tag == TOKEN_MINUS_ASSIGN) {
                    u32 neg = temp_for(p->fn, TY_LONG);
                    IrInstr in; in.kind = IR_UNARY; in.op = IR_NEGATE; in.a = intop.val; in.dst = neg; ir_emit(p->fn, in);
                    intop = res_value_typed(ir_var(neg), TY_LONG);
                }
                stored = pointer_offset(p, left.val, referenced(left.type), intop);
            } else {
                if (type_is_pointer(right.type)) { fail(p, "invalid operands to compound assignment"); return left; }
                // x op= e  ==>  x = (lvtype)((commontype)x op (commontype)e)
                Type ct = common_type(left.type, right.type);
                ExpResult lv = convert(p, res_value_typed(left.val, left.type), ct);
                ExpResult rv = convert(p, right, ct);
                u32 tmp = temp_for(p->fn, ct);
                emit_binary(p, compound_op(tag), lv.val, rv.val, tmp, type_unsigned(ct));
                stored = convert(p, res_value_typed(ir_var(tmp), ct), left.type).val;
            }
            emit_store(p, left, stored);
            left = res_value_typed(stored, left.type);   // assignment result is not an lvalue
        } else if (tag == TOKEN_AND) {
            // a && b: if a is 0, short-circuit to 0 without evaluating b
            advance(p);
            if (type_is_void(left.type) || type_is_struct(left.type)) { fail(p, "operand of '&&' must be scalar"); return left; }
            u32 false_label = ir_new_label(p->fn);
            emit_jump_if_zero(p, left.val, false_label);
            ExpResult right = parse_exp_prec(p, prec + 1);
            if (p->failed) return left;
            if (type_is_void(right.type) || type_is_struct(right.type)) { fail(p, "operand of '&&' must be scalar"); return left; }
            emit_jump_if_zero(p, right.val, false_label);
            u32 result = ir_new_temp(p->fn);
            u32 end_label = ir_new_label(p->fn);
            emit_copy(p, ir_constant(1), result);
            emit_jump(p, end_label);
            emit_label(p, false_label);
            emit_copy(p, ir_constant(0), result);
            emit_label(p, end_label);
            left = res_value(ir_var(result));
        } else if (tag == TOKEN_OR) {
            // a || b: if a is nonzero, short-circuit to 1 without evaluating b
            advance(p);
            if (type_is_void(left.type) || type_is_struct(left.type)) { fail(p, "operand of '||' must be scalar"); return left; }
            u32 true_label = ir_new_label(p->fn);
            emit_jump_if_not_zero(p, left.val, true_label);
            ExpResult right = parse_exp_prec(p, prec + 1);
            if (p->failed) return left;
            if (type_is_void(right.type) || type_is_struct(right.type)) { fail(p, "operand of '||' must be scalar"); return left; }
            emit_jump_if_not_zero(p, right.val, true_label);
            u32 result = ir_new_temp(p->fn);
            u32 end_label = ir_new_label(p->fn);
            emit_copy(p, ir_constant(0), result);
            emit_jump(p, end_label);
            emit_label(p, true_label);
            emit_copy(p, ir_constant(1), result);
            emit_label(p, end_label);
            left = res_value(ir_var(result));
        } else if (tag == TOKEN_QUESTION) {
            // cond ? then : else  -- `left` is the condition. The result is the common type of the
            // two branches, but that isn't known until both are parsed, so each branch jumps to a
            // store block emitted afterward (where the conversion to the common type happens).
            advance(p);
            u32 else_label = ir_new_label(p->fn);
            emit_jump_if_zero(p, left.val, else_label);
            ExpResult mid = parse_exp(p);                    // middle is a full expression
            if (p->failed) return left;
            if (p->cur.tag != TOKEN_COLON) { fail(p, "expected ':'"); return left; }
            advance(p);
            u32 then_store = ir_new_label(p->fn);
            emit_jump(p, then_store);                        // then-branch done; store once ct is known
            emit_label(p, else_label);
            ExpResult els = parse_exp_prec(p, prec);         // same prec => right-associative
            if (p->failed) return left;
            if (type_is_void(mid.type) || type_is_void(els.type)) {
                // a void branch makes the whole ?: void (both must then be void); no value to store
                if (!(type_is_void(mid.type) && type_is_void(els.type))) { fail(p, "both branches of ?: must be void or neither"); return left; }
                u32 end_label = ir_new_label(p->fn);
                emit_jump(p, end_label);                     // else branch finished (side effects already emitted)
                emit_label(p, then_store);                   // then branch lands here, falls through
                emit_label(p, end_label);
                left = res_value_typed(ir_constant(0), TY_VOID);
            } else if (type_is_struct(mid.type) || type_is_struct(els.type)) {
                // both branches must be the same struct/union; copy the chosen one into a result slot.
                // The address of that slot is recomputed on each path (the then-branch jumps past the
                // else-branch's setup), so `raddr` is valid however we reach end_label.
                if (!same_type(mid.type, els.type)) { fail(p, "incompatible struct types in ?:"); return left; }
                u64 sz = type_size(mid.type);
                u32 slot = ir_new_array_temp(p->fn, (u32)sz);
                u32 raddr = ir_new_ptr_temp(p->fn);
                u32 end_label = ir_new_label(p->fn);
                IrInstr ad1; ad1.kind = IR_ADDROF; ad1.a = ir_var(slot); ad1.dst = raddr; ir_emit(p->fn, ad1);
                emit_block_copy(p, ir_var(raddr), els.val, sz);   // else path
                emit_jump(p, end_label);
                emit_label(p, then_store);
                IrInstr ad2; ad2.kind = IR_ADDROF; ad2.a = ir_var(slot); ad2.dst = raddr; ir_emit(p->fn, ad2);
                emit_block_copy(p, ir_var(raddr), mid.val, sz);   // then path
                emit_label(p, end_label);
                left = res_value_typed(ir_var(raddr), mid.type);
            } else {
                mid = decay(mid); els = decay(els);   // array/function operands (e.g. a string literal) decay to pointers
                Bool mp = type_is_pointer(mid.type), ep = type_is_pointer(els.type);
                Type ct;
                if (mp || ep) {
                    if (mp && ep) { if (!same_type(mid.type, els.type)) { fail(p, "incompatible pointer types in ternary"); return left; } ct = mid.type; }
                    else if (is_null_constant(mp ? els : mid)) ct = mp ? mid.type : els.type;
                    else { fail(p, "incompatible types in ternary"); return left; }
                } else ct = common_type(mid.type, els.type);
                u32 result = temp_for(p->fn, ct);
                u32 end_label = ir_new_label(p->fn);
                emit_copy(p, convert(p, els, ct).val, result);   // else path stores here, falls to end
                emit_jump(p, end_label);
                emit_label(p, then_store);
                emit_copy(p, convert(p, mid, ct).val, result);   // then path stores here
                emit_label(p, end_label);
                left = res_value_typed(ir_var(result), ct);
            }
        } else {
            IrBinaryOp op = binary_op(tag);
            advance(p);
            ExpResult right = parse_exp_prec(p, prec + 1);   // +1 => left-associative
            if (p->failed) return left;
            left = decay(left); right = decay(right);
            if (!type_is_scalar(left.type) || !type_is_scalar(right.type)) { fail(p, "operand of a binary operator must be scalar"); return left; }
            left = promote(p, left); right = promote(p, right);   // integer promotion (pointers unaffected)

            Bool is_cmp = (op >= IR_EQUAL && op <= IR_GREATER_EQUAL);
            if (op == IR_LSHIFT || op == IR_RSHIFT) {
                if (type_is_double(left.type) || type_is_double(right.type)) { fail(p, "operand of shift must be an integer"); return left; }
                // shifts: the result type is the (promoted) left operand's; the right isn't converted
                u32 dst = temp_for(p->fn, left.type);
                emit_binary(p, op, left.val, right.val, dst, type_unsigned(left.type));
                left = res_value_typed(ir_var(dst), left.type);
            } else if (type_is_pointer(left.type) || type_is_pointer(right.type)) {
                Bool lp = type_is_pointer(left.type), rp = type_is_pointer(right.type);
                if (op == IR_ADD || op == IR_SUBTRACT) {
                    if (lp && rp) {
                        if (op != IR_SUBTRACT || !same_type(left.type, right.type)) { fail(p, "invalid pointer subtraction"); return left; }
                        if (type_is_void(referenced(left.type))) { fail(p, "arithmetic on a pointer to void"); return left; }
                        u32 diff = ir_new_ptr_temp(p->fn);
                        emit_binary(p, IR_SUBTRACT, left.val, right.val, diff, 1);
                        u32 res = ir_new_ptr_temp(p->fn);
                        emit_binary(p, IR_DIVIDE, ir_var(diff), ir_constant_sized(type_size(referenced(left.type)), 8), res, 0);
                        left = res_value_typed(ir_var(res), TY_LONG);
                    } else {
                        if (rp && op == IR_SUBTRACT) { fail(p, "cannot subtract a pointer from an integer"); return left; }
                        ExpResult ptr = lp ? left : right, intop = lp ? right : left;
                        intop = convert(p, intop, TY_LONG);   // widen the index to 64-bit (sign/zero-extend per its type) before negating
                        if (op == IR_SUBTRACT) {
                            u32 neg = temp_for(p->fn, TY_LONG);
                            IrInstr in; in.kind = IR_UNARY; in.op = IR_NEGATE; in.a = intop.val; in.dst = neg; ir_emit(p->fn, in);
                            intop = res_value_typed(ir_var(neg), TY_LONG);
                        }
                        left = res_value_typed(pointer_offset(p, ptr.val, referenced(ptr.type), intop), ptr.type);
                    }
                } else if (is_cmp) {
                    Bool eq = (op == IR_EQUAL || op == IR_NOT_EQUAL);   // only ==/!= accept a null constant
                    if (lp && rp) { if (!same_type(left.type, right.type) && !type_is_void_ptr(left.type) && !type_is_void_ptr(right.type)) { fail(p, "comparison of incompatible pointer types"); return left; } }
                    else if (!(eq && is_null_constant(lp ? right : left))) { fail(p, "invalid pointer comparison"); return left; }
                    // a void* compares against any object pointer; the common type is void* if either is
                    Type pt = type_is_void_ptr(left.type) || type_is_void_ptr(right.type) ? pointer_to(TY_VOID) : (lp ? left.type : right.type);
                    u32 dst = temp_for(p->fn, TY_INT);
                    emit_binary(p, op, convert(p, left, pt).val, convert(p, right, pt).val, dst, 1);
                    left = res_value_typed(ir_var(dst), TY_INT);
                } else { fail(p, "invalid operands to binary operator"); return left; }
            } else {
                // usual arithmetic conversions: convert both operands to their common type
                Type ct = common_type(left.type, right.type);
                if (type_is_double(ct) && (op == IR_REMAINDER || op == IR_BIT_AND || op == IR_BIT_OR || op == IR_BIT_XOR)) {
                    fail(p, "operand of this operator must be an integer"); return left;
                }
                ExpResult lc = convert(p, left, ct);
                ExpResult rc = convert(p, right, ct);
                Type rt = is_cmp ? TY_INT : ct;   // a comparison yields int, arithmetic yields ct
                u32 dst = temp_for(p->fn, rt);
                emit_binary(p, op, lc.val, rc.val, dst, type_unsigned(ct));
                left = res_value_typed(ir_var(dst), rt);
            }
        }
    }
    return left;
}

static ExpResult parse_exp(Parser* p)
{
    return parse_exp_prec(p, 0);
}

// a full expression: assignment-expressions joined by the comma operator. Each operand but the last is
// evaluated for its side effects (already emitted) and its value discarded; the last is the result.
// Used only where a comma is the operator, not a separator (statements, for-clauses, conditions, ...).
static ExpResult parse_full_exp(Parser* p)
{
    ExpResult e = parse_exp(p);
    while (!p->failed && p->cur.tag == TOKEN_COMMA) {
        advance(p);
        e = parse_exp(p);
    }
    return e;
}

// parse just a type (no storage class): int / long / long int / int long / long long
static Bool parse_type_only(Parser* p, Type* type)
{
    while (p->cur.tag == TOKEN_KW_CONST || p->cur.tag == TOKEN_KW_VOLATILE) advance(p);   // leading qualifiers
    if (p->cur.tag == TOKEN_IDENTIFIER) {
        Type td = typedef_find(p, p->cur.start, p->cur.limit - p->cur.start);
        if (td) { advance(p); *type = td; return 1; }
    }
    if (p->cur.tag == TOKEN_KW_STRUCT || p->cur.tag == TOKEN_KW_UNION) { *type = parse_struct_specifier(p); return !p->failed; }
    if (p->cur.tag == TOKEN_KW_ENUM) { *type = parse_enum_specifier(p); return !p->failed; }
    int int_count = 0, long_count = 0, short_count = 0, unsigned_count = 0, signed_count = 0, char_count = 0, double_count = 0, void_count = 0;
    for (;;) {
        switch (p->cur.tag) {
        case TOKEN_KW_INT:      int_count++;      advance(p); break;
        case TOKEN_KW_LONG:     long_count++;     advance(p); break;
        case TOKEN_KW_SHORT:    short_count++;    advance(p); break;
        case TOKEN_KW_UNSIGNED: unsigned_count++; advance(p); break;
        case TOKEN_KW_SIGNED:   signed_count++;   advance(p); break;
        case TOKEN_KW_CHAR:     char_count++;     advance(p); break;
        case TOKEN_KW_DOUBLE:   double_count++;   advance(p); break;
        case TOKEN_KW_VOID:     void_count++;     advance(p); break;
        case TOKEN_KW_CONST: case TOKEN_KW_VOLATILE: advance(p); break;
        default: goto done;
        }
    }
    done:;
    if (int_count > 1 || long_count > 2 || short_count > 1 || unsigned_count > 1 || signed_count > 1 || char_count > 1 || double_count > 1 || void_count > 1 || (unsigned_count && signed_count)
        || (char_count && (int_count || long_count || short_count))
        || (short_count && (long_count || char_count))
        || (double_count && (int_count || long_count || short_count || unsigned_count || signed_count || char_count))
        || (void_count && (int_count || long_count || short_count || unsigned_count || signed_count || char_count || double_count))
        || (int_count == 0 && long_count == 0 && short_count == 0 && unsigned_count == 0 && signed_count == 0 && char_count == 0 && double_count == 0 && void_count == 0)) { fail(p, "invalid type"); return 0; }
    *type = void_count   ? TY_VOID
          : double_count ? TY_DOUBLE
          : char_count   ? (signed_count ? TY_SCHAR : unsigned_count ? TY_UCHAR : TY_CHAR)
          : short_count  ? (unsigned_count ? TY_USHORT : TY_SHORT)
                         : make_int_type(long_count > 0, unsigned_count > 0);
    return 1;
}

// parse a parameter list (after "("): "void" | <type> <id> { "," <type> <id> }; collects the
// Parse a parameter list into `out` (reset first), one NameRef per parameter. `out` is the parser's
// reused scratch; resetting it recycles the prior function's segment (the outer params are already
// consumed before any nested declaration in a body re-enters here).
static void parse_params(Parser* p, SegList* out);   // forward decl: a function declarator parses params

// A declarator AST (parsed outside-in; the type is derived inside-out by process_dc). cc has no
// function type, so a function declarator only describes the thing being declared, not a value type --
// function pointers are unsupported (and not produced by any test/self-host source we target).
typedef enum { DC_NAME, DC_PTR, DC_FUN, DC_ARR } DcKind;
typedef struct struct_Dc { DcKind kind; struct struct_Dc* inner; size_t off, len; u64 count; } Dc;
static Dc* dc_new(DcKind k, Dc* inner) { Dc* d = ARENA_ALLOC(&global_arena, sizeof(Dc)); d->kind = k; d->inner = inner; d->off = 0; d->len = 0; d->count = 0; return d; }

static Dc* parse_declarator_ast(Parser* p);
static Bool parse_const_expr(Parser* p, i64* out);   // an integer constant expression (array dims, enum values)

// simple-declarator ::= identifier | "(" declarator ")"  (empty for an abstract/unnamed declarator)
static Dc* parse_simple_dc(Parser* p)
{
    if (p->cur.tag == TOKEN_LPAREN) {
        Token nx = peek(p);
        // a parenthesized declarator if what follows starts one; otherwise the '(' is a function
        // parameter list (handled as a suffix) and this declarator is unnamed
        if (nx.tag == TOKEN_ASTERISK || nx.tag == TOKEN_LPAREN || nx.tag == TOKEN_LBRACKET || (nx.tag == TOKEN_IDENTIFIER && !token_starts_type(p, nx))) {
            advance(p);
            Dc* d = parse_declarator_ast(p);
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return d; }
            advance(p);
            return d;
        }
        return dc_new(DC_NAME, 0);
    }
    Dc* d = dc_new(DC_NAME, 0);
    if (p->cur.tag == TOKEN_IDENTIFIER && !is_reserved(p)) { d->off = p->cur.start; d->len = p->cur.limit - p->cur.start; advance(p); }
    return d;
}

// direct-declarator ::= simple-declarator ( "(" params ")" | "[" size "]" )*
static Dc* parse_direct_dc(Parser* p)
{
    Dc* d = parse_simple_dc(p);
    if (p->cur.tag == TOKEN_LPAREN) {                  // function suffix
        advance(p);                                    // past '('
        if (d->kind == DC_PTR) {
            // a function-POINTER declarator, e.g. `(*f)(params)`. cc's funptr type is the return type
            // only, so its parameter list is decorative -- skip it to the matching ')'. (Don't run
            // parse_params here: it reuses the shared p->params, which would clobber the enclosing real
            // function's parameters -- this very funptr declarator may be one of those parameters.)
            for (int depth = 1; p->cur.tag != TOKEN_EOF; advance(p)) {
                if (p->cur.tag == TOKEN_LPAREN) depth++;
                else if (p->cur.tag == TOKEN_RPAREN && --depth == 0) break;
            }
        } else {
            parse_params(p, &p->params);               // a real function: these are the declaration's params
        }
        if (p->failed) return d;
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return d; }
        advance(p);
        return dc_new(DC_FUN, d);
    }
    while (p->cur.tag == TOKEN_LBRACKET) {              // array suffix(es); leftmost is the outermost
        advance(p);
        u64 n = 0;                                      // 0 == unspecified `[]`, deduced from the initializer
        if (p->cur.tag != TOKEN_RBRACKET) {
            i64 ev;                                     // an integer constant expression (sizeof, enums, arithmetic)
            if (!parse_const_expr(p, &ev)) return d;
            if (ev < 0) { fail(p, "array size is negative"); return d; }
            n = (u64)ev;
            if (p->cur.tag != TOKEN_RBRACKET) { fail(p, "expected ']'"); return d; }
        }
        advance(p);   // ]
        Dc* a = dc_new(DC_ARR, d); a->count = n; d = a;
    }
    return d;
}

// declarator ::= "*" [qualifiers] declarator | direct-declarator
static Dc* parse_declarator_ast(Parser* p)
{
    if (p->cur.tag == TOKEN_ASTERISK) {
        advance(p);
        while (p->cur.tag == TOKEN_KW_CONST || p->cur.tag == TOKEN_KW_VOLATILE) advance(p);   // `* const` etc.
        return dc_new(DC_PTR, parse_declarator_ast(p));
    }
    return parse_direct_dc(p);
}

// Derive the type from `base` (inside-out), set the name (off/len; 0,0 = abstract), and set *is_func
// for a function declarator (whose params are left in p->params).
static Type process_dc(Parser* p, Dc* d, Type base, size_t* noff, size_t* nlen, Bool* is_func)
{
    if (d->kind == DC_NAME) { *noff = d->off; *nlen = d->len; return base; }
    if (d->kind == DC_PTR)  return process_dc(p, d->inner, pointer_to(base), noff, nlen, is_func);
    if (d->kind == DC_ARR) {
        if (type_is_incomplete(base)) { fail(p, "array of incomplete element type"); *noff = 0; *nlen = 0; return base; }
        return process_dc(p, d->inner, array_of(base, d->count), noff, nlen, is_func);
    }
    // a function declarator. `T name(...)` is a function (inner is the bare name). `T (*name)(...)` is a
    // pointer to function returning T -- the inner is a pointer declarator, and the funptr type absorbs it.
    if (d->inner->kind == DC_PTR)
        return process_dc(p, d->inner->inner, funptr_type(base), noff, nlen, is_func);
    if (d->inner->kind != DC_NAME) { fail(p, "unsupported function declarator"); *noff = 0; *nlen = 0; return base; }
    *is_func = 1;
    return process_dc(p, d->inner, base, noff, nlen, is_func);
}

// Parse a declarator after the base type: returns the derived type, the name (0,0 if abstract), and
// whether it is a function declarator (its params left in p->params).
static Type parse_declarator(Parser* p, Type base, size_t* noff, size_t* nlen, Bool* is_func)
{
    *noff = 0; *nlen = 0; *is_func = 0;
    Dc* d = parse_declarator_ast(p);
    if (p->failed) return base;
    return process_dc(p, d, base, noff, nlen, is_func);
}

// find an in-scope tag, searching the StructDefs from index `from` upward (newest first). `from` =
// 0 finds any visible tag (a reference); `from` = struct_scope_start finds only the current block's
// tags (so a `struct s { ... }` in an inner block defines a new type instead of redefining an outer one).
static StructDef* struct_find(Parser* p, size_t tag_off, size_t tag_len, size_t from)
{
    for (size_t i = p->struct_def_count; i > from; i--) {
        StructDef* sd = SEG_LIST_REF(StructDef, &p->struct_defs, i - 1);
        if (sd->in_scope && sd->tag_len == tag_len && src_eq(p, sd->tag_off, sd->tag_len, tag_off, tag_len)) return sd;
    }
    return 0;
}
// close the innermost tag scope: the tags it declared stay allocated (TypeNodes point at them) but
// become unfindable, and `struct_scope_start` is restored to the enclosing block's.
static void pop_struct_scope(Parser* p, size_t outer_start)
{
    for (size_t i = p->struct_scope_start; i < p->struct_def_count; i++)
        SEG_LIST_REF(StructDef, &p->struct_defs, i)->in_scope = 0;
    p->struct_scope_start = outer_start;
}

// ---- constant-expression evaluator ---------------------------------------------------------------
// Folds a constant expression directly to a value with no IR emission, so it works at file scope (enum
// initializers, array dimensions, static initializers). A ConstVal is either an integer or a double;
// casts and double arithmetic are handled, so a static initializer like `(int)(-.867 * R)` folds.
// parse_const_expr requires an integral result (array dims, enums); parse_const_init accepts either.

typedef struct { Bool is_double; union { i64 i; double d; } v; } ConstVal;
static ConstVal cv_int(i64 i)    { ConstVal c; c.is_double = 0; c.v.i = i; return c; }
static ConstVal cv_dbl(double d) { ConstVal c; c.is_double = 1; c.v.d = d; return c; }
static i64    cv_to_i64(ConstVal c) { return c.is_double ? (i64)c.v.d : c.v.i; }
static double cv_to_dbl(ConstVal c) { return c.is_double ? c.v.d : (double)c.v.i; }

static Bool const_eval_cond(Parser* p, ConstVal* out);   // fwd: parenthesized sub-expressions / ?:
static Bool const_eval_unary(Parser* p, ConstVal* out);  // fwd: a cast's operand is a unary/cast-expression

// convert a folded value to a cast's target type (double target -> double; any other -> integer value)
static ConstVal const_cast_to(Type ty, ConstVal v)
{
    return type_is_double(ty) ? cv_dbl(cv_to_dbl(v)) : cv_int(cv_to_i64(v));
}

static Bool const_eval_primary(Parser* p, ConstVal* out)
{
    switch (p->cur.tag) {
    case TOKEN_LPAREN:
        if (token_starts_type(p, peek(p))) {            // a cast: "(" <type> ")" <cast-expression>
            advance(p);   // (
            Type cast_ty;
            if (!parse_type_only(p, &cast_ty)) return 0;
            size_t coff, clen; Bool cfunc = 0;
            cast_ty = parse_declarator(p, cast_ty, &coff, &clen, &cfunc);
            if (p->failed) return 0;
            if (cfunc || clen != 0) { fail(p, "invalid abstract declarator in cast"); return 0; }
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' after cast type"); return 0; }
            advance(p);   // )
            ConstVal v;
            if (!const_eval_unary(p, &v)) return 0;
            *out = const_cast_to(cast_ty, v);
            return 1;
        }
        advance(p);
        if (!const_eval_cond(p, out)) return 0;
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' in constant expression"); return 0; }
        advance(p);
        return 1;
    case TOKEN_CONSTANT_INT: {
        if (p->src[p->cur.start] == '\'') { *out = cv_int(char_constant_value(p->src, p->cur.start)); advance(p); return 1; }
        Bool eu, el; *out = cv_int((i64)int_token_value(p->src, p->cur.start, p->cur.limit, &eu, &el));
        advance(p);
        return 1;
    }
    case TOKEN_CONSTANT_FLOAT: {
        union { u64 u; double d; } pun; pun.u = parse_double_bits(p->src, p->cur.start, p->cur.limit);
        *out = cv_dbl(pun.d);
        advance(p);
        return 1;
    }
    case TOKEN_IDENTIFIER: {
        i64 ev;
        if (enum_const_find(p, p->cur.start, p->cur.limit - p->cur.start, &ev)) { *out = cv_int(ev); advance(p); return 1; }
        break;
    }
    }
    fail(p, "expected a constant expression");
    return 0;
}

static Bool const_eval_unary(Parser* p, ConstVal* out)
{
    switch (p->cur.tag) {
    case TOKEN_PLUS:  advance(p); return const_eval_unary(p, out);
    case TOKEN_MINUS: advance(p); if (!const_eval_unary(p, out)) return 0;
        *out = out->is_double ? cv_dbl(-out->v.d) : cv_int(-out->v.i); return 1;
    case TOKEN_TILDE: advance(p); if (!const_eval_unary(p, out)) return 0;
        if (out->is_double) { fail(p, "operator '~' requires an integer constant"); return 0; }
        *out = cv_int(~out->v.i); return 1;
    case TOKEN_BANG:  advance(p); if (!const_eval_unary(p, out)) return 0;
        *out = cv_int(out->is_double ? !(out->v.d != 0) : !out->v.i); return 1;
    case TOKEN_KW_SIZEOF: {
        // reuse the full sizeof parser (type-name or unevaluated expression); it folds to a constant
        ExpResult e = parse_factor(p);
        if (p->failed) return 0;
        if (e.val.kind != IR_CONSTANT) { fail(p, "sizeof operand is not a constant expression"); return 0; }
        *out = cv_int((i64)e.val.value);
        return 1;
    }
    default: return const_eval_primary(p, out);
    }
}

// binary operator precedence (higher binds tighter); 0 means "not a binary operator"
static int const_binop_prec(u8 tag)
{
    switch (tag) {
    case TOKEN_ASTERISK: case TOKEN_SLASH: case TOKEN_PERCENT: return 10;
    case TOKEN_PLUS: case TOKEN_MINUS:                         return 9;
    case TOKEN_LSHIFT: case TOKEN_RSHIFT:                      return 8;
    case TOKEN_LESS: case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER: case TOKEN_GREATER_EQUAL:              return 7;
    case TOKEN_EQUAL_EQUAL: case TOKEN_NOT_EQUAL:              return 6;
    case TOKEN_AMP:                                           return 5;
    case TOKEN_CARET:                                         return 4;
    case TOKEN_PIPE:                                          return 3;
    case TOKEN_AND:                                           return 2;
    case TOKEN_OR:                                            return 1;
    default:                                                 return 0;
    }
}

static i64 const_apply_binop(u8 tag, i64 a, i64 b)
{
    switch (tag) {
    case TOKEN_ASTERISK:      return a * b;
    case TOKEN_SLASH:         return b ? a / b : 0;
    case TOKEN_PERCENT:       return b ? a % b : 0;
    case TOKEN_PLUS:          return a + b;
    case TOKEN_MINUS:         return a - b;
    case TOKEN_LSHIFT:        return a << b;
    case TOKEN_RSHIFT:        return a >> b;
    case TOKEN_LESS:          return a < b;
    case TOKEN_LESS_EQUAL:    return a <= b;
    case TOKEN_GREATER:       return a > b;
    case TOKEN_GREATER_EQUAL: return a >= b;
    case TOKEN_EQUAL_EQUAL:   return a == b;
    case TOKEN_NOT_EQUAL:     return a != b;
    case TOKEN_AMP:           return a & b;
    case TOKEN_CARET:         return a ^ b;
    case TOKEN_PIPE:          return a | b;
    case TOKEN_AND:           return a && b;
    case TOKEN_OR:            return a || b;
    default:                  return 0;
    }
}

// apply a binary operator to two folded values: double arithmetic if either side is a double (and the
// integer-only operators -- %, shifts, bitwise -- are then rejected), otherwise the integer ladder.
static ConstVal const_apply(Parser* p, u8 op, ConstVal a, ConstVal b)
{
    if (!a.is_double && !b.is_double) return cv_int(const_apply_binop(op, a.v.i, b.v.i));
    double x = cv_to_dbl(a), y = cv_to_dbl(b);
    switch (op) {
    case TOKEN_ASTERISK:      return cv_dbl(x * y);
    case TOKEN_SLASH:         return cv_dbl(y != 0 ? x / y : 0);
    case TOKEN_PLUS:          return cv_dbl(x + y);
    case TOKEN_MINUS:         return cv_dbl(x - y);
    case TOKEN_LESS:          return cv_int(x < y);
    case TOKEN_LESS_EQUAL:    return cv_int(x <= y);
    case TOKEN_GREATER:       return cv_int(x > y);
    case TOKEN_GREATER_EQUAL: return cv_int(x >= y);
    case TOKEN_EQUAL_EQUAL:   return cv_int(x == y);
    case TOKEN_NOT_EQUAL:     return cv_int(x != y);
    case TOKEN_AND:           return cv_int((x != 0) && (y != 0));
    case TOKEN_OR:            return cv_int((x != 0) || (y != 0));
    default: fail(p, "operator requires integer constants"); return cv_int(0);   // %, <<, >>, &, ^, |
    }
}

static Bool const_eval_binary(Parser* p, int min_prec, ConstVal* out)
{
    if (!const_eval_unary(p, out)) return 0;
    for (;;) {
        int prec = const_binop_prec(p->cur.tag);
        if (prec == 0 || prec < min_prec) break;
        u8 op = p->cur.tag;
        advance(p);
        ConstVal rhs;
        if (!const_eval_binary(p, prec + 1, &rhs)) return 0;   // left-assoc
        *out = const_apply(p, op, *out, rhs);
        if (p->failed) return 0;
    }
    return 1;
}

static Bool const_eval_cond(Parser* p, ConstVal* out)   // ?: (lowest precedence)
{
    if (!const_eval_binary(p, 1, out)) return 0;
    if (p->cur.tag == TOKEN_QUESTION) {
        advance(p);
        ConstVal t, f;
        if (!const_eval_cond(p, &t)) return 0;
        if (p->cur.tag != TOKEN_COLON) { fail(p, "expected ':' in constant expression"); return 0; }
        advance(p);
        if (!const_eval_cond(p, &f)) return 0;
        *out = (out->is_double ? out->v.d != 0 : out->v.i != 0) ? t : f;
    }
    return 1;
}

static Bool const_eval(Parser* p, ConstVal* out) { return const_eval_cond(p, out); }   // int or double

static Bool parse_const_expr(Parser* p, i64* out)   // an INTEGER constant expression (array dims, enums, cases)
{
    ConstVal v;
    if (!const_eval(p, &v)) return 0;
    if (v.is_double) { fail(p, "expected an integer constant expression"); return 0; }
    *out = v.v.i;
    return 1;
}

static Bool parse_enum_value(Parser* p, i64* out) { return parse_const_expr(p, out); }

// parse an `enum` specifier: `enum [tag] [{ name [= const], ... }]`. cc gives every enum the type int
// and doesn't intern enum tags; a `{ ... }` body binds its enumerators as int constants in scope.
static Type parse_enum_specifier(Parser* p)
{
    advance(p);   // 'enum'
    if (p->cur.tag == TOKEN_IDENTIFIER) advance(p);   // optional tag (not interned: all enums are int)
    if (p->cur.tag != TOKEN_LBRACE) return TY_INT;    // a bare `enum Tag` reference
    advance(p);   // '{'
    i64 next = 0;
    while (p->cur.tag != TOKEN_RBRACE) {
        if (p->cur.tag != TOKEN_IDENTIFIER || is_reserved(p)) { fail(p, "expected an enumerator name"); return TY_INT; }
        size_t off = p->cur.start, len = p->cur.limit - p->cur.start;
        advance(p);
        i64 val = next;
        if (p->cur.tag == TOKEN_ASSIGN) {
            advance(p);
            if (!parse_enum_value(p, &val)) return TY_INT;
        }
        enum_const_add(p, off, len, val);
        next = val + 1;
        if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
        break;
    }
    if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected ',' or '}' in enum"); return TY_INT; }
    advance(p);   // '}'
    return TY_INT;
}

// parse a `struct`/`union` specifier: `struct/union [tag] [{ member ... }]`. A bare reference returns
// the (possibly incomplete) interned StructDef's type; a `{ ... }` body completes it in place. Members
// follow C padding: each is placed at the next offset aligned to its alignment, the struct's alignment
// is its widest member's, and its size is rounded up to that. A union overlaps all members at offset 0.
static Type parse_struct_specifier(Parser* p)
{
    Bool is_union = (p->cur.tag == TOKEN_KW_UNION);
    advance(p);
    size_t tag_off = 0, tag_len = 0;
    if (p->cur.tag == TOKEN_IDENTIFIER) { tag_off = p->cur.start; tag_len = p->cur.limit - p->cur.start; advance(p); }
    if (tag_len == 0 && p->cur.tag != TOKEN_LBRACE) { fail(p, "expected a tag or '{' after struct/union"); return TY_INT; }

    Bool has_body = p->cur.tag == TOKEN_LBRACE;
    // a definition (`{ ... }`) only matches a tag in the *current* block (else it's a fresh, shadowing
    // type); a bare reference matches any visible tag.
    StructDef* sd = tag_len ? struct_find(p, tag_off, tag_len, has_body ? p->struct_scope_start : 0) : 0;
    if (sd && sd->is_union != is_union) { fail(p, "tag redeclared as a different kind of type"); return TY_INT; }
    if (!sd) {
        StructDef fresh = {0};
        fresh.tag_off = tag_off; fresh.tag_len = tag_len; fresh.is_union = is_union; fresh.in_scope = 1;
        sd = SEG_LIST_APPEND(StructDef, &p->struct_defs, fresh);
        p->struct_def_count++;
    }

    if (!has_body) return struct_type(sd);   // a reference, not a definition

    if (sd->complete) { fail(p, "redefinition of struct/union"); return struct_type(sd); }
    advance(p);   // {
    SegList ms = SEG_LIST_INIT(Member);
    size_t mcount = 0;
    u64 offset = 0, size = 0;
    u32 align = 1;
    while (p->cur.tag != TOKEN_RBRACE && !p->failed) {
        StorageClass msc; Type base;
        if (!parse_specifiers(p, &msc, &base)) break;
        if (msc != SC_NONE) { fail(p, "a struct/union member cannot have a storage class"); break; }
        while (1) {                                  // one or more comma-separated declarators share `base`
            size_t moff, mlen; Bool mfunc = 0;
            Type mty = parse_declarator(p, base, &moff, &mlen, &mfunc);
            if (p->failed) break;
            if (mfunc) { fail(p, "a struct/union member cannot be a function"); break; }
            if (mlen == 0) { fail(p, "expected a member name"); break; }
            if (type_is_void(mty) || (type_is_struct(mty) && !mty->sdef->complete)) { fail(p, "member has incomplete type"); break; }
            for (size_t i = 0; i < mcount; i++) {
                Member prev = SEG_LIST_VAL(Member, &ms, i);
                if (src_eq(p, prev.name_off, prev.name_len, moff, mlen)) { fail(p, "duplicate member name"); break; }
            }
            if (p->failed) break;
            u32 ma = type_align(mty);
            if (ma > align) align = ma;
            if (!is_union) offset = (offset + ma - 1) & ~((u64)ma - 1);
            Member m = { moff, mlen, mty, is_union ? 0 : offset };
            SEG_LIST_APPEND(Member, &ms, m);
            mcount++;
            u64 msz = type_size(mty);
            if (is_union) { if (msz > size) size = msz; }
            else          offset += msz;
            if (p->cur.tag != TOKEN_COMMA) break;
            advance(p);   // ,
        }
        if (p->failed) break;
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';' after member"); break; }
        advance(p);
    }
    if (!p->failed && mcount == 0) fail(p, "struct/union has no members");
    if (!p->failed && p->cur.tag != TOKEN_RBRACE) fail(p, "expected '}'");
    if (p->failed) { seg_list_deinit(&ms); return struct_type(sd); }
    advance(p);   // }
    if (!is_union) size = offset;
    size = (size + align - 1) & ~((u64)align - 1);
    Member* arr = ARENA_ALLOC(&global_arena, mcount * sizeof(Member));
    for (size_t i = 0; i < mcount; i++) arr[i] = SEG_LIST_VAL(Member, &ms, i);
    seg_list_deinit(&ms);
    sd->members = arr; sd->member_count = (u32)mcount; sd->size = size; sd->align = align; sd->complete = 1;
    return struct_type(sd);
}

static void parse_params(Parser* p, SegList* out)
{
    seg_list_deinit(out);
    *out = SEG_LIST_INIT(NameRef);
    p->params_unspecified = 0;
    if (p->cur.tag == TOKEN_RPAREN) { p->params_unspecified = 1; return; }   // empty `()`: K&R unspecified params, not zero
    if (p->cur.tag == TOKEN_KW_VOID && peek(p).tag == TOKEN_RPAREN) {
        advance(p);
        return;
    }
    for (;;) {
        Type pty;
        if (!parse_type_only(p, &pty)) break;
        size_t pname_off, pname_len; Bool pfunc = 0;
        pty = parse_declarator(p, pty, &pname_off, &pname_len, &pfunc);
        if (p->failed) break;
        // a `void` by-value parameter is always invalid; an incomplete struct/union is allowed here
        // (this may be a prototype -- completeness is only required at the definition and at calls).
        if (type_is_void(pty)) { fail(p, "parameter has incomplete type 'void'"); break; }
        if (type_is_array(pty)) pty = pointer_to(referenced(pty));   // an array parameter adjusts to a pointer
        // an unnamed parameter (pname_len == 0) is legal in a prototype; it just can't be referenced
        for (size_t i = 0; pname_len && i < out->count; i++) {
            NameRef prev = SEG_LIST_VAL(NameRef, out, i);
            if (prev.len && src_eq(p, prev.off, prev.len, pname_off, pname_len)) { fail(p, "duplicate parameter name"); break; }
        }
        if (p->failed) break;
        SEG_LIST_APPEND(NameRef, out, ((NameRef){ pname_off, pname_len, pty }));
        if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
        break;
    }
}

// parse a static/file-scope initializer constant and convert it (at compile time) to the variable's
// type `ty`, returning the 64-bit pattern for the .data slot. An integer->integer initializer is
// stored as-is (codegen reads it at the variable's width/signedness); double conversions go through
// host double arithmetic.
static Bool parse_const_init(Parser* p, Type ty, u64* out)
{
    // a double-typed static takes a single floating/integer constant (no const-expr folding for doubles)
    if (type_is_double(ty)) {
        if (p->cur.tag == TOKEN_CONSTANT_INT) {
            if (p->src[p->cur.start] == '\'') {                          // character constant: int-valued
                i64 cv = char_constant_value(p->src, p->cur.start);
                union { double d; u64 u; } pun; pun.d = (double)cv; *out = pun.u;
                advance(p);
                return 1;
            }
            Bool has_u, has_l;
            u64 v = int_token_value(p->src, p->cur.start, p->cur.limit, &has_u, &has_l);
            union { double d; u64 u; } pun;                          // double <- integer constant
            pun.d = has_u ? (double)v : (double)(i64)v;
            *out = pun.u;
            advance(p);
            return 1;
        }
        if (p->cur.tag == TOKEN_CONSTANT_FLOAT) {
            union { u64 u; double d; } pun;
            pun.u = parse_double_bits(p->src, p->cur.start, p->cur.limit);
            *out = pun.u;                                        // double <- floating constant
            advance(p);
            return 1;
        }
        fail(p, "initializer must be a constant"); return 0;
    }
    // an integer-typed static: fold a constant expression (which may use casts and double arithmetic,
    // e.g. `(int)(-.867 * R)` or a bare `4.9`) and convert the result to the target integer type.
    ConstVal cv;
    if (!const_eval(p, &cv)) return 0;
    *out = cv.is_double ? (type_unsigned(ty) ? (u64)cv.v.d : (u64)(i64)cv.v.d) : (u64)cv.v.i;
    return 1;
}

// record a fixup at `offset` within the current static's image, naming a TU-local target global (a
// function or another static); the linker resolves the target's kind and final address.
static void record_static_reloc(Parser* p, u32 offset, u32 target)
{
    StaticReloc r = { offset, target };
    if (p->static_reloc_count < p->static_relocs.count) *SEG_LIST_REF(StaticReloc, &p->static_relocs, p->static_reloc_count) = r;
    else SEG_LIST_APPEND(StaticReloc, &p->static_relocs, r);
    p->static_reloc_count++;
}

// Materialize a static initializer for type `ty` into `buf` (which the caller pre-zeroed to
// type_size(ty) bytes): braces with per-element recursion + zero-filled tail for arrays, a single
// converted constant written little-endian for scalars. Static initializers must be constants.
// `base` is the static's image start (for computing relocation offsets within it).
static Bool static_init_image(Parser* p, Type ty, u8* buf, u8* base)
{
    if (type_is_struct(ty)) {
        const StructDef* sd = ty->sdef;
        if (p->cur.tag != TOKEN_LBRACE) { fail(p, "struct requires a braced initializer"); return 0; }
        advance(p);
        u32 n = sd->is_union ? 1 : sd->member_count;
        u32 i = 0;
        if (p->cur.tag != TOKEN_RBRACE) for (;;) {
            if (i >= n) { fail(p, "too many initializers"); return 0; }
            if (!static_init_image(p, sd->members[i].type, buf + sd->members[i].offset, base)) return 0;
            i++;
            if (p->cur.tag != TOKEN_COMMA) break;
            advance(p);
            if (p->cur.tag == TOKEN_RBRACE) break;
        }
        if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected '}'"); return 0; }
        advance(p);
        return 1;                                            // unspecified members stay zero
    }
    if (type_is_array(ty)) {
        Type elem = referenced(ty); u32 esz = type_size(elem);
        if (type_is_char(elem) && p->cur.tag == TOKEN_STRING) {   // char array initialized by a string literal
            u32 len; const u8* bytes = decode_string_run(p, &len);
            if (len > ty->count) { fail(p, "string initializer is too long"); return 0; }
            u32 ncopy = (len + 1 <= ty->count) ? len + 1 : (u32)ty->count;   // include the null only if it fits
            for (u32 k = 0; k < ncopy; k++) buf[k] = bytes[k];
            return 1;                                            // remaining bytes stay zero
        }
        if (p->cur.tag != TOKEN_LBRACE) { fail(p, "array requires a braced initializer"); return 0; }
        advance(p);
        if (p->cur.tag == TOKEN_RBRACE) { fail(p, "empty initializer list"); return 0; }
        u64 i = 0;
        for (;;) {
            u64 lo = i, hi = i;                          // positional: a single element at `i`
            if (p->cur.tag == TOKEN_LBRACKET) {          // designator: `[idx] =` or GNU range `[lo ... hi] =`
                advance(p);
                i64 a;
                if (!parse_const_expr(p, &a)) return 0;
                i64 b = a;
                if (p->cur.tag == TOKEN_ELLIPSIS) { advance(p); if (!parse_const_expr(p, &b)) return 0; }
                if (a < 0 || b < a) { fail(p, "invalid array designator"); return 0; }
                if (p->cur.tag != TOKEN_RBRACKET) { fail(p, "expected ']'"); return 0; }
                advance(p);
                if (p->cur.tag != TOKEN_ASSIGN) { fail(p, "expected '=' after array designator"); return 0; }
                advance(p);
                lo = (u64)a; hi = (u64)b;
            }
            if (hi >= ty->count) { fail(p, "too many array initializers"); return 0; }
            if (!static_init_image(p, elem, buf + lo * esz, base)) return 0;   // the first element of the range
            for (u64 k = lo + 1; k <= hi; k++)                                 // replicate across the range
                for (u32 z = 0; z < esz; z++) buf[k * esz + z] = buf[lo * esz + z];
            i = hi + 1;
            if (p->cur.tag != TOKEN_COMMA) break;
            advance(p);
            if (p->cur.tag == TOKEN_RBRACE) break;   // trailing comma
        }
        if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected '}'"); return 0; }
        advance(p);
        return 1;                                    // unspecified elements stay zero (buffer pre-zeroed)
    }
    // an optional cast prefix on a pointer initializer, e.g. `(actionf_p1)A_Light0` or `(void*)0`: the
    // cast only reinterprets the type, so skip it and materialize the address constant / value below.
    if ((type_is_funptr(ty) || type_is_pointer(ty)) && p->cur.tag == TOKEN_LPAREN && token_starts_type(p, peek(p))) {
        advance(p);   // (
        Type cast_ty;
        if (!parse_type_only(p, &cast_ty)) return 0;
        size_t coff, clen; Bool cfunc = 0;
        cast_ty = parse_declarator(p, cast_ty, &coff, &clen, &cfunc);
        if (p->failed) return 0;
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' after cast type"); return 0; }
        advance(p);   // )
    }
    // a string literal as a pointer initializer (e.g. `char* p = "..."` or an element of a `char*[]`):
    // its bytes live in an anonymous internal static; record a relocation to that static, 8 bytes zeroed.
    if (type_is_pointer(ty) && same_type(referenced(ty), TY_CHAR) && p->cur.tag == TOKEN_STRING) {
        u32 len; const u8* bytes = decode_string_run(p, &len);
        u32 sid = local_static_declare(p, 0, 0, bytes, len + 1, 0, 0, array_of(TY_CHAR, (u64)len + 1));
        record_static_reloc(p, (u32)(buf - base), sid);
        return 1;
    }
    // an address constant: an optional '&', an object/function name, then constant subscripts. Covers a
    // function name (a vtable slot `{ some_func }`), an array name that decays (`funcs = ntdll_funcs`),
    // `&mousearray[1]`, and `cheat_powerup_seq[0]` (a subarray that decays to a pointer). `&arr[i]` and
    // `arr[i]` resolve to the same address, so '&' is optional and does not change the offset. Record a
    // relocation to the base object; the byte offset is the addend, stored in the slot, which the exe
    // layer adds to the resolved base address.
    if (type_is_funptr(ty) || type_is_pointer(ty)) {
        Bool addr_of = p->cur.tag == TOKEN_AMP;
        if (addr_of && peek(p).tag != TOKEN_IDENTIFIER) { fail(p, "expected an object name after '&' in a static initializer"); return 0; }
        if (addr_of) advance(p);   // '&'
        if (p->cur.tag == TOKEN_IDENTIFIER) {
            u32 gid; GlobalKind gk;
            if (global_find(p, p->cur.start, p->cur.limit - p->cur.start, &gid, &gk)) {
                Type t = SEG_LIST_REF(Global, &p->funcs, gid)->type;
                advance(p);   // id
                u64 addend = 0;
                while (p->cur.tag == TOKEN_LBRACKET) {   // a constant subscript: only an array has a compile-time address
                    advance(p);
                    i64 idx;
                    if (!parse_const_expr(p, &idx)) return 0;
                    if (p->cur.tag != TOKEN_RBRACKET) { fail(p, "expected ']'"); return 0; }
                    advance(p);
                    if (!type_is_array(t)) { fail(p, "non-constant subscript in a static initializer"); return 0; }
                    t = referenced(t);
                    addend += (u64)idx * type_size(t);
                }
                for (u32 k = 0; k < 8; k++) buf[k] = (u8)(addend >> (k * 8));   // the addend lives in the slot
                record_static_reloc(p, (u32)(buf - base), gid);
                return 1;
            }
            if (addr_of) { fail(p, "unknown object in static initializer"); return 0; }
        }
    }
    u64 v;
    if (!parse_const_init(p, ty, &v)) return 0;
    if (type_is_pointer(ty) && v != 0) { fail(p, "invalid initializer for pointer"); return 0; }
    u32 sz = type_size(ty);
    for (u32 k = 0; k < sz; k++) buf[k] = (u8)(v >> (k * 8));
    return 1;
}

// p->cur is '{'. Count the top-level (brace-depth 0) elements of a braced initializer without consuming
// input -- the lexer is stateless, so we save/restore the parser position and scan ahead.
static u64 count_init_elements(Parser* p)
{
    Token save_cur = p->cur; size_t save_off = p->off; Token save_next = p->next; Bool save_has = p->has_next;
    advance(p);   // '{'
    u64 count = 0;
    if (p->cur.tag != TOKEN_RBRACE) {
        int depth = 0;
        Bool saw = 0;   // a real token has appeared since the last depth-0 comma
        for (;;) {
            int t = p->cur.tag;
            if (t == TOKEN_EOF) break;
            if (t == TOKEN_RBRACE && depth == 0) { if (saw) count++; break; }
            if (t == TOKEN_LBRACE || t == TOKEN_LPAREN || t == TOKEN_LBRACKET) { depth++; saw = 1; }
            else if (t == TOKEN_RBRACE || t == TOKEN_RPAREN || t == TOKEN_RBRACKET) { depth--; saw = 1; }
            else if (t == TOKEN_COMMA && depth == 0) { count++; saw = 0; }
            else saw = 1;
            advance(p);
        }
    }
    p->cur = save_cur; p->off = save_off; p->next = save_next; p->has_next = save_has;
    return count;
}

// Parse a static-storage definition of type *tyref: *out_init is its materialized image (NULL when
// there is no '=' initializer, i.e. zero-initialized), *out_len is the .data slot size, and
// *out_relocs/*out_reloc_count are the function-address fixups found in the image (NULL/0 if none).
// A flexible array (`T x[] = ...`) is completed in place: *tyref is rewritten with the deduced length.
static Bool parse_static_definition(Parser* p, Type* tyref, const u8** out_init, u32* out_len,
                                    const StaticReloc** out_relocs, u32* out_reloc_count)
{
    Type ty = *tyref;
    *out_init = 0;
    *out_relocs = 0;
    *out_reloc_count = 0;
    if (p->cur.tag != TOKEN_ASSIGN) { *out_len = type_size(ty); return 1; }
    advance(p);
    if (type_is_array(ty) && ty->count == 0) {   // `T x[] = ...`: take the outer dimension from the initializer
        u64 n;
        if (type_is_char(referenced(ty)) && p->cur.tag == TOKEN_STRING) {
            Token sc = p->cur; size_t so = p->off; Token sn = p->next; Bool sh = p->has_next;
            u32 len; decode_string_run(p, &len);                       // count includes the null terminator
            p->cur = sc; p->off = so; p->next = sn; p->has_next = sh;
            n = (u64)len + 1;
        } else if (p->cur.tag == TOKEN_LBRACE) {
            n = count_init_elements(p);
        } else { fail(p, "invalid initializer for array"); return 0; }
        if (n == 0) { fail(p, "array has size zero"); return 0; }
        ty = array_of(referenced(ty), n);
        *tyref = ty;
    }
    u32 sz = type_size(ty);
    *out_len = sz;
    u8* buf = ARENA_ALLOC(&global_arena, sz ? sz : 1);
    for (u32 k = 0; k < sz; k++) buf[k] = 0;
    p->static_reloc_count = 0;
    if (!static_init_image(p, ty, buf, buf)) return 0;
    *out_init = buf;
    if (p->static_reloc_count) {   // copy the scratch fixups into a stable arena array for this static
        StaticReloc* arr = ARENA_ALLOC(&global_arena, p->static_reloc_count * sizeof(StaticReloc));
        for (size_t i = 0; i < p->static_reloc_count; i++) arr[i] = SEG_LIST_VAL(StaticReloc, &p->static_relocs, i);
        *out_relocs = arr;
        *out_reloc_count = (u32)p->static_reloc_count;
    }
    return 1;
}

// <declaration> ::= <type> <id> [ "=" <exp> ] ";"          (a variable)
//                 | <type> <id> "(" <params> ")" ";"       (a local function declaration)
// `allow_func_decl` is false in a for-loop header, where only a variable declaration is legal.
static void zero_fill(Parser* p, IrVal addr, Type ty)
{
    if (type_is_array(ty)) {
        Type elem = referenced(ty); u32 esz = type_size(elem);
        for (u64 i = 0; i < ty->count; i++)
            zero_fill(p, addr_plus(p, addr, i * esz), elem);
    } else if (type_is_struct(ty)) {
        const StructDef* sd = ty->sdef;
        u32 n = sd->is_union ? 1 : sd->member_count;   // a union zeroes only its first member
        for (u32 i = 0; i < n; i++)
            zero_fill(p, addr_plus(p, addr, sd->members[i].offset), sd->members[i].type);
    } else {
        IrInstr in; in.kind = IR_STORE; in.a = addr; in.u.b = ir_constant_sized(0, type_size(ty)); ir_emit(p->fn, in);
    }
}

// store an initializer for `ty` at pointer `addr`: a braced list for arrays (zero-filling the tail),
// or an expression for a scalar
static void parse_initializer(Parser* p, IrVal addr, Type ty)
{
    if (type_is_struct(ty)) {
        if (p->cur.tag != TOKEN_LBRACE) {                 // `= expr` : copy a whole struct value
            ExpResult v = parse_exp(p);
            if (p->failed) return;
            if (!same_type(v.type, ty)) { fail(p, "incompatible struct initializer"); return; }
            emit_block_copy(p, addr, v.val, type_size(ty));
            return;
        }
        const StructDef* sd = ty->sdef;
        u32 n = sd->is_union ? 1 : sd->member_count;      // a union initializes only its first member
        advance(p);   // {
        u32 i = 0;
        if (p->cur.tag != TOKEN_RBRACE) for (;;) {
            if (i >= n) { fail(p, "too many initializers"); return; }
            parse_initializer(p, addr_plus(p, addr, sd->members[i].offset), sd->members[i].type);
            if (p->failed) return;
            i++;
            if (p->cur.tag != TOKEN_COMMA) break;
            advance(p);
            if (p->cur.tag == TOKEN_RBRACE) break;        // trailing comma
        }
        if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected '}'"); return; }
        advance(p);
        for (; i < n; i++)                                // zero-fill unspecified members
            zero_fill(p, addr_plus(p, addr, sd->members[i].offset), sd->members[i].type);
        return;
    }
    if (type_is_array(ty)) {
        Type elem = referenced(ty); u32 esz = type_size(elem);
        if (type_is_char(elem) && p->cur.tag == TOKEN_STRING) {   // char array initialized by a string literal
            u32 len; const u8* bytes = decode_string_run(p, &len);
            if (len > ty->count) { fail(p, "string initializer is too long"); return; }
            u32 ncopy = (len + 1 <= ty->count) ? len + 1 : (u32)ty->count;   // include the null only if it fits
            for (u64 k = 0; k < ty->count; k++) {
                u32 a = ir_new_ptr_temp(p->fn);
                emit_binary(p, IR_ADD, addr, ir_constant_sized(k, 8), a, 1);
                u8 byte = k < ncopy ? bytes[k] : 0;              // copied char or zero-filled tail
                IrInstr in; in.kind = IR_STORE; in.a = ir_var(a); in.u.b = ir_constant_sized(byte, 1); ir_emit(p->fn, in);
            }
            return;
        }
        if (p->cur.tag != TOKEN_LBRACE) { fail(p, "array requires a braced initializer"); return; }
        advance(p);
        if (p->cur.tag == TOKEN_RBRACE) { fail(p, "empty initializer list"); return; }
        u64 i = 0;
        for (;;) {
            if (i >= ty->count) { fail(p, "too many array initializers"); return; }
            u32 a = ir_new_ptr_temp(p->fn);
            emit_binary(p, IR_ADD, addr, ir_constant_sized(i * esz, 8), a, 1);
            parse_initializer(p, ir_var(a), elem);
            if (p->failed) return;
            i++;
            if (p->cur.tag != TOKEN_COMMA) break;
            advance(p);
            if (p->cur.tag == TOKEN_RBRACE) break;       // trailing comma
        }
        if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected '}'"); return; }
        advance(p);
        for (; i < ty->count; i++) {                          // zero-fill the unspecified tail
            u32 a = ir_new_ptr_temp(p->fn);
            emit_binary(p, IR_ADD, addr, ir_constant_sized(i * esz, 8), a, 1);
            zero_fill(p, ir_var(a), elem);
        }
    } else {
        ExpResult v = parse_exp(p);
        if (p->failed) return;
        v = convert_to(p, v, ty);
        IrInstr in; in.kind = IR_STORE; in.a = addr; in.u.b = v.val; ir_emit(p->fn, in);
    }
}

static void parse_declaration(Parser* p, Bool allow_func_decl)
{
    StorageClass sc;
    Type base;
    if (!parse_specifiers(p, &sc, &base)) return;
    if (p->cur.tag == TOKEN_SEMICOLON) { advance(p); return; }   // a type-only declaration (e.g. `struct s { ... };`)

    // a comma-separated list of declarators sharing the storage class and base type (`int *p, q;`);
    // each declarator derives its own type from `base`. The `;` is consumed once, after the list.
    for (;;) {
        size_t name_off, name_len; Bool is_func = 0;
        Type ty = parse_declarator(p, base, &name_off, &name_len, &is_func);
        if (p->failed) return;
        if (name_len == 0) { fail(p, "expected a name"); return; }

        if (sc == SC_TYPEDEF) {
            // `typedef <type> <name>` aliases <name> (a function declarator would make a function
            // *type*, which cc doesn't model -- reject it).
            if (is_func) { fail(p, "typedef of a function type is unsupported"); return; }
            typedef_add(p, name_off, name_len, ty);
        } else if (is_func) {
            // a function declaration at block scope (functions have external linkage; `static` is illegal)
            if (!allow_func_decl) { fail(p, "function declaration not allowed in 'for' loop header"); return; }
            if (sc == SC_STATIC) { fail(p, "block-scope function declaration cannot be static"); return; }
            const Var* clash = scope_lookup(p, name_off, name_len);
            if (clash && !clash->is_func) { fail(p, "'int' variable redeclared as a function"); return; }
            func_declare(p, name_off, name_len, p->params_unspecified ? PARAM_COUNT_UNSPECIFIED : (u32)p->params.count, sc, 0, ty, &p->params);
            if (p->failed) return;
            if (!clash) scope_add_func(p, name_off, name_len);   // occupy the scope (redeclaration is fine)
            if (p->cur.tag == TOKEN_LBRACE) { fail(p, "nested function definition"); return; }
        } else if (type_is_incomplete(ty)) {
            fail(p, "variable has incomplete type"); return;
        } else if (sc == SC_EXTERN) {
            // block-scope `extern int a;` binds the name to the file-scope static. It may not redeclare
            // an identifier already declared in this scope with no linkage (a local or local static).
            const Var* clash = scope_lookup(p, name_off, name_len);
            if (clash && !(clash->is_static && !clash->no_linkage)) {
                fail(p, "declaration of identifier with no linkage follows a declaration with linkage"); return;
            }
            u32 gid; GlobalKind gk;
            if (global_find(p, name_off, name_len, &gid, &gk) && gk == GLOBAL_STATIC) {
                if (!same_type(SEG_LIST_REF(Global, &p->funcs, gid)->type, ty)) { fail(p, "conflicting types for variable"); return; }
            } else {
                gid = global_var_declare(p, name_off, name_len, SC_EXTERN, 0, type_size(ty), 0, 0, 0, ty);
            }
            scope_add_static(p, name_off, name_len, gid, 0);
        } else if (sc == SC_STATIC) {
            // a block-scope static: static storage, no linkage. Its initializer must be a constant.
            if (scope_lookup(p, name_off, name_len)) { fail(p, "variable redeclared"); return; }
            const u8* init; u32 init_len; const StaticReloc* relocs; u32 reloc_count;
            if (!parse_static_definition(p, &ty, &init, &init_len, &relocs, &reloc_count)) return;
            u32 sid = local_static_declare(p, name_off, name_len, init, init_len, relocs, reloc_count, ty);
            scope_add_static(p, name_off, name_len, sid, 1);
        } else {
            // an automatic variable
            if (var_in_current_scope(p, name_off, name_len)) { fail(p, "variable redeclared"); return; }
            u8 reg_binding = parse_reg_binding(p);   // optional GCC local register variable: `__asm__("r10")`
            if (p->failed) return;
            Bool has_init = (p->cur.tag == TOKEN_ASSIGN);
            if (has_init) advance(p);
            // `T x[] = ...`: deduce the outer dimension from the initializer before sizing the slot
            // (mirrors parse_static_definition).
            if (has_init && type_is_array(ty) && ty->count == 0) {
                u64 n;
                if (type_is_char(referenced(ty)) && p->cur.tag == TOKEN_STRING) {
                    Token sc = p->cur; size_t so = p->off; Token sn = p->next; Bool sh = p->has_next;
                    u32 len; decode_string_run(p, &len);                       // count includes the null terminator
                    p->cur = sc; p->off = so; p->next = sn; p->has_next = sh;
                    n = (u64)len + 1;
                } else if (p->cur.tag == TOKEN_LBRACE) {
                    n = count_init_elements(p);
                } else { fail(p, "invalid initializer for array"); return; }
                if (n == 0) { fail(p, "array has size zero"); return; }
                ty = array_of(referenced(ty), n);
            }
            u32 temp = var_add(p, name_off, name_len, ty, 0);   // in scope within its own initializer
            if (reg_binding != 0xFF) SEG_LIST_REF(Var, &p->vars, p->var_count - 1)->asm_reg = reg_binding;
            if (has_init) {
                if (type_is_aggregate(ty)) {
                    u32 a = ir_new_ptr_temp(p->fn);
                    IrInstr ad; ad.kind = IR_ADDROF; ad.a = ir_var(temp); ad.dst = a; ir_emit(p->fn, ad);
                    parse_initializer(p, ir_var(a), ty);
                    if (p->failed) return;
                } else {
                    ExpResult init = parse_exp(p);
                    if (p->failed) return;
                    init = convert_to(p, init, ty);          // convert the initializer to the variable's type
                    emit_copy(p, init.val, temp);
                }
            }
        }

        if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
        break;
    }
    if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
    advance(p);
}

// "while" "(" <exp> ")" <statement>
static void parse_while(Parser* p)
{
    advance(p);   // 'while'
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '('"); return; }
    advance(p);

    u32 cont = ir_new_label(p->fn);
    u32 brk = ir_new_label(p->fn);
    emit_label(p, cont);                 // continue re-tests the condition
    ExpResult cond = parse_full_exp(p);
    if (p->failed) return;
    if (type_is_void(cond.type) || type_is_struct(cond.type)) { fail(p, "controlling expression must be scalar"); return; }
    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return; }
    advance(p);
    emit_jump_if_zero(p, cond.val, brk);

    break_push(p, brk);
    continue_push(p, cont);
    parse_statement(p);
    continue_pop(p);
    break_pop(p);
    if (p->failed) return;
    emit_jump(p, cont);
    emit_label(p, brk);
}

// "do" <statement> "while" "(" <exp> ")" ";"
static void parse_do(Parser* p)
{
    advance(p);   // 'do'
    u32 start = ir_new_label(p->fn);
    u32 cont = ir_new_label(p->fn);
    u32 brk = ir_new_label(p->fn);

    emit_label(p, start);
    break_push(p, brk);
    continue_push(p, cont);
    parse_statement(p);                  // body
    continue_pop(p);
    break_pop(p);
    if (p->failed) return;

    if (p->cur.tag != TOKEN_KW_WHILE) { fail(p, "expected 'while'"); return; }
    advance(p);
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '('"); return; }
    advance(p);
    emit_label(p, cont);                 // continue re-tests the condition
    ExpResult cond = parse_full_exp(p);
    if (p->failed) return;
    if (type_is_void(cond.type) || type_is_struct(cond.type)) { fail(p, "controlling expression must be scalar"); return; }
    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return; }
    advance(p);
    emit_jump_if_not_zero(p, cond.val, start);
    emit_label(p, brk);
    if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
    advance(p);
}

// "for" "(" <init> ";" <exp-opt> ";" <exp-opt> ")" <statement>
//   <init> ::= <declaration> | <exp> ";" | ";"
// The post-expression sits in the header but runs after the body, so the labels are arranged
// so emission still follows source order: cond, then post, then body.
static void parse_for(Parser* p)
{
    advance(p);   // 'for'
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '('"); return; }
    advance(p);

    // a for-header declaration is scoped to the loop
    size_t outer_scope_start = p->scope_start;
    size_t outer_var_count = p->var_count;
    size_t outer_struct_scope = p->struct_scope_start;
    size_t outer_typedef_count = p->typedef_count;
    size_t outer_enum_const_count = p->enum_const_count;
    p->scope_start = p->var_count;
    p->struct_scope_start = p->struct_def_count;

    u32 top = ir_new_label(p->fn);          // condition test
    u32 cont = ir_new_label(p->fn);         // continue target: the post-expression
    u32 body_label = ir_new_label(p->fn);
    u32 brk = ir_new_label(p->fn);

    do {
        // init clause
        if (token_starts_type(p, p->cur)) {
            parse_declaration(p, 0);                     // consumes through its ';'  (no function decls here)
        } else if (p->cur.tag == TOKEN_SEMICOLON) {
            advance(p);
        } else {
            parse_full_exp(p);
            if (p->failed) break;
            if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); break; }
            advance(p);
        }
        if (p->failed) break;

        emit_label(p, top);
        if (p->cur.tag != TOKEN_SEMICOLON) {            // condition (optional)
            ExpResult cond = parse_full_exp(p);
            if (p->failed) break;
            if (type_is_void(cond.type) || type_is_struct(cond.type)) { fail(p, "controlling expression must be scalar"); break; }
            emit_jump_if_zero(p, cond.val, brk);
        }
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); break; }
        advance(p);

        emit_jump(p, body_label);                       // skip the post on the way in
        emit_label(p, cont);
        if (p->cur.tag != TOKEN_RPAREN) {               // post-expression (optional)
            parse_full_exp(p);
            if (p->failed) break;
        }
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); break; }
        advance(p);
        emit_jump(p, top);

        emit_label(p, body_label);
        break_push(p, brk);
        continue_push(p, cont);
        parse_statement(p);                             // body
        continue_pop(p);
        break_pop(p);
        if (p->failed) break;
        emit_jump(p, cont);
        emit_label(p, brk);
    } while (0);

    p->var_count = outer_var_count;
    p->scope_start = outer_scope_start;
    p->typedef_count = outer_typedef_count;
    p->enum_const_count = outer_enum_const_count;
    pop_struct_scope(p, outer_struct_scope);
}

// register a case value in the current switch (reporting duplicates); returns its IR label
static u32 switch_add_case(Parser* p, u32 value)
{
    SwitchCtx* s = p->sw;
    for (size_t i = 0; i < s->case_count; i++) {
        SwitchCase c = SEG_LIST_VAL(SwitchCase, &s->cases, i);
        if (c.value == value) { fail(p, "duplicate case value"); return c.label; }
    }
    u32 label = ir_new_label(p->fn);
    SEG_LIST_APPEND(SwitchCase, &s->cases, ((SwitchCase){ value, label }));
    s->case_count += 1;
    return label;
}

// "switch" "(" <exp> ")" <statement>
// The body is emitted first (with case/default labels inline, so fallthrough and even
// duff's device just work); the compare-and-jump dispatch is emitted after it and jumps in.
static void parse_switch(Parser* p)
{
    advance(p);   // 'switch'
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '('"); return; }
    advance(p);
    ExpResult ctrl = parse_full_exp(p);
    if (p->failed) return;
    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return; }
    advance(p);
    ctrl = promote(p, ctrl);               // integer-promote the controlling expression (char/short -> int)

    u32 tval = ir_new_temp(p->fn);
    emit_copy(p, ctrl.val, tval);          // a stable copy to compare against in the dispatch

    u32 dispatch = ir_new_label(p->fn);
    u32 brk = ir_new_label(p->fn);
    emit_jump(p, dispatch);                // enter via the dispatch, which jumps into the body

    SwitchCtx ctx;
    ctx.cases = SEG_LIST_INIT(SwitchCase);
    ctx.case_count = 0;
    ctx.has_default = 0;
    ctx.default_label = 0;
    SwitchCtx* outer = p->sw;
    p->sw = &ctx;
    break_push(p, brk);                    // a switch is a break target, but not a continue target

    parse_statement(p);                    // body (case/default labels are emitted inline)

    break_pop(p);
    p->sw = outer;
    if (p->failed) return;

    emit_jump(p, brk);                     // falling off the end of the switch exits it
    emit_label(p, dispatch);
    for (size_t i = 0; i < ctx.case_count; i++) {
        SwitchCase c = SEG_LIST_VAL(SwitchCase, &ctx.cases, i);
        u32 cmp = ir_new_temp(p->fn);
        emit_binary(p, IR_EQUAL, ir_var(tval), ir_constant(c.value), cmp, 0);
        emit_jump_if_not_zero(p, ir_var(cmp), c.label);
    }
    emit_jump(p, ctx.has_default ? ctx.default_label : brk);
    emit_label(p, brk);
    seg_list_deinit(&ctx.cases);
}

// the inner content of a TOKEN_STRING (between the quotes), with its length
static const u8* asm_str(const Parser* p, Token t, size_t* len)
{
    size_t n = t.limit - t.start;
    *len = n >= 2 ? n - 2 : 0;
    return &p->src[t.start + 1];
}

static Bool asm_span_is(const u8* s, size_t len, const char* lit)
{
    for (size_t k = 0; k < len; k++) { if (lit[k] == 0 || s[k] != (u8)lit[k]) return 0; }
    return lit[len] == 0;
}

// if [*c, end) begins with the literal `lit`, advance *c past it and return 1; else leave *c and return 0
static Bool asm_eat(const u8** c, const u8* end, const char* lit)
{
    const u8* q = *c;
    for (size_t i = 0; lit[i]; i++) { if (q >= end || *q != (u8)lit[i]) return 0; q++; }
    *c = q;
    return 1;
}
static void asm_skip_ws(const u8** c, const u8* end) { while (*c < end && (**c == ' ' || **c == '\t')) (*c)++; }

// map an x86-64 register name (as in a local register variable's `__asm__("r10")`) to its number;
// 0xFF for anything unrecognized
#define REG(num) (s[0] == 'r' ? (u8)(num) : 0xFF)   // the switch fixes the rest; just confirm the 'r' prefix
static u8 asm_regname(const u8* s, size_t len)
{
    switch (len) {
    case 2:
        switch (s[1]) {
        case '8': return REG(8);
        case '9': return REG(9);
        }
        return 0xFF;
    case 3:
        switch (s[1]) {
        case 'a': return REG(0);   // rax
        case 'c': return REG(1);   // rcx
        case 'b': return REG(3);   // rbx
        case 's': return REG(6);   // rsi
        case 'd':
            switch (s[2]) {
            case 'x': return REG(2);   // rdx
            case 'i': return REG(7);   // rdi
            }
            return 0xFF;
        case '1':
            switch (s[2]) {
            case '0': return REG(10);
            case '1': return REG(11);
            case '2': return REG(12);
            case '3': return REG(13);
            case '4': return REG(14);
            case '5': return REG(15);
            }
            return 0xFF;
        }
        return 0xFF;
    }
    return 0xFF;
}
#undef REG

// map a GCC register constraint (e.g. "a", "=a", "D") to an x86-64 register number; sets *is_out
// for a leading '='. Returns 0xFF for anything we don't support yet.
static u8 asm_constraint_reg(const u8* s, size_t len, Bool* is_out)
{
    size_t i = 0;
    *is_out = 0;
    if (len && s[0] == '=') { *is_out = 1; i = 1; }
    if (len - i != 1) return 0xFF;
    switch (s[i]) {
        case 'a': return 0;   // rax
        case 'b': return 3;   // rbx
        case 'c': return 1;   // rcx
        case 'd': return 2;   // rdx
        case 'S': return 6;   // rsi
        case 'D': return 7;   // rdi
        default:  return 0xFF;
    }
}

// the minimal assembler: append the machine code for the asm template `s` to fn's byte pool. Sets
// *start to the first byte's pool index, returns the byte count, and sets *op0_reg to the register the
// template's `%0` operand binds to (0xFF if the template has no `%0`). Fail loud on anything unsupported.
//
// This is deliberately a hand-recognized handful of exact instruction forms, NOT a general assembler:
// cc emits the template's bytes here, BEFORE parse_asm has chosen registers for the operands, so a
// template can only reference operands whose register this function fixes itself (e.g. `%0` -> rax in
// the gs-read below). To support general parameterized asm (arbitrary mnemonics, any `%N`, caller-
// chosen registers), this would need to become a real encoder: parse operands first, let parse_asm
// assign each `%N` a register, then build ModRM/SIB from those choices. Until that's needed we add one
// recognized form at a time and reject everything else.
static u32 asm_assemble(Parser* p, const u8* s, size_t len, u32* start, u8* op0_reg)
{
    *op0_reg = 0xFF;
    while (len && (s[0] == ' ' || s[0] == '\t')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;

    if (asm_span_is(s, len, "syscall")) {
        *start = ir_add_asm_byte(p->fn, 0x0F);
        ir_add_asm_byte(p->fn, 0x05);
        return 2;
    }

    // `movq %%gs:0xNN, %0` -- a gs-relative 64-bit load into operand %0 (peb() reads the PEB at
    // gs:0x60). Recognize ONLY this exact shape, parse the displacement, and bind %0 to rax; anything
    // that deviates falls through to the fail-loud below rather than being silently mis-assembled.
    const u8* c = s;
    const u8* end = s + len;
    if (asm_eat(&c, end, "movq")) {
        asm_skip_ws(&c, end);
        if (asm_eat(&c, end, "%%gs:0x")) {
            u32 disp = 0; int ndig = 0;
            for (; c < end; c++, ndig++) {
                u8 h = *c; u32 v;
                if (h >= '0' && h <= '9')      v = (u32)(h - '0');
                else if (h >= 'a' && h <= 'f') v = (u32)(10 + h - 'a');
                else if (h >= 'A' && h <= 'F') v = (u32)(10 + h - 'A');
                else break;
                disp = disp * 16 + v;
            }
            asm_skip_ws(&c, end);
            Bool ok = ndig >= 1 && ndig <= 8 && asm_eat(&c, end, ",");
            if (ok) { asm_skip_ws(&c, end); ok = asm_eat(&c, end, "%0"); asm_skip_ws(&c, end); ok = ok && c == end; }
            if (ok) {
                // mov rax, qword ptr gs:[disp32]  ==  65 48 8B 04 25 <disp32-le>
                *start = ir_add_asm_byte(p->fn, 0x65);
                ir_add_asm_byte(p->fn, 0x48);
                ir_add_asm_byte(p->fn, 0x8B);
                ir_add_asm_byte(p->fn, 0x04);
                ir_add_asm_byte(p->fn, 0x25);
                ir_add_asm_byte(p->fn, (u8)(disp));
                ir_add_asm_byte(p->fn, (u8)(disp >> 8));
                ir_add_asm_byte(p->fn, (u8)(disp >> 16));
                ir_add_asm_byte(p->fn, (u8)(disp >> 24));
                *op0_reg = 0;   // %0 -> rax
                return 9;
            }
        }
    }

    fail(p, "unsupported inline-asm instruction");
    return 0;
}

// parse an optional GCC local-register-variable binding -- `__asm__("r10")` after a declarator's name,
// which pins the variable to that register -- and return its register number (0xFF if there is none).
static u8 parse_reg_binding(Parser* p)
{
    if (p->cur.tag != TOKEN_KW_ASM) return 0xFF;
    advance(p);
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '(' after __asm__ in a declarator"); return 0xFF; }
    advance(p);
    if (p->cur.tag != TOKEN_STRING) { fail(p, "expected a register-name string"); return 0xFF; }
    size_t rlen; const u8* rname = asm_str(p, p->cur, &rlen);
    u8 reg = asm_regname(rname, rlen);
    if (reg == 0xFF) { fail(p, "unknown register in __asm__ binding"); return 0xFF; }
    advance(p);
    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' after the __asm__ register"); return 0xFF; }
    advance(p);
    return reg;
}

// __asm__ [volatile] ( <template> [ : <outputs> [ : <inputs> [ : <clobbers> ] ] ] ) ;
// Each output/input operand is "<constraint>" ( <expr> ); clobbers are bare strings we ignore.
static void parse_asm(Parser* p)
{
    advance(p);                                  // __asm__
    if (p->cur.tag == TOKEN_KW_VOLATILE) advance(p);   // optional qualifier
    if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '(' after __asm__"); return; }
    advance(p);

    if (p->cur.tag != TOKEN_STRING) { fail(p, "expected an asm template string"); return; }
    size_t tlen; const u8* tmpl = asm_str(p, p->cur, &tlen);
    u32 byte_start; u8 op0_reg; u32 byte_count = asm_assemble(p, tmpl, tlen, &byte_start, &op0_reg);
    if (p->failed) return;
    advance(p);

    u32 op_start = (u32)p->fn->asm_op_count;
    u32 op_count = 0;
    u32 section = 0;                             // 1=outputs, 2=inputs, 3=clobbers
    while (p->cur.tag == TOKEN_COLON) {
        advance(p);
        section++;
        while (p->cur.tag == TOKEN_STRING) {
            if (section >= 3) { advance(p); if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; } break; }

            size_t clen; const u8* cstr = asm_str(p, p->cur, &clen);
            // a generic "r"/"=r" constraint takes its register from the operand's local register variable
            // (its `__asm__("...")` binding); otherwise the constraint letter names the register.
            Bool is_out = clen > 0 && cstr[0] == '=';
            Bool is_r = (clen == 1 && cstr[0] == 'r') || (clen == 2 && cstr[0] == '=' && cstr[1] == 'r');
            u8 reg = 0;
            if (!is_r) {
                reg = asm_constraint_reg(cstr, clen, &is_out);
                if (reg == 0xFF) { fail(p, "unsupported asm constraint"); return; }
            }
            advance(p);
            if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '(' in asm operand"); return; }
            advance(p);
            ExpResult e;
            if (is_r) {
                if (p->cur.tag != TOKEN_IDENTIFIER) { fail(p, "an \"r\" asm operand must be a variable"); return; }
                const Var* rv = scope_lookup(p, p->cur.start, p->cur.limit - p->cur.start);
                if (!rv) { fail(p, "undeclared \"r\" asm operand"); return; }
                if (rv->asm_reg != 0xFF) {
                    reg = rv->asm_reg;          // a local register variable (e.g. syscall6's r10) names the reg
                } else if (op_count == 0 && op0_reg != 0xFF) {
                    reg = op0_reg;              // a plain "r"/"=r": the register is fixed by the template's %0
                } else {
                    fail(p, "an \"r\" asm operand must be register-bound or the template's %0"); return;
                }
                e = res_lvalue(rv->temp, rv->type);
                advance(p);
            } else {
                e = parse_exp(p);
                if (p->failed) return;
            }
            if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' in asm operand"); return; }
            advance(p);

            AsmOperand op; op.reg = reg; op.is_out = is_out;
            if (is_out) {
                if (!e.is_lvalue || e.is_static) { fail(p, "asm output must be a local variable"); return; }
                op.temp = e.lvalue_temp; op.val = ir_constant(0);
            } else {
                op.val = e.val; op.temp = 0;
            }
            ir_add_asm_op(p->fn, op);
            op_count++;

            if (p->cur.tag == TOKEN_COMMA) { advance(p); continue; }
            break;
        }
    }

    if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')' to close __asm__"); return; }
    advance(p);
    if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';' after __asm__"); return; }
    advance(p);

    IrInstr in;
    in.kind = IR_ASM;
    in.u.asm_.byte_start = byte_start; in.u.asm_.byte_count = byte_count;
    in.u.asm_.op_start = op_start;     in.u.asm_.op_count = op_count;
    ir_emit(p->fn, in);
}

// <statement> ::= "return" <exp> ";" | "if" "(" <exp> ")" <statement> [ "else" <statement> ]
//               | "while"/"do"/"for" loops | "switch" | "case"/"default" | "break"/"continue"
//               | "goto" <label> ";" | <label> ":" <statement> | <exp> ";" | ";" | __asm__ (...)
static void parse_statement(Parser* p)
{
    switch (p->cur.tag) {
    case TOKEN_KW_ASM:
        parse_asm(p);
        return;
    case TOKEN_KW_RETURN: {
        advance(p);
        if (p->cur.tag == TOKEN_SEMICOLON) {              // `return;` -- only valid in a void function
            if (!type_is_void(p->ret_type)) { fail(p, "non-void function must return a value"); return; }
            IrInstr ret; ret.kind = IR_RETURN; ret.a = ir_constant(0); ir_emit(p->fn, ret);
            advance(p);
            return;
        }
        ExpResult r = parse_full_exp(p);
        if (p->failed) return;
        if (type_is_void(p->ret_type)) { fail(p, "void function cannot return a value"); return; }
        r = convert_to(p, r, p->ret_type);   // convert to the function's return type
        if (type_is_aggregate(p->ret_type)) {
            // sret: copy the struct into the caller's result slot (the hidden pointer param, temp 0)
            // and return that pointer in the return register
            emit_block_copy(p, ir_var(0), r.val, type_size(p->ret_type));
            IrInstr ret; ret.kind = IR_RETURN; ret.a = ir_var(0); ir_emit(p->fn, ret);
        } else {
            IrInstr ret; ret.kind = IR_RETURN; ret.a = r.val; ir_emit(p->fn, ret);
        }
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
        advance(p);
        return;
    }
    case TOKEN_KW_WHILE:  parse_while(p);  return;
    case TOKEN_KW_DO:     parse_do(p);     return;
    case TOKEN_KW_FOR:    parse_for(p);    return;
    case TOKEN_KW_SWITCH: parse_switch(p); return;
    case TOKEN_KW_BREAK: {
        advance(p);
        u32 target;
        if (!break_current(p, &target)) { fail(p, "'break' outside of a loop or switch"); return; }
        emit_jump(p, target);
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
        advance(p);
        return;
    }
    case TOKEN_KW_CONTINUE: {
        advance(p);
        u32 target;
        if (!continue_current(p, &target)) { fail(p, "'continue' outside of a loop"); return; }
        emit_jump(p, target);
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
        advance(p);
        return;
    }
    case TOKEN_KW_CASE: {
        advance(p);
        if (p->sw == 0) { fail(p, "'case' outside of a switch"); return; }
        i64 cval;
        if (!parse_const_expr(p, &cval)) return;   // a constant expression (folds `(0x80+0x3b)`, enums, sizeof, ...)
        u32 label = switch_add_case(p, (u64)cval);
        if (p->failed) return;
        if (p->cur.tag != TOKEN_COLON) { fail(p, "expected ':'"); return; }
        advance(p);
        emit_label(p, label);
        parse_statement(p);
        return;
    }
    case TOKEN_KW_DEFAULT: {
        advance(p);
        if (p->sw == 0) { fail(p, "'default' outside of a switch"); return; }
        if (p->sw->has_default) { fail(p, "multiple default labels in switch"); return; }
        u32 label = ir_new_label(p->fn);
        p->sw->default_label = label;
        p->sw->has_default = 1;
        if (p->cur.tag != TOKEN_COLON) { fail(p, "expected ':'"); return; }
        advance(p);
        emit_label(p, label);
        parse_statement(p);
        return;
    }
    case TOKEN_KW_IF: {
        advance(p);
        if (p->cur.tag != TOKEN_LPAREN) { fail(p, "expected '('"); return; }
        advance(p);
        ExpResult cond = parse_full_exp(p);
        if (p->failed) return;
        if (type_is_void(cond.type) || type_is_struct(cond.type)) { fail(p, "controlling expression must be scalar"); return; }
        if (p->cur.tag != TOKEN_RPAREN) { fail(p, "expected ')'"); return; }
        advance(p);
        u32 else_label = ir_new_label(p->fn);
        emit_jump_if_zero(p, cond.val, else_label);
        parse_statement(p);                          // then-branch
        if (p->failed) return;
        if (p->cur.tag == TOKEN_KW_ELSE) {
            advance(p);
            u32 end_label = ir_new_label(p->fn);
            emit_jump(p, end_label);
            emit_label(p, else_label);
            parse_statement(p);                      // else-branch
            if (p->failed) return;
            emit_label(p, end_label);
        } else {
            emit_label(p, else_label);
        }
        return;
    }
    case TOKEN_KW_GOTO: {
        advance(p);
        if (p->cur.tag != TOKEN_IDENTIFIER || is_reserved(p)) { fail(p, "expected a label name"); return; }
        u32 lbl = label_ref(p, p->cur.start, p->cur.limit - p->cur.start);
        advance(p);
        emit_jump(p, lbl);
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
        advance(p);
        return;
    }
    case TOKEN_LBRACE:
        parse_block(p);   // compound statement: its own scope
        return;
    case TOKEN_SEMICOLON:
        advance(p);       // null statement
        return;
    default:
        if (p->cur.tag == TOKEN_IDENTIFIER && !is_reserved(p) && peek(p).tag == TOKEN_COLON) {
            // labeled statement: <label> ":" <statement>
            u32 lbl = label_def(p, p->cur.start, p->cur.limit - p->cur.start);
            advance(p);   // label name
            advance(p);   // ':'
            emit_label(p, lbl);
            parse_statement(p);
            return;
        }
        parse_full_exp(p);     // expression statement; value discarded
        if (p->failed) return;
        if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
        advance(p);
        return;
    }
}

// <block-item> ::= <declaration> | <statement>
static void parse_block_item(Parser* p)
{
    // a declaration starts with a type specifier (incl. a typedef-name) or a storage-class keyword
    if (token_starts_type(p, p->cur) || (token_traits[p->cur.tag] & TOK_STORAGE)) parse_declaration(p, 1);
    else                      parse_statement(p);
}

// <block> ::= "{" { <block-item> } "}"   (introduces a new variable scope)
static void parse_block(Parser* p)
{
    advance(p);   // consume '{'
    size_t outer_scope_start = p->scope_start;
    size_t outer_var_count = p->var_count;
    size_t outer_struct_scope = p->struct_scope_start;
    size_t outer_typedef_count = p->typedef_count;
    size_t outer_enum_const_count = p->enum_const_count;
    p->scope_start = p->var_count;
    p->struct_scope_start = p->struct_def_count;

    while (!p->failed && p->cur.tag != TOKEN_RBRACE && p->cur.tag != TOKEN_EOF) {
        parse_block_item(p);
    }
    if (!p->failed) {
        if (p->cur.tag != TOKEN_RBRACE) fail(p, "expected '}'");
        else advance(p);
    }

    // leave the scope: its names go out of scope (their temps persist as stack slots)
    p->var_count = outer_var_count;
    p->scope_start = outer_scope_start;
    p->typedef_count = outer_typedef_count;
    p->enum_const_count = outer_enum_const_count;
    pop_struct_scope(p, outer_struct_scope);
}

// does the source span [off, off+len) spell exactly `s`?
static Bool name_is(const Parser* p, size_t off, size_t len, const char* s)
{
    for (size_t k = 0; k < len; k++) {
        if (p->src[off + k] != (u8)s[k]) return 0;
    }
    return s[len] == 0;
}

// reset the per-function parser state before a function body
static void begin_function(Parser* p, u32 func_id, u32 param_count)
{
    ir_func_init(&p->cur_func, func_id, &p->program->lists);
    p->cur_func.param_count = param_count;
    p->fn = &p->cur_func;
    p->var_count = 0;
    p->scope_start = 0;
    p->label_count = 0;
    p->break_depth = 0;
    p->continue_depth = 0;
    p->sw = 0;
}

// <declaration> ::= <specifiers> [ <declarator> [ "=" <init> ] { "," <declarator> [ "=" <init> ] } ] ";"
//   A function DEFINITION is the one form that isn't a comma list: <specifiers> <declarator> "{" <body> "}".
static void parse_function(Parser* p)
{
    if (p->cur.tag == TOKEN_SEMICOLON) { advance(p); return; }   // a stray empty declaration (e.g. a trailing `;;`)
    StorageClass sc;
    Type base;
    if (!parse_specifiers(p, &sc, &base)) return;
    if (p->cur.tag == TOKEN_SEMICOLON) { advance(p); return; }   // a type-only declaration (e.g. `struct s { ... };`)
    size_t name_off, name_len; Bool is_func = 0;
    Type ty = parse_declarator(p, base, &name_off, &name_len, &is_func);
    if (p->failed) return;
    if (name_len == 0) { fail(p, "expected a name"); return; }

    // everything except a function definition (a body) is a comma-separated declaration list sharing the
    // base type and storage class: typedefs, file-scope variables, and/or function prototypes.
    if (!(is_func && sc != SC_TYPEDEF && p->cur.tag == TOKEN_LBRACE)) {
        for (;;) {
            if (sc == SC_TYPEDEF) {
                if (is_func) { fail(p, "typedef of a function type is unsupported"); return; }
                typedef_add(p, name_off, name_len, ty);
            } else if (is_func) {
                func_declare(p, name_off, name_len, p->params_unspecified ? PARAM_COUNT_UNSPECIFIED : (u32)p->params.count, sc, 1, ty, &p->params);
                if (p->failed) return;
            } else {
                // a file-scope variable. A bare `extern T x` is a declaration (no slot); any other form is a
                // (possibly tentative) definition. parse_static_definition may complete an array type from its
                // initializer, so it takes &ty.
                Bool is_decl_only = sc == SC_EXTERN && p->cur.tag != TOKEN_ASSIGN;
                if (type_is_incomplete(ty) && !is_decl_only) { fail(p, "variable has incomplete type"); return; }
                const u8* init = 0; u32 init_len = is_decl_only ? 0 : (u32)type_size(ty);
                const StaticReloc* relocs = 0; u32 reloc_count = 0;
                if (!is_decl_only && !parse_static_definition(p, &ty, &init, &init_len, &relocs, &reloc_count)) return;
                global_var_declare(p, name_off, name_len, sc, init, init_len, relocs, reloc_count, 1, ty);
            }
            if (p->cur.tag == TOKEN_COMMA) {   // another declarator from the same base type
                advance(p);
                is_func = 0;
                ty = parse_declarator(p, base, &name_off, &name_len, &is_func);
                if (p->failed) return;
                if (name_len == 0) { fail(p, "expected a name"); return; }
                continue;
            }
            if (p->cur.tag != TOKEN_SEMICOLON) { fail(p, "expected ';'"); return; }
            advance(p);
            return;
        }
    }

    // a function definition: its parameters were parsed by the declarator and are in p->params
    u32 param_n = (u32)p->params.count;
    u32 func_id = func_declare(p, name_off, name_len, p->params_unspecified ? PARAM_COUNT_UNSPECIFIED : param_n, sc, 1, ty, &p->params);
    if (p->failed) return;
    p->ret_type = ty;
    advance(p);   // '{' (its presence was confirmed above)

    func_define(p, func_id);
    if (p->failed) return;

    // a function returning a large struct takes a hidden first pointer parameter (sret): temp 0 is the
    // caller's result slot, so param_count is one more than declared and the real params follow at 1..n.
    Bool sret = type_is_aggregate(ty);
    begin_function(p, func_id, sret ? param_n + 1 : param_n);
    if (sret) var_add(p, 0, 0, pointer_to(ty), 0);   // the hidden sret pointer -> temp 0 (unnamed)

    // the parameters are the next temps and share the body's outermost scope. Read them before the
    // body parses: a nested declaration there would reset p->params (the shared scratch).
    for (size_t i = 0; i < param_n; i++) {
        NameRef pr = SEG_LIST_VAL(NameRef, &p->params, i);
        // an unnamed parameter (len 0) still needs its temp slot, but it isn't a referenceable name,
        // so it can't clash and isn't looked up
        if (pr.len && var_in_current_scope(p, pr.off, pr.len)) { fail(p, "duplicate parameter name"); return; }
        var_add(p, pr.off, pr.len, pr.type, 1);   // aggregate params arrive by reference (see var_add)
    }

    while (!p->failed && p->cur.tag != TOKEN_RBRACE && p->cur.tag != TOKEN_EOF) {
        parse_block_item(p);
    }
    if (p->failed) return;

    for (size_t i = 0; i < p->label_count; i++) {
        if (!SEG_LIST_REF(Label, &p->labels, i)->defined) { fail(p, "use of undeclared label"); return; }
    }

    // implicit `return 0;` so a function that falls off the end still returns cleanly
    IrInstr ret; ret.kind = IR_RETURN; ret.a = ir_constant(0); ir_emit(p->fn, ret);

    if (p->cur.tag != TOKEN_RBRACE) { fail(p, "expected '}'"); return; }
    advance(p);

    tu_add(p->program, p->cur_func);
}

Bool parse(const u8* src, size_t len, TranslationUnit* out, const char** err_msg, size_t* err_off)
{
    Parser p;
    p.src = src;
    p.len = len;
    p.off = 0;
    p.has_next = 0;
    p.program = out;
    tu_init(out, src);
    p.funcs = SEG_LIST_INIT(Global);
    p.global_index = name_map_init();
    p.func_count = 0;
    // a valid scratch context so file-scope const-eval (e.g. a `sizeof` operand in a static
    // initializer or array dimension) can emit-and-rewind before any real function is parsed.
    ir_func_init(&p.cur_func, 0, &out->lists);
    p.fn = &p.cur_func;
    p.vars = SEG_LIST_INIT(Var);
    p.var_count = 0;
    p.scope_start = 0;
    p.labels = SEG_LIST_INIT(Label);
    p.label_count = 0;
    p.break_targets = SEG_LIST_INIT(u32);
    p.break_depth = 0;
    p.continue_targets = SEG_LIST_INIT(u32);
    p.continue_depth = 0;
    p.sw = 0;
    p.param_types = SEG_LIST_INIT(Type);
    p.param_type_count = 0;
    p.struct_defs = SEG_LIST_INIT(StructDef);
    p.struct_def_count = 0;
    p.struct_scope_start = 0;
    p.typedefs = SEG_LIST_INIT(TypedefName);
    p.typedef_count = 0;
    p.enum_consts = SEG_LIST_INIT(EnumConst);
    p.enum_const_count = 0;
    p.static_relocs = SEG_LIST_INIT(StaticReloc);
    p.static_reloc_count = 0;
    p.params = SEG_LIST_INIT(NameRef);
    p.err_msg = 0;
    p.err_off = 0;
    p.failed = 0;
    advance(&p);

    while (!p.failed && p.cur.tag != TOKEN_EOF) {
        parse_function(&p);
    }

    if (!p.failed) {
        // publish the symbol table (one entry per id, in id order)
        for (size_t i = 0; i < p.func_count; i++) {
            const Global* f = SEG_LIST_REF(Global, &p.funcs, i);
            IrGlobal sym;
            sym.name_off = f->name_off;
            sym.name_len = f->name_len;
            sym.kind = f->kind;
            sym.param_count = f->param_count;
            sym.init = f->init;
            sym.init_len = f->init_len;
            sym.relocs = f->relocs;
            sym.reloc_count = f->reloc_count;
            sym.defined = f->defined;
            // a symbol is TU-local (never merged across objects) if it has internal linkage -- a
            // file-scope `static` var/function -- or no linkage at all (a local static).
            sym.internal = f->internal || f->linkage == LINK_INTERNAL;
            tu_add_global(out, sym);
        }
    }

    if (!p.failed) {
        out->main_func_id = (u32)p.func_count;   // == func_id_count: no main in this TU
        for (size_t i = 0; i < p.func_count; i++) {
            const Global* fa = SEG_LIST_REF(Global, &p.funcs, i);
            if (name_is(&p, fa->name_off, fa->name_len, "main")) {
                out->main_func_id = (u32)i;
                break;
            }
        }
        out->func_id_count = (u32)p.func_count;
    }

    seg_list_deinit(&p.params);

    if (p.failed) {
        *err_msg = p.err_msg;
        *err_off = p.err_off;
        return 0;
    }
    return 1;
}
