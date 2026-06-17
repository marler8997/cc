#include "pp.h"

#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "../ur/filesink.h"
#include "../ur/fs.h"
#include "../ur/log.h"
#include "../ur/mem.h"
#include "../ur/pages.h"
#include "../ur/pagesize.h"
#include "../ur/processexit.h"
#include "../ur/seglist.h"
#include "../ur/sink.h"
#include "builtindefines.h"
#include "chartraits.h"
#include "defines.h"
#include "namemap.h"

extern int lookup_include(const u8* s, size_t n);   // index into the arrays below, or -1
extern const char* embed_header_content[];
extern size_t embed_header_content_size[];

typedef struct {
    Pages pages;
    size_t size;
} PpOut;

// ensure `o` has room for `n` more bytes, growing once (doubling, or straight to the needed size if
// that is larger) so a span append never reallocates byte-by-byte
static void pp_reserve(PpOut* o, size_t n)
{
    if (o->size + n > o->pages.size) {
        size_t want = o->size + n;
        size_t grown = o->pages.size ? o->pages.size * 2 : page_size();
        MUST(alloc_pages(&o->pages, want > grown ? want : grown));
    }
}

static void pp_emit(PpOut* o, u8 byte)
{
    pp_reserve(o, 1);
    o->pages.ptr[o->size++] = byte;
}

static void pp_emit_str(PpOut* o, const char* s, size_t n)
{
    pp_reserve(o, n);
    libc_memcpy(o->pages.ptr + o->size, s, n);
    o->size += n;
}

static void pp_emit_u64(PpOut* o, size_t v)
{
    char tmp[20]; size_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) pp_emit(o, (u8)tmp[--n]);
}

// emit a GCC-style linemarker `# <line> "<file>"` so error reporting can map the merged preprocessed
// output back to the original file/line (get_ppd_source_loc scans back to the nearest one).
static void pp_emit_marker(PpOut* o, size_t line, const char* file, size_t file_len)
{
    pp_emit(o, '#'); pp_emit(o, ' ');
    pp_emit_u64(o, line);
    pp_emit(o, ' '); pp_emit(o, '"');
    pp_emit_str(o, file, file_len);
    pp_emit(o, '"'); pp_emit(o, '\n');
}

typedef struct {
    const u8* name;
    size_t name_len;
    const u8* body;
    size_t body_len;
    Bool defined;
    Bool function_like;     // defined as NAME(params) -- '(' adjacent to the name
    const u8* params;       // raw text inside the parens ("num, a1, a2"), empty if none
    size_t params_len;
} Macro;

typedef enum { PP_EOF, PP_ENDIF, PP_ELSE, PP_ELIF } PpStop;

typedef struct {
    const u8* src;
    size_t len;
    Bool stdinc;
    size_t i;
    PpOut out;
    PpOut if_scratch;    // reused buffer for a #if controlling expression (resolve defined/__has_builtin)
    SegList macros;      // growable Macro[]
    size_t macro_count;
    NameMap macro_map;   // macro name -> index into `macros`, so macro_find is O(1) not a linear scan
                         // (cc.c pulls in hundreds of macros via Hedley + headers)
    NameMap include_cache;
    SegList include_files;
    size_t include_count;
    Pages frames;        // reused ExpandFrame[] work stack for iterative macro expansion (see pp_expand);
                         // a contiguous array (direct-indexed by depth) -- this is the hottest pp data
    size_t frame_depth;  // live frame count; lets pp_expand be re-entrant (arg pre-expansion calls it)
    const char* cur_dir;     // directory of the file being processed, for relative #include "..."
    size_t cur_dir_len;
    Bool elif_cond;          // condition of the #elif that just ended a group (read on a PP_ELIF return)
    const char* cur_file;    // file/line being processed, for linemarkers and __FILE__ / __LINE__
    size_t cur_file_len;
    size_t cur_line;
    const Defines* defines;
} Pp;

static Bool is_hspace(u8 c)         { return c == ' ' || c == '\t'; }

static Bool span_eq(const u8* s, size_t n, const char* lit)
{
    for (size_t k = 0; k < n; k++) {
        if (lit[k] == 0 || s[k] != (u8)lit[k]) return 0;
    }
    return lit[n] == 0;
}

static Bool bytes_eq(const u8* a, const u8* b, size_t n)
{
    for (size_t k = 0; k < n; k++) if (a[k] != b[k]) return 0;
    return 1;
}

static Macro* macro_find(Pp* pp, const u8* name, size_t name_len)
{
    u32 idx;
    if (!name_map_get(&pp->macro_map, name, name_len, &idx)) return 0;
    Macro* m = SEG_LIST_REF(Macro, &pp->macros, idx);
    return m->defined ? m : 0;   // an #undef'd macro keeps its slot/map entry but is no longer "defined"
}

// builtin identifiers fall into a few categories: most are object-like "value" defines (replacement
// text from BuiltinDefineValues), but __LINE__/__FILE__ are computed at point of use and __has_builtin
// is a function-like operator (more operators may follow: __has_include, __has_attribute, ...).
typedef enum {
    BUILTIN_CAT_VALUE,      // object-like; text from BuiltinDefineValues (NULL field => not defined for target)
    BUILTIN_CAT_OPERATOR,   // function-like operator: __has_builtin(name) -> 0/1
    BUILTIN_CAT_LINE,       // the current source line number
    BUILTIN_CAT_FILE,       // the current file as a string literal
} BuiltinCategory;

static BuiltinCategory builtin_category(BuiltinDefine d)
{
    switch (d) {
    case BUILTIN_DEFINE_has_builtin: return BUILTIN_CAT_OPERATOR;
    case BUILTIN_DEFINE_line:        return BUILTIN_CAT_LINE;
    case BUILTIN_DEFINE_file:        return BUILTIN_CAT_FILE;
    default:                         return BUILTIN_CAT_VALUE;   // an ordinary value define
    }
}

// is `name` defined for this TU? A non-value builtin (an operator, __LINE__, __FILE__) is always
// provided; a value builtin is defined iff its field is non-NULL for the target; a name that isn't a
// builtin at all falls through to the user (-D) / source macros.
static Bool pp_is_defined(Pp* pp, const u8* name, size_t name_len)
{
    int d = lookup_builtin_define(name, name_len);
    if (d >= 0) {
        if (builtin_category((BuiltinDefine)d) != BUILTIN_CAT_VALUE) return 1;
        return builtin_define_value(pp->defines->builtin, (BuiltinValueDefine)d) != 0;
    }
    return macro_find(pp, name, name_len) != 0;
}

static void macro_define(Pp* pp, const u8* name, size_t name_len, const u8* params, size_t params_len,
                         Bool function_like, const u8* body, size_t body_len)
{
    Macro* m = macro_find(pp, name, name_len);
    if (!m) {
        Macro nm;
        nm.name = name;
        nm.name_len = name_len;
        m = SEG_LIST_APPEND(Macro, &pp->macros, nm);
        name_map_put(&pp->macro_map, name, name_len, (u32)pp->macro_count);   // (redefine-after-undef
        pp->macro_count++;                                                    //  orphans the old slot; harmless)
    }
    m->body = body;
    m->body_len = body_len;
    m->params = params;
    m->params_len = params_len;
    m->function_like = function_like;
    m->defined = 1;
}

// trim leading/trailing horizontal space from [*s, *e)
static void trim(const u8** s, const u8** e)
{
    while (*s < *e && is_hspace(**s)) (*s)++;
    while (*e > *s && is_hspace((*e)[-1])) (*e)--;
}

// if `p` points at a string/char literal, return the position just past its closing quote (handling
// backslash escapes); otherwise return `p` unchanged. Lets argument scanning ignore quoted commas/parens.
static const u8* skip_str(const u8* p, const u8* end)
{
    if (p >= end || (*p != '"' && *p != '\'')) return p;
    u8 quote = *p++;
    while (p < end && *p != quote) { if (*p == '\\' && p + 1 < end) p++; p++; }
    if (p < end) p++;   // consume the closing quote
    return p;
}

// the index of identifier [id, id+len) among a comma-separated list [s, e), or -1 if absent
static int list_index(const u8* s, const u8* e, const u8* id, size_t len)
{
    int idx = 0;
    const u8* item = s;
    for (const u8* p = s; p <= e; p++) {
        if (p == e || *p == ',') {
            const u8* a = item; const u8* z = p; trim(&a, &z);
            if ((size_t)(z - a) == len && bytes_eq(a, id, len)) return idx;
            idx++;
            item = p + 1;
            if (p == e) break;
        }
    }
    return -1;
}

// the n-th comma-separated item of [s, e) (top-level commas only; nested parens are skipped),
// trimmed, via *out/*out_len. Returns 0 if there is no n-th item.
static Bool list_nth(const u8* s, const u8* e, int n, const u8** out, size_t* out_len)
{
    int idx = 0, depth = 0;
    const u8* item = s;
    const u8* p = s;
    for (;;) {
        if (p == e || (*p == ',' && depth == 0)) {
            if (idx == n) {
                const u8* a = item; const u8* z = p; trim(&a, &z);
                *out = a; *out_len = (size_t)(z - a); return 1;
            }
            idx++;
            item = p + 1;
            if (p == e) break;
            p++;
        } else if (*p == '"' || *p == '\'') {
            p = skip_str(p, e);              // a quoted comma/paren is not an arg separator
        } else {
            if (*p == '(') depth++;
            else if (*p == ')' && depth > 0) depth--;
            p++;
        }
    }
    return 0;
}

// One pending expansion: a cursor over a token range plus the macro it stands for (its hide-set
// member; 0 for the root line). `mark` is the arena position before this frame's substituted text,
// rewound when the frame is popped.
typedef struct {
    const u8* pos;
    const u8* end;
    Macro* macro;
    ArenaPosition mark;
} ExpandFrame;

// push a frame onto the contiguous stack, growing the backing pages if needed (which may move them --
// callers must re-fetch the base pointer after any push, which pp_expand does at its loop top)
static void frames_push(Pages* frames, size_t* depth, ExpandFrame fr)
{
    size_t need = (*depth + 1) * sizeof(ExpandFrame);
    if (need > frames->size) {
        size_t grown = frames->size ? frames->size * 2 : page_size();
        MUST(alloc_pages(frames, need > grown ? need : grown));
    }
    ((ExpandFrame*)frames->ptr)[*depth] = fr;
    (*depth)++;
}

static const u8* pp_expand(Pp* pp, const u8* a, const u8* b);   // fwd: macro_substitute pre-expands args

// does [p, end) start (after optional whitespace) with the `##` paste operator?
static Bool next_is_paste(const u8* p, const u8* end)
{
    while (p < end && is_hspace(*p)) p++;
    return p + 1 < end && p[0] == '#' && p[1] == '#';
}

// Substitute function-like macro `m`'s body with the call arguments [args, args_end); the result is
// written to a fresh arena buffer (via *out/*out_len). Implements:
//   `#param`   -> stringize the RAW argument text (C: # operands are not expanded)
//   `a ## b`   -> paste: drop `##` and the surrounding whitespace; ## operands use RAW arg text
//   other param -> substitute the MACRO-EXPANDED argument (C's argument pre-expansion)
// The body is built in pp->out's tail (a growable, stack-disciplined scratch) so the nested expansion
// done by pp_expand_arg composes naturally, then copied to the arena for the caller's frame.
static void macro_substitute(Pp* pp, Macro* m, const u8* args, const u8* args_end, const u8** out, size_t* out_len)
{
    size_t mark = pp->out.size;
    const u8* bs = m->body; const u8* be = m->body + m->body_len;
    Bool after_paste = 0;    // the previous body token was `##`, so the next param is a raw paste operand

    while (bs < be) {
        if (bs + 1 < be && bs[0] == '#' && bs[1] == '#') {     // `##` paste
            while (pp->out.size > mark && is_hspace(pp->out.pages.ptr[pp->out.size - 1])) pp->out.size--;  // trim left ws
            bs += 2;
            while (bs < be && is_hspace(*bs)) bs++;            // trim right ws
            after_paste = 1;
            continue;
        }
        if (bs[0] == '#') {                                    // `#param` stringize (raw arg)
            const u8* q = bs + 1;
            while (q < be && is_hspace(*q)) q++;
            const u8* pid = q;
            while (q < be && is_ident_continue(*q)) q++;
            int pi = (pid < q) ? list_index(m->params, m->params + m->params_len, pid, (size_t)(q - pid)) : -1;
            if (pi < 0) { LOG_STRING("pp: '#' must be followed by a macro parameter"); process_exit(74); }
            const u8* av; size_t al;
            if (!list_nth(args, args_end, pi, &av, &al)) { av = (const u8*)""; al = 0; }
            pp_emit(&pp->out, '"');
            for (size_t k = 0; k < al; k++) {
                if (av[k] == '"' || av[k] == '\\') pp_emit(&pp->out, '\\');   // escape " and \ inside the literal
                pp_emit(&pp->out, av[k]);
            }
            pp_emit(&pp->out, '"');
            bs = q; after_paste = 0;
            continue;
        }
        if (is_ident_start(*bs)) {
            const u8* bid = bs;
            while (bs < be && is_ident_continue(*bs)) bs++;
            size_t blen = (size_t)(bs - bid);
            if (span_eq(bid, blen, "__VA_ARGS__")) {           // variadic args: the call args past the named ones
                int named = list_index(m->params, m->params + m->params_len, (const u8*)"...", 3);
                const u8* va; size_t vl;
                if (named >= 0 && list_nth(args, args_end, named, &va, &vl)) {
                    if (after_paste || next_is_paste(bs, be)) pp_emit_str(&pp->out, (const char*)va, (size_t)(args_end - va));
                    else                                       pp_expand(pp, va, args_end);
                }
                after_paste = 0;
                continue;
            }
            int pi = list_index(m->params, m->params + m->params_len, bid, blen);
            if (pi < 0) { pp_emit_str(&pp->out, (const char*)bid, blen); after_paste = 0; continue; }
            const u8* av; size_t al;
            if (list_nth(args, args_end, pi, &av, &al)) {
                if (after_paste || next_is_paste(bs, be))
                    pp_emit_str(&pp->out, (const char*)av, al);   // a `##` operand: raw, unexpanded
                else
                    pp_expand(pp, av, av + al);                   // ordinary use: pre-expand straight into `sub`
            }
            after_paste = 0;
        } else {
            pp_emit(&pp->out, *bs);
            bs++;
        }
    }

    size_t n = pp->out.size - mark;
    if (n == 0) { *out = 0; *out_len = 0; return; }   // empty substitution: the frame is never dereferenced
    u8* buf = ARENA_ALLOC(&global_arena, n);
    libc_memcpy(buf, pp->out.pages.ptr + mark, n);
    pp->out.size = mark;
    *out = buf; *out_len = n;
}

// Expand macros in [a, b) to the output, iteratively, returning where it stopped: the first newline
// reached at the root level (or b). So a function-like macro invocation whose argument list crosses
// newlines is collected whole -- callers pass b = end of source (not end of line) and resume from the
// returned newline. Each active macro is a frame on an explicit stack, so the hide-set ("a macro can't
// expand itself") is just "is this macro one of the live frames". Function-like macros substitute their
// args into the body in the arena; object-like frames point straight at the macro body. Macro bodies
// have no newlines, so a root-frame newline marks the real end of the logical line (a newline inside a
// sub-frame came from a multi-line argument and is emitted as part of the expansion).
static const u8* pp_expand(Pp* pp, const u8* a, const u8* b)
{
    // re-entrant: frames below `base` belong to an outer (in-progress) pp_expand -- e.g. one whose
    // macro_substitute is pre-expanding an argument by calling us. We only own frames >= base.
    size_t base = pp->frame_depth;
    ArenaPosition entry = arena_position(&global_arena);
    frames_push(&pp->frames, &pp->frame_depth, (ExpandFrame){ a, b, 0, entry });

    while (pp->frame_depth > base) {
        ExpandFrame* frames = (ExpandFrame*)pp->frames.ptr;   // re-fetch: a push below may have moved it
        ExpandFrame* f = &frames[pp->frame_depth - 1];
        if (f->pos >= f->end) {
            if (f->macro) pp_emit(&pp->out, ' ');   // close a macro expansion: a space stops its last token pasting onto the next
            arena_reset(&global_arena, f->mark); pp->frame_depth--; continue;
        }
        if (!is_ident_start(*f->pos)) {
            switch (*f->pos) {
            case '\\':                                   // strip a line continuation; a lone '\' is ordinary
                if (f->pos + 1 < f->end && f->pos[1] == '\n') { f->pos += 2; continue; }
                break;
            case '\n':
                // base == 0 means we're the top-level line driver (pp_block): a root newline ends the
                // logical line, so pop our frame and return it. base > 0 means we're pre-expanding a
                // captured macro argument, where an internal newline is just whitespace -- consume on.
                if (pp->frame_depth == base + 1) {
                    if (base == 0) { arena_reset(&global_arena, entry); pp->frame_depth = base; return f->pos; }
                    pp_emit(&pp->out, ' '); f->pos++; continue;
                }
                break;
            case '"':
            case '\'': {
                // a string/char literal is emitted verbatim -- the preprocessor must not expand macros
                // inside it (e.g. "src/clzll.c" must survive a `#define clzll ...`). A real literal closes
                // on the same line; an unclosed quote is treated as an ordinary byte (don't run past the line).
                u8 quote = *f->pos;
                const u8* q = f->pos + 1;
                while (q < f->end && *q != quote && *q != '\n') { if (*q == '\\' && q + 1 < f->end) q++; q++; }
                if (q < f->end && *q == quote) {
                    q++;
                    pp_emit_str(&pp->out, (const char*)f->pos, (size_t)(q - f->pos)); f->pos = q; continue;
                }
                pp_emit(&pp->out, *f->pos); f->pos++; continue;
            }
            case '/':
                // comments are removed here (C phase 3). A block comment becomes spaces of equal width
                // (newlines kept) so the line AND column of any code following it on the line survive for
                // diagnostics; its content (incl. directive-looking lines) is never seen by pp_block, which
                // already routed this non-'#' line to us. A line comment has nothing after it, so just drop
                // it. A lone '/' (division) is ordinary.
                if (f->pos + 1 < f->end && (f->pos[1] == '/' || f->pos[1] == '*')) {
                    if (f->pos[1] == '/') {
                        f->pos += 2;
                        while (f->pos < f->end && *f->pos != '\n') f->pos++;
                    } else {
                        f->pos += 2; pp_emit(&pp->out, ' '); pp_emit(&pp->out, ' ');   // the "/*"
                        while (f->pos < f->end && !(*f->pos == '*' && f->pos + 1 < f->end && f->pos[1] == '/')) {
                            pp_emit(&pp->out, *f->pos == '\n' ? '\n' : ' ');
                            f->pos++;
                        }
                        if (f->pos < f->end) { f->pos += 2; pp_emit(&pp->out, ' '); pp_emit(&pp->out, ' '); }   // the "*/"
                    }
                    continue;
                }
                break;
            }
            // ordinary byte run: stop at the next identifier, quote, '\\', '/', or '\n' so those re-enter above
            const u8* r = f->pos + 1;
            while (r < f->end && !is_pp_break(*r)) r++;
            pp_emit_str(&pp->out, (const char*)f->pos, (size_t)(r - f->pos)); f->pos = r; continue;
        }

        const u8* id = f->pos;
        const u8* s = id;
        while (s < f->end && is_ident_continue(*s)) s++;
        Macro* m = macro_find(pp, id, (size_t)(s - id));
        Bool busy = 0;
        if (m)                                          // only macros can be on the frame stack; skip the
            for (size_t k = base; k < pp->frame_depth; k++)   // scan for the common non-macro identifier
                if (frames[k].macro == m) { busy = 1; break; }

        if (m && !busy && m->function_like) {
            // a function-like macro expands only if the name is followed by '(' (args may span newlines)
            const u8* look = s;
            while (look < f->end && is_hspace(*look)) look++;
            if (look >= f->end || *look != '(') { pp_emit_str(&pp->out, (const char*)id, (size_t)(s - id)); f->pos = s; continue; }
            look++;                                  // past '('
            const u8* args = look;
            int pdepth = 1;
            while (look < f->end && pdepth > 0) {
                if (*look == '"' || *look == '\'') { look = skip_str(look, f->end); continue; }   // skip quoted ()
                if (*look == '(') pdepth++;
                else if (*look == ')') { pdepth--; if (pdepth == 0) break; }
                look++;
            }
            if (pdepth != 0) { LOG_STRING("pp: unterminated macro argument list"); process_exit(75); }
            const u8* args_end = look;
            f->pos = look + 1;                       // parent consumes the whole invocation

            ArenaPosition mark = arena_position(&global_arena);
            const u8* sub; size_t sub_len;
            macro_substitute(pp, m, args, args_end, &sub, &sub_len);
            pp_emit(&pp->out, ' ');                  // open the expansion: a space stops it pasting onto preceding text
            frames_push(&pp->frames, &pp->frame_depth, (ExpandFrame){ sub, sub + sub_len, m, mark });   // may move pp->frames
        } else if (m && !busy) {
            f->pos = s;                              // parent consumes the macro name
            pp_emit(&pp->out, ' ');                  // open the expansion (the pop above emits the matching close)
            frames_push(&pp->frames, &pp->frame_depth, (ExpandFrame){ m->body, m->body + m->body_len, m, arena_position(&global_arena) });
        } else {
            // a builtin identifier (or, if not a builtin, an ordinary identifier emitted verbatim)
            size_t id_len = (size_t)(s - id);
            int d = lookup_builtin_define(id, id_len);
            const char* v;
            if (d < 0) { pp_emit_str(&pp->out, (const char*)id, id_len); f->pos = s; continue; }
            // a substituted builtin value is bracketed with spaces, exactly like a macro expansion, so it
            // cannot paste onto a neighbour (e.g. `.` `__LINE__` must not re-lex as a floating constant)
            switch (builtin_category((BuiltinDefine)d)) {
            case BUILTIN_CAT_LINE:
                pp_emit(&pp->out, ' '); pp_emit_u64(&pp->out, pp->cur_line); pp_emit(&pp->out, ' ');
                break;
            case BUILTIN_CAT_FILE:
                pp_emit(&pp->out, ' '); pp_emit(&pp->out, '"');
                pp_emit_str(&pp->out, pp->cur_file, pp->cur_file_len);
                pp_emit(&pp->out, '"'); pp_emit(&pp->out, ' ');
                break;
            case BUILTIN_CAT_VALUE:
                v = builtin_define_value(pp->defines->builtin, (BuiltinValueDefine)d);
                if (v) { pp_emit(&pp->out, ' '); pp_emit_str(&pp->out, v, libc_strlen(v)); pp_emit(&pp->out, ' '); }  // defined
                else   pp_emit_str(&pp->out, (const char*)id, id_len);  // not defined -> verbatim identifier
                break;
            case BUILTIN_CAT_OPERATOR:   // only meaningful inside #if; verbatim elsewhere
            default:
                pp_emit_str(&pp->out, (const char*)id, id_len);
                break;
            }
            f->pos = s;
        }
    }
    return b;
}

static const u8* read_ident(const u8* src, size_t* r, size_t line_end, size_t* out_len)
{
    while (*r < line_end && is_hspace(src[*r])) (*r)++;
    const u8* name = &src[*r];
    while (*r < line_end && is_ident_continue(src[*r])) (*r)++;
    *out_len = (size_t)(&src[*r] - name);
    return name;
}

// ---- #if constant-expression evaluator -----------------------------------------
// the compiler builtins cc provides, for `__has_builtin(...)`. Keep in sync with the builtins the
// parser recognizes (e.g. __builtin_clzll). __has_builtin itself is an operator, not a builtin, so
// (matching clang) it isn't listed here.
static Bool pp_is_known_builtin(const u8* s, size_t n)
{
    static const char* const builtins[] = { "__builtin_clzll" };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (span_eq(s, n, builtins[i])) return 1;
    return 0;
}

// Handles `defined NAME` / `defined(NAME)`, integer literals, `!`, `&&`, `||`, and
// parens. Per C, an identifier that isn't a macro evaluates to 0.
typedef struct { Pp* pp; const u8* s; const u8* end; } IfEval;

static void if_ws(IfEval* e) { while (e->s < e->end && is_hspace(*e->s)) e->s++; }

static i64 if_or(IfEval* e);

static i64 if_primary(IfEval* e)
{
    if_ws(e);
    if (e->s >= e->end) return 0;
    u8 c = *e->s;
    if (c == '!') { e->s++; return !if_primary(e); }
    if (c == '(') {
        e->s++;
        i64 v = if_or(e);
        if_ws(e);
        if (e->s < e->end && *e->s == ')') e->s++;
        return v;
    }
    if (is_decimal(c)) {
        i64 v = 0;
        while (e->s < e->end && is_decimal(*e->s)) v = v * 10 + (*e->s++ - '0');
        while (e->s < e->end && (*e->s == 'u' || *e->s == 'U' || *e->s == 'l' || *e->s == 'L')) e->s++;
        return v;
    }
    if (is_ident_start(c)) {
        const u8* id = e->s;
        while (e->s < e->end && is_ident_continue(*e->s)) e->s++;
        size_t id_len = (size_t)(e->s - id);
        if (id_len == 7 && bytes_eq(id, (const u8*)"defined", 7)) {
            if_ws(e);
            Bool paren = (e->s < e->end && *e->s == '(');
            if (paren) { e->s++; if_ws(e); }
            const u8* nm = e->s;
            while (e->s < e->end && is_ident_continue(*e->s)) e->s++;
            size_t nm_len = (size_t)(e->s - nm);
            if (paren) { if_ws(e); if (e->s < e->end && *e->s == ')') e->s++; }
            return pp_is_defined(e->pp, nm, nm_len);   // includes builtins like __has_builtin / __FILE__
        }
        // __has_builtin(NAME): 1 if cc provides that builtin, else 0 (like clang/gcc's operator)
        if (span_eq(id, id_len, "__has_builtin")) {
            if_ws(e);
            if (e->s >= e->end || *e->s != '(') { LOG_STRING("pp: expected '(' after __has_builtin"); process_exit(71); }
            e->s++; if_ws(e);
            const u8* nm = e->s;
            while (e->s < e->end && is_ident_continue(*e->s)) e->s++;
            size_t nm_len = (size_t)(e->s - nm);
            if_ws(e); if (e->s < e->end && *e->s == ')') e->s++;
            return pp_is_known_builtin(nm, nm_len);
        }
        return 0;   // an identifier that isn't a macro -> 0
    }
    return 0;
}

static i64 if_and(IfEval* e)
{
    i64 v = if_primary(e);
    for (;;) {
        if_ws(e);
        if (!(e->s + 1 < e->end && e->s[0] == '&' && e->s[1] == '&')) break;
        e->s += 2;
        i64 r = if_primary(e);
        v = (v && r);
    }
    return v;
}

static i64 if_or(IfEval* e)
{
    i64 v = if_and(e);
    for (;;) {
        if_ws(e);
        if (!(e->s + 1 < e->end && e->s[0] == '|' && e->s[1] == '|')) break;
        e->s += 2;
        i64 r = if_and(e);
        v = (v || r);
    }
    return v;
}

// A macro body may contain comments. cc strips comments in the lexer, but a macro body is inlined at
// each use, bypassing that, so its comments are removed here (comment -> nothing). Returns the original
// pointer when the body has no comment; otherwise a cleaned copy in the arena. String/char literals are
// respected so a `//` or `/*` inside one is preserved.
static const u8* clean_macro_body(const u8* body, size_t len, size_t* out_len)
{
    Bool has_comment = 0;
    for (size_t k = 0; k + 1 < len; k++)
        if (body[k] == '/' && (body[k + 1] == '/' || body[k + 1] == '*')) { has_comment = 1; break; }
    if (!has_comment) { *out_len = len; return body; }

    u8* clean = ARENA_ALLOC(&global_arena, len ? len : 1);
    size_t n = 0;
    for (size_t k = 0; k < len; ) {
        u8 c = body[k];
        if (c == '"' || c == '\'') {                       // copy a string/char literal verbatim
            u8 q = c; clean[n++] = body[k++];
            while (k < len) {
                clean[n++] = body[k];
                if (body[k] == '\\' && k + 1 < len) { k++; clean[n++] = body[k]; }
                else if (body[k] == q) { k++; break; }
                k++;
            }
            continue;
        }
        if (c == '/' && k + 1 < len && body[k + 1] == '/') break;   // line comment: drop the rest
        if (c == '/' && k + 1 < len && body[k + 1] == '*') {        // block comment: skip through */
            k += 2;
            while (k + 1 < len && !(body[k] == '*' && body[k + 1] == '/')) k++;
            k += 2;
            continue;
        }
        clean[n++] = body[k++];
    }
    *out_len = n;
    return clean;
}

// Emit [s,end) to `out`, replacing `defined X` / `defined(X)` and `__has_builtin(X)` with "1"/"0".
// Resolving these before macro expansion protects their identifier operand from being expanded (per
// C: e.g. `#if defined(_WIN32)` must test the name, not expand the predefined `_WIN32`).
static void pp_if_resolve_ops(Pp* pp, const u8* s, const u8* end, PpOut* out)
{
    while (s < end) {
        if (!is_ident_start(*s)) { pp_emit(out, *s); s++; continue; }
        const u8* id = s;
        while (s < end && is_ident_continue(*s)) s++;
        size_t n = (size_t)(s - id);
        Bool is_defined = span_eq(id, n, "defined");
        Bool is_has_builtin = span_eq(id, n, "__has_builtin");
        if (!is_defined && !is_has_builtin) { pp_emit_str(out, (const char*)id, (size_t)(s - id)); continue; }
        while (s < end && is_hspace(*s)) s++;
        Bool paren = (s < end && *s == '(');
        if (is_has_builtin && !paren) { LOG_STRING("pp: expected '(' after __has_builtin"); process_exit(71); }
        if (paren) { s++; while (s < end && is_hspace(*s)) s++; }
        const u8* nm = s;
        while (s < end && is_ident_continue(*s)) s++;
        size_t nlen = (size_t)(s - nm);
        if (paren) { while (s < end && is_hspace(*s)) s++; if (s < end && *s == ')') s++; }
        Bool yes = is_defined ? pp_is_defined(pp, nm, nlen)   // includes builtins (__has_builtin, __FILE__, ...)
                              : pp_is_known_builtin(nm, nlen);
        pp_emit(out, yes ? '1' : '0'); pp_emit(out, ' ');
    }
}

static Bool pp_eval_if(Pp* pp, const u8* expr, const u8* end)
{
    // resolve defined/__has_builtin (operands protected) into the reused if_scratch, macro-expand that
    // into pp->out's tail, evaluate the integer expression, then truncate the tail back off pp->out.
    pp->if_scratch.size = 0;
    pp_if_resolve_ops(pp, expr, end, &pp->if_scratch);

    size_t mark = pp->out.size;
    pp_expand(pp, pp->if_scratch.pages.ptr, pp->if_scratch.pages.ptr + pp->if_scratch.size);

    IfEval e = { pp, pp->out.pages.ptr + mark, pp->out.pages.ptr + pp->out.size };
    Bool result = if_or(&e) != 0;
    pp->out.size = mark;
    return result;
}

static PpStop pp_block(Pp* pp, Bool emitting);   // forward decl: pp_include recurses into it (file/line live on Pp)

static Bool is_path_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

// length of the directory portion of `path`: everything up to (not including) the last path separator,
// or 0 if there is none. So get_dir_name("a/b/c.h", 7) == 3 ("a/b").
static size_t get_dir_name(const char* path, size_t len)
{
    size_t n = len;
    while (n > 0 && !is_path_sep(path[n - 1])) n--;
    if (n > 0) n--;   // drop the separator itself
    return n;
}

// resolves "." and ".." components so we can check for equality between include paths
static size_t normalize_include_path(char* p, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; i < len; ) {
        size_t cs = i;
        while (i < len && p[i] != '/') i++;
        size_t clen = i - cs;
        if (i < len) i++;                        // consume the '/'
        if (clen == 1 && p[cs] == '.') {
            // "." -> drop
        } else if (clen == 2 && p[cs] == '.' && p[cs + 1] == '.' && w > 0) {
            w--;                                 // ".." -> pop the previous component (and its trailing '/')
            while (w > 0 && p[w - 1] != '/') w--;
        } else {
            for (size_t c = 0; c < clen; c++) p[w++] = p[cs + c];
            p[w++] = '/';
        }
    }
    if (w > 0 && p[w - 1] == '/') w--;           // drop the trailing '/'
    return w;
}

static void pp_read_include(const char* path, size_t path_len, u8** out, size_t* out_len, const char** out_path)
{
    File f;
    Error e = file_open_span(path, path_len, &f);
    if (e) {
        char buf[512];
        Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
        MUST(SINK_LITERAL(&w, "error: open include \""));
        MUST(sink_put(&w, path, path_len));
        MUST(SINK_LITERAL(&w, "\" failed, error="));
        MUST(sink_format_error(&w, e));
        MUST(SINK_LITERAL(&w, "\n"));
        MUST(sink_flush(&w));
        process_exit(e);
    }
    size_t size; MUST(file_size(f, &size));
    // one stable allocation holds the file content, then a copy of `path`, the include cache keys on
    // that copy, which must outlive the arena-allocated path the caller passed in.
    u8* mem; MUST(pages_alloc_fixed((void**)&mem, size + path_len));
    size_t total = 0;
    while (total < size) { size_t got; MUST(file_read(f, mem + total, size - total, &got)); if (!got) break; total += got; }
    file_close(f);
    libc_memcpy(mem + size, path, path_len);
    *out = mem; *out_len = total; *out_path = (const char*)(mem + size);
}

typedef struct { const u8* bytes; size_t len; } CachedInclude;

// process the file referenced by #include "rel", sharing this Pp's macros and output. `rel` is the
// quoted path; resolution is relative to the including file's directory (cur_dir), so `..` works.
static void pp_include(Pp* pp, const u8* rel, size_t rel_len)
{
    size_t dl = pp->cur_dir_len;
    size_t full_len = dl ? dl + 1 + rel_len : rel_len;
    char* full = ARENA_ALLOC(&global_arena, full_len + 1);
    size_t k = 0;
    if (dl) { libc_memcpy(full, pp->cur_dir, dl); full[dl] = '/'; k = dl + 1; }
    libc_memcpy(full + k, rel, rel_len);
    full_len = normalize_include_path(full, full_len);
    full[full_len] = 0;

    u8* bytes; size_t blen;
    u32 ci;
    if (name_map_get(&pp->include_cache, (const u8*)full, full_len, &ci)) {
        CachedInclude* cf = SEG_LIST_REF(CachedInclude, &pp->include_files, ci);
        bytes = (u8*)cf->bytes; blen = cf->len;
    } else {
        const char* key;   // a stable copy of `full`, allocated alongside the content (full is arena/temp)
        pp_read_include(full, full_len, &bytes, &blen, &key);
        CachedInclude nf = { bytes, blen };
        SEG_LIST_APPEND(CachedInclude, &pp->include_files, nf);
        name_map_put(&pp->include_cache, (const u8*)key, full_len, (u32)pp->include_count++);
    }

    size_t ndir = get_dir_name(full, full_len);   // the included file's own dir, for its nested includes

    const u8* save_src = pp->src; size_t save_len = pp->len, save_i = pp->i;
    const char* save_dir = pp->cur_dir; size_t save_dlen = pp->cur_dir_len;
    const char* save_file = pp->cur_file; size_t save_flen = pp->cur_file_len, save_line = pp->cur_line;
    pp->src = bytes; pp->len = blen; pp->i = 0;
    pp->cur_dir = full; pp->cur_dir_len = ndir;
    pp->cur_file = full; pp->cur_file_len = full_len; pp->cur_line = 1;

    pp_emit_marker(&pp->out, 1, full, full_len);   // entering the included file at its line 1
    if (pp_block(pp, 1) != PP_EOF) { LOG_STRING("pp: unbalanced #if in included file"); process_exit(72); }

    pp->src = save_src; pp->len = save_len; pp->i = save_i;
    pp->cur_dir = save_dir; pp->cur_dir_len = save_dlen;
    pp->cur_file = save_file; pp->cur_file_len = save_flen; pp->cur_line = save_line;
}

// process the system header referenced by #include <name>, resolved against the embedded header
// table. Shares this Pp's macros and output, like pp_include; an embedded header has no directory,
// so its own nested includes must be angle-bracket (resolved here too).
static void pp_include_embedded(Pp* pp, size_t line, const u8* name, size_t name_len)
{
    int hidx = pp->stdinc ? lookup_include(name, name_len) : -1;
    if (hidx >= 0) {
        const char* bytes = embed_header_content[hidx];
        size_t blen = embed_header_content_size[hidx];
        const u8* save_src = pp->src; size_t save_len = pp->len, save_i = pp->i;
        const char* save_dir = pp->cur_dir; size_t save_dlen = pp->cur_dir_len;
        const char* save_file = pp->cur_file; size_t save_flen = pp->cur_file_len, save_line = pp->cur_line;
        pp->src = (u8*)bytes; pp->len = blen; pp->i = 0;
        pp->cur_dir = 0; pp->cur_dir_len = 0;
        pp->cur_file = (const char*)name; pp->cur_file_len = name_len; pp->cur_line = 1;   // name spans the (saved) outer src

        pp_emit_marker(&pp->out, 1, (const char*)name, name_len);
        if (pp_block(pp, 1) != PP_EOF) { LOG_STRING("pp: unbalanced #if in included header"); process_exit(72); }

        pp->src = save_src; pp->len = save_len; pp->i = save_i;
        pp->cur_dir = save_dir; pp->cur_dir_len = save_dlen;
        pp->cur_file = save_file; pp->cur_file_len = save_flen; pp->cur_line = save_line;
        return;
    }
    char buf[512];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    if (pp->cur_file && pp->cur_file_len) MUST(sink_put(&w, pp->cur_file, pp->cur_file_len));
    else                                  MUST(SINK_LITERAL(&w, "<source>"));
    MUST(SINK_LITERAL(&w, ":"));
    MUST(sink_format_u64(&w, line));
    MUST(SINK_LITERAL(&w, ": error: #include <"));
    MUST(sink_put(&w, (const char*)name, name_len));
    MUST(SINK_LITERAL(&w, ">: no such embedded system header\n"));
    MUST(sink_flush(&w));
    process_exit(71);
}

// report a preprocessor error at a source location (cur_file:line) and exit
static void pp_error_at(Pp* pp, size_t line, const char* msg)
{
    char buf[512];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    if (pp->cur_file && pp->cur_file_len) MUST(sink_put(&w, pp->cur_file, pp->cur_file_len));
    else                                  MUST(SINK_LITERAL(&w, "<source>"));
    MUST(SINK_LITERAL(&w, ":"));
    MUST(sink_format_u64(&w, line));
    MUST(SINK_LITERAL(&w, ": error: "));
    MUST(sink_put(&w, msg, libc_strlen(msg)));
    MUST(SINK_LITERAL(&w, "\n"));
    MUST(sink_flush(&w));
}

// Process lines until the directive closing THIS conditional group (#else/#elif/#endif)
// or EOF, recursing on nested #if. Emit lines only while `emitting`; dropped directive/
// excluded lines become blank lines so original line numbers survive.
static PpStop pp_block(Pp* pp, Bool emitting)
{
    while (pp->i < pp->len) {
        size_t line_begin = pp->i;
        size_t line_end = pp->i;
        while (line_end < pp->len && pp->src[line_end] != '\n') line_end++;
        // a logical line follows backslash-newline continuations (the `\`<nl>s are stripped at expansion)
        while (line_end > line_begin && pp->src[line_end - 1] == '\\' && line_end < pp->len) {
            line_end++;                                                          // past the '\n'
            while (line_end < pp->len && pp->src[line_end] != '\n') line_end++;  // to the next physical newline
        }
        Bool has_nl = line_end < pp->len;
        size_t next = line_end + (has_nl ? 1 : 0);
        size_t nls = 0;                          // input newlines this logical line spans (>1 if continued)
        for (size_t z = line_begin; z < next; z++) if (pp->src[z] == '\n') nls++;

        size_t p = line_begin;
        while (p < line_end && is_hspace(pp->src[p])) p++;

        if (p < line_end && pp->src[p] == '#') {
            size_t q = p + 1;
            while (q < line_end && is_hspace(pp->src[q])) q++;
            const u8* nm = &pp->src[q];
            while (q < line_end && pp->src[q] >= 'a' && pp->src[q] <= 'z') q++;
            size_t nlen = (size_t)(&pp->src[q] - nm);

            size_t directive_line = pp->cur_line;   // this directive's line (cur_line is advanced below)
            for (size_t k = 0; k < nls; k++) pp_emit(&pp->out, '\n');   // directive (incl. its continuations) -> blank lines
            pp->i = next;
            pp->cur_line += nls;

            if (span_eq(nm, nlen, "if") || span_eq(nm, nlen, "ifdef") || span_eq(nm, nlen, "ifndef")) {
                Bool cond;
                if (span_eq(nm, nlen, "if")) {
                    cond = pp_eval_if(pp, &pp->src[q], &pp->src[line_end]);
                } else {
                    size_t r = q, name_len;
                    const u8* name = read_ident(pp->src, &r, line_end, &name_len);
                    Bool defined = pp_is_defined(pp, name, name_len);
                    cond = span_eq(nm, nlen, "ifndef") ? !defined : defined;
                }
                Bool any_taken = cond;
                PpStop s = pp_block(pp, emitting && cond);
                while (1) {
                    switch (s) {
                    case PP_EOF:
                        pp_error_at(pp, directive_line, "unterminated #if");
                        process_exit(72);
                    case PP_ENDIF: goto after_loop;
                    case PP_ELSE: {
                        Bool active = emitting && !any_taken;
                        any_taken = 1;
                        s = pp_block(pp, active);
                        break;
                    }
                    case PP_ELIF: {
                        Bool active = emitting && !any_taken && pp->elif_cond;
                        if (active) any_taken = 1;
                        s = pp_block(pp, active);
                        break;
                    }
                    }
                }
            after_loop:
                ;
            } else if (span_eq(nm, nlen, "else")) {
                return PP_ELSE;
            } else if (span_eq(nm, nlen, "elif")) {
                pp->elif_cond = pp_eval_if(pp, &pp->src[q], &pp->src[line_end]);
                return PP_ELIF;
            } else if (span_eq(nm, nlen, "endif")) {
                return PP_ENDIF;
            } else if (span_eq(nm, nlen, "define")) {
                if (emitting) {
                    size_t r = q, name_len;
                    const u8* name = read_ident(pp->src, &r, line_end, &name_len);
                    if (name_len == 0) { LOG_STRING("pp: #define missing name"); process_exit(74); }
                    const u8* params = 0; size_t params_len = 0; Bool function_like = 0;
                    if (r < line_end && pp->src[r] == '(') {    // '(' adjacent to name -> function-like
                        function_like = 1;
                        r++;
                        params = &pp->src[r];
                        while (r < line_end && pp->src[r] != ')') r++;
                        if (r >= line_end) { LOG_STRING("pp: unterminated macro parameter list"); process_exit(74); }
                        params_len = (size_t)(&pp->src[r] - params);
                        r++;                                    // past ')'
                    }
                    while (r < line_end && is_hspace(pp->src[r])) r++;
                    const u8* body = &pp->src[r];
                    size_t body_len = line_end - r;
                    body = clean_macro_body(body, body_len, &body_len);   // strip comments (the body is inlined)
                    while (body_len > 0 && is_hspace(body[body_len - 1])) body_len--;
                    macro_define(pp, name, name_len, params, params_len, function_like, body, body_len);
                }
            } else if (span_eq(nm, nlen, "undef")) {
                if (emitting) {
                    size_t r = q, name_len;
                    const u8* name = read_ident(pp->src, &r, line_end, &name_len);
                    Macro* m = macro_find(pp, name, name_len);
                    if (m) m->defined = 0;
                }
            } else if (span_eq(nm, nlen, "pragma")) {
                if (emitting) {   // a #pragma in a skipped conditional branch is not ours to judge
                    size_t r = q, pn;
                    const u8* pname = read_ident(pp->src, &r, line_end, &pn);
                    // #pragma pack changes struct layout; cc doesn't model it, and silently ignoring it would
                    // corrupt every struct after it -- so fail loud. Every other #pragma is a hint (warning,
                    // comment, region, GCC diagnostics, ...) cc ignores, exactly as gcc/clang ignore unknown pragmas.
                    if (span_eq(pname, pn, "pack")) {
                        pp_error_at(pp, directive_line, "unsupported #pragma pack (changes struct layout)");
                        process_exit(74);
                    }
                }
            } else if (span_eq(nm, nlen, "include")) {
                if (emitting) {
                    size_t r = q;
                    while (r < line_end && is_hspace(pp->src[r])) r++;
                    if (r < line_end && pp->src[r] == '"') {
                        r++;
                        size_t ps = r;
                        while (r < line_end && pp->src[r] != '"') r++;
                        pp_include(pp, &pp->src[ps], r - ps);
                        pp_emit_marker(&pp->out, pp->cur_line, pp->cur_file, pp->cur_file_len);   // back in this file, after the #include
                    } else if (r < line_end && pp->src[r] == '<') {
                        r++;
                        size_t ps = r;
                        while (r < line_end && pp->src[r] != '>') r++;
                        pp_include_embedded(pp, directive_line, &pp->src[ps], r - ps);
                        pp_emit_marker(&pp->out, pp->cur_line, pp->cur_file, pp->cur_file_len);   // back in this file
                    } else {
                        pp_error_at(pp, directive_line, "malformed #include");
                        process_exit(74);
                    }
                }
            } else if (span_eq(nm, nlen, "error")) {
                if (emitting) {
                    char stderr_buf[256];
                    Sink stderr = sink_init(&stderr_vtable, stderr_buf, sizeof(stderr_buf));
                    MUST(SINK_LITERAL(&stderr, "#error:"));
                    MUST(sink_put(&stderr, (const char*)&pp->src[q], line_end - q));
                    MUST(SINK_LITERAL(&stderr, "\n"));
                    MUST(sink_flush(&stderr));
                    process_exit(71);
                }
            } else if (nlen == 0 && q >= line_end) {
                // explicitly ignored: null directive (`#` alone)
            } else {
                // everything else (#if, #include, #error, #line, garbage, ...) -- fail
                // loud so a new directive surfaces as a test error and we decide then
                // whether to ignore it or implement it.
                char stderr_buf[128];
                Sink stderr = sink_init(&stderr_vtable, stderr_buf, sizeof(stderr_buf));
                MUST(SINK_LITERAL(&stderr, "pp: unhandled directive #"));
                MUST(sink_put(&stderr, (const char*)nm, nlen));
                MUST(SINK_LITERAL(&stderr, "\n"));
                MUST(sink_flush(&stderr));
                process_exit(71);
            }
        } else {
            if (emitting) {
                // expand to end-of-source (not end-of-line) so a macro call's args may span newlines;
                // pp_expand stops at the logical line's closing newline and returns it.
                const u8* stop = pp_expand(pp, &pp->src[line_begin], &pp->src[pp->len]);
                size_t stop_off = (size_t)(stop - pp->src);
                if (stop_off < pp->len) {
                    pp_emit(&pp->out, '\n');
                    size_t consumed = 0;                 // input lines this logical line spanned (scans only this line)
                    for (size_t z = line_begin; z <= stop_off; z++) if (pp->src[z] == '\n') consumed++;
                    pp->cur_line += consumed;
                    pp->i = stop_off + 1;
                    if (consumed > 1) pp_emit_marker(&pp->out, pp->cur_line, pp->cur_file, pp->cur_file_len);   // resync after a multi-line macro call
                } else {
                    pp->i = pp->len;                     // reached EOF (no trailing newline)
                }
            } else {
                for (size_t k = 0; k < nls; k++) pp_emit(&pp->out, '\n');   // excluded line(s) -> blank placeholders
                pp->i = next;
                pp->cur_line += nls;
            }
        }
    }
    return PP_EOF;
}

void preprocess(
    const u8* src, size_t len,
    const char* file, size_t file_len,
    const Defines* defines,
    Bool stdinc,
    u8** out, size_t* out_len
) {
    Pp pp;
    pp.src = src;
    pp.len = len;
    pp.stdinc = stdinc;
    pp.i = 0;
    pp.out.pages = PAGES_INIT();
    pp.out.size = 0;
    pp.if_scratch.pages = PAGES_INIT();
    pp.if_scratch.size = 0;
    pp.macros = SEG_LIST_INIT(Macro);
    pp.macro_count = 0;
    pp.macro_map = name_map_init();
    pp.include_cache = name_map_init();
    pp.include_files = SEG_LIST_INIT(CachedInclude);
    pp.include_count = 0;
    pp.frames = PAGES_INIT();
    pp.frame_depth = 0;
    pp.cur_dir = file;
    pp.cur_dir_len = get_dir_name(file, file_len);    // the file's directory, for relative #include
    pp.cur_file = file;
    pp.cur_file_len = file_len;
    pp.cur_line = 1;
    pp.defines = defines;

    // each custom (-D) define is "NAME" (defined to 1) or "NAME=VALUE"
    for (size_t i = 0; i < defines->custom_count; i++) {
        const u8* d = (const u8*)defines->custom[i];
        size_t dlen = libc_strlen(defines->custom[i]);
        size_t eq = 0;
        while (eq < dlen && d[eq] != '=') eq++;
        if (eq < dlen) macro_define(&pp, d, eq, 0, 0, 0, d + eq + 1, dlen - eq - 1);
        else           macro_define(&pp, d, dlen, 0, 0, 0, (const u8*)"1", 1);
    }

    pp_emit_marker(&pp.out, 1, file, file_len);       // the main file starts at line 1
    PpStop s = pp_block(&pp, 1);
    if (s != PP_EOF) { LOG_STRING("pp: #else/#elif/#endif without #if"); process_exit(70); }

    pp_emit(&pp.out, 0);   // NUL sentinel for the lexer

    pages_deinit(&pp.if_scratch.pages);
    pages_deinit(&pp.frames);
    *out = pp.out.pages.ptr;
    *out_len = pp.out.size - 1;
    MUST(lock_pages(&pp.out.pages));
}
