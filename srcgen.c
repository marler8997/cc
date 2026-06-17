#include "ur/arena.h"
#include "ur/cli.h"
#include "ur/error.h"
#include "ur/filesink.h"
#include "ur/fs.h"
#include "ur/int.h"
#include "ur/mem.h"
#include "ur/pagesize.h"
#include "ur/processexit.h"
#include "ur/segment.h"
#include "ur/sink.h"
#include "src/builtindefines.h"

#include "ur/abortmacros.c"
#include "ur/allocstats.c"
#include "ur/arena.c"
#include "ur/clzll.c"
#include "ur/crtstart.c"
#include "ur/filesink.c"
#include "ur/int.c"
#include "ur/mem.c"
#include "ur/pages.c"
#include "ur/pagesize.c"
#include "ur/segment.c"
#include "ur/sink.c"
#include "ur/unicode.c"

static void die_file_error(const char* action, const char* path, Error err)
{
    char buf[256];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    MUST(SINK_LITERAL(&w, "embed: "));
    MUST(sink_put(&w, action, libc_strlen(action)));
    MUST(SINK_LITERAL(&w, " "));
    MUST(sink_put(&w, path, libc_strlen(path)));
    MUST(SINK_LITERAL(&w, ": error "));
    MUST(sink_format_error(&w, err));
    MUST(SINK_LITERAL(&w, "\n"));
    MUST(sink_flush(&w));
    process_exit(err);
}

// Encode `size` raw input bytes onto `sink` as quoted string segments (one per source line; '\r'
// dropped so CRLF matches). Stateless across calls: each chunk is self-contained, so a source line
// split at a page boundary just becomes two adjacent (concatenated) string literals.
static Error embed_data(Sink* sink, const u8* data, size_t size)
{
#define TRY(call) do { Error _e = (call); if (_e) return _e; } while (0)
    Bool seg_started = 0;
    for (size_t i = 0; i < size; i++) {
        u8 ch = data[i];
        switch (ch) {
        case '\r': break;
        case '\n':
            if (!seg_started) TRY(SINK_LITERAL(sink, "\n    \""));
            TRY(SINK_LITERAL(sink, "\\n\""));
            seg_started = 0;
            break;
        case '\\': case '"':
            if (!seg_started) { TRY(SINK_LITERAL(sink, "\n    \"")); seg_started = 1; }
            TRY(sink_put_byte(sink, '\\'));
            TRY(sink_put_byte(sink, ch));
            break;
        default:
            if (!seg_started) { TRY(SINK_LITERAL(sink, "\n    \"")); seg_started = 1; }
            TRY(sink_put_byte(sink, ch));
            break;
        }
    }
    if (seg_started) TRY(sink_put_byte(sink, '"'));   // close a line that had no trailing '\n'
    return 0;
#undef TRY
}

// Read `path` a page at a time and emit `const unsigned char <symbol>[] = "...";` + `size_t
// <symbol>_size` onto `sink`. The file is never held whole; `page` is the caller's read window.
static void write_file_embed(Sink* sink, u8* page, const char* path, const char* symbol, Bool is_header)
{
    if (is_header) MUST(SINK_LITERAL(sink, "static "));
    MUST(SINK_LITERAL(sink, "const char "));
    MUST(sink_put(sink, symbol, libc_strlen(symbol)));
    MUST(SINK_LITERAL(sink, "[] ="));

    File in_file;
    Error e = file_open_z(path, libc_strlen(path), &in_file);
    if (e) die_file_error("cannot open", path, e);
    Bool any = 0;
    while (1) {
        size_t got;
        MUST(file_read(in_file, page, page_size(), &got));
        if (got == 0) break;
        any = 1;
        MUST(embed_data(sink, page, got));
    }
    file_close(in_file);

    if (!any) MUST(SINK_LITERAL(sink, "\n    \"\""));   // empty file: still need a literal
    MUST(SINK_LITERAL(sink, ";\n"));
    if (!is_header) {
        MUST(SINK_LITERAL(sink, "size_t "));
        MUST(sink_put(sink, symbol, libc_strlen(symbol)));
        MUST(SINK_LITERAL(sink, "_size = sizeof("));
        MUST(sink_put(sink, symbol, libc_strlen(symbol)));
        MUST(SINK_LITERAL(sink, ") - 1;\n"));
    }
}

static void emit_indent(Sink* w, size_t spaces)
{
    for (size_t i = 0; i < spaces; i++) MUST(sink_put_byte(w, ' '));
}

static void emit_switch_trie(
    Sink* w,
    size_t count,
    const char** keys,
    const char** values,     // the per-key return expression; ignored when value_prefix != 0
    size_t* idx,             // reordered in place to group candidates by the split char
    size_t key_len,
    const char* fallback,
    Bool value_is_index,     // if set, a matched key returns its index (position in keys[]), not values[i]
    size_t indent            // spaces for this switch's case labels; its `}`/fallback sit at indent-4
) {
    ASSERT(count != 0);
    if (count == 1) {
        size_t i = idx[0];
        MUST(SINK_LITERAL(w, "return libc_memcmp(s, \""));
        MUST(sink_put(w, keys[i], key_len));
        MUST(SINK_LITERAL(w, "\", "));
        MUST(sink_format_u64(w, key_len));
        MUST(SINK_LITERAL(w, ") ? "));
        MUST(sink_put(w, fallback, libc_strlen(fallback)));
        MUST(SINK_LITERAL(w, " : "));
        if (value_is_index) {
            MUST(sink_format_u64(w, i));   // the key's own index -- free here, no enum/allocation
        } else {
            MUST(sink_put(w, values[i], libc_strlen(values[i])));
        }
        MUST(SINK_LITERAL(w, ";\n"));
        return;
    }

    // find a position whose char is unique across all candidates (separates them in one switch)
    size_t split = key_len;   // len == "no single position separates them all"
    for (size_t p = 0; p < key_len && split == key_len; p++) {
        Bool seen[256] = {0};
        Bool unique = 1;
        for (size_t a = 0; a < count; a++) { u8 c = (u8)keys[idx[a]][p]; if (seen[c]) { unique = 0; break; } seen[c] = 1; }
        if (unique) split = p;
    }
    if (split == key_len) {   // fall back to the position with the most distinct chars; groups recurse
        size_t best = 0;
        for (size_t p = 0; p < key_len; p++) {
            Bool seen[256] = {0};
            size_t distinct = 0;
            for (size_t a = 0; a < count; a++) { u8 c = (u8)keys[idx[a]][p]; if (!seen[c]) { seen[c] = 1; distinct++; } }
            if (distinct > best) { best = distinct; split = p; }
        }
    }

    MUST(SINK_LITERAL(w, "switch (s["));
    MUST(sink_format_u64(w, split));
    MUST(SINK_LITERAL(w, "]) {\n"));
    // partition idx in place by s[split]: gather each distinct char's entries into a contiguous
    // sub-range, then recurse on it. Sibling ranges are disjoint, so a recursive call only reorders
    // within its own slice -- the already-emitted/not-yet-seen groups are never disturbed.
    for (size_t group = 0; group < count; ) {
        char c = keys[idx[group]][split];
        size_t end = group + 1;
        for (size_t a = end; a < count; a++) {
            if (keys[idx[a]][split] == c) {
                size_t t = idx[end]; idx[end] = idx[a]; idx[a] = t;
                end++;
            }
        }
        emit_indent(w, indent);
        MUST(SINK_LITERAL(w, "case '"));
        MUST(sink_put_byte(w, (u8)c));
        MUST(SINK_LITERAL(w, "': "));
        emit_switch_trie(w, end - group, keys, values, idx + group, key_len, fallback, value_is_index, indent + 4);
        group = end;
    }
    emit_indent(w, indent - 4);
    MUST(SINK_LITERAL(w, "}\n"));
    emit_indent(w, indent - 4);
    MUST(SINK_LITERAL(w, "return "));
    MUST(sink_put(w, fallback, libc_strlen(fallback)));
    MUST(SINK_LITERAL(w, ";\n"));
}

static void emit_length_switch(
    Sink* w,
    size_t count,
    const char** keys,
    const char** values,
    size_t* index_buf, // big enough to hold "count" values
    const char* fallback,
    Bool value_is_index // if set, a matched key returns its index (position in keys[]), not values[i]
) {
    MUST(SINK_LITERAL(w, "    switch (n) {\n"));
    size_t handled = 0;
    for (size_t len = 1; handled < count; len++) {
        size_t gn = 0;
        for (size_t i = 0; i < count; i++) if (libc_strlen(keys[i]) == len) index_buf[gn++] = i;
        handled += gn;
        if (gn == 0 && handled == 0) continue;   // skip leading lengths with no entries
        MUST(SINK_LITERAL(w, "    case "));
        MUST(sink_format_u64(w, len));
        MUST(SINK_LITERAL(w, ": "));
        if (gn == 0) {
            MUST(SINK_LITERAL(w, "return "));
            MUST(sink_put(w, fallback, libc_strlen(fallback)));
            MUST(SINK_LITERAL(w, ";\n"));
            continue;
        }
        emit_switch_trie(w, gn, keys, values, index_buf, len, fallback, value_is_index, 8);
    }
    MUST(SINK_LITERAL(w, "    }\n    return "));
    MUST(sink_put(w, fallback, libc_strlen(fallback)));
    MUST(SINK_LITERAL(w, ";\n"));
}

void gen_libc_embed(void)
{
    struct { const char* path; const char* symbol; } files[] = {
        { "libc/libc.c",           "embed_libc_c"  },
        { "libc/include/stdio.h",  "embed_stdio_h" },
        { "libc/include/stdlib.h",  "embed_stdlib_h" },
        { "libc/include/errno.h",  "embed_errno_h" },
        { "libc/include/string.h",  "embed_string_h" },
        { "libc/include/time.h",  "embed_time_h" },
        { "libc/include/math.h",  "embed_math_h" },
        { "libc/include/inttypes.h",  "embed_inttypes_h" },
        { "libc/include/stdint.h",  "embed_stdint_h" },
        { "libc/include/limits.h",  "embed_limits_h" },
    };
    size_t file_count = sizeof(files) / sizeof(files[0]);
    File out_file;
    #define SUB_PATH "generated/libc_embed.c"
    Error create_err = file_create_z(SUB_PATH, sizeof(SUB_PATH) - 1, 0, &out_file);
    if (create_err) die_file_error("cannot create", SUB_PATH, create_err);
    #undef SUB_PATH

    char out_buf[4096];
    FileSink file_sink = file_sink_init(out_file, out_buf, sizeof(out_buf));
    Sink* sink = &file_sink.base;

    {
        const char* prologue = "#include \"../ur/int.h\"\n#include \"../ur/mem.h\"\n\n";   // standalone TU: size_t, libc_memcmp
        MUST(sink_put(sink, prologue, libc_strlen(prologue)));
    }
    {
        u8* page = segment_reserve(segment_class(page_size()));
        for (size_t i = 0; i < file_count; i++) {
            Bool is_header = (i > 0);
            write_file_embed(sink, page, files[i].path, files[i].symbol, is_header);
        }
        segment_release(segment_class(page_size()), page);
    }

    // The header bytes/length, indexed by lookup_include's return value (files[0] is libc.c, not a header).
    MUST(SINK_LITERAL(sink, "\nconst char* embed_header_content[] = {\n"));
    for (size_t i = 1; i < file_count; i++) {
        MUST(SINK_LITERAL(sink, "    "));
        MUST(sink_put(sink, files[i].symbol, libc_strlen(files[i].symbol)));
        MUST(SINK_LITERAL(sink, ",\n"));
    }
    MUST(SINK_LITERAL(sink, "};\nsize_t embed_header_content_size[] = {\n"));
    for (size_t i = 1; i < file_count; i++) {
        MUST(SINK_LITERAL(sink, "    sizeof("));
        MUST(sink_put(sink, files[i].symbol, libc_strlen(files[i].symbol)));
        MUST(SINK_LITERAL(sink, ") - 1,\n"));
    }
    MUST(SINK_LITERAL(sink, "};\n"));

    // lookup_include(s, n): index of the header named s[0..n) into embed_header_content[], or -1.
    // switch on length, then a s[0]/s[1]/... char trie, like identifier_tag in lex.c.
    MUST(SINK_LITERAL(sink, "int lookup_include(const u8* s, size_t n)\n{\n"));
    {
        const char* key[sizeof(files) / sizeof(files[0])];
        const char* val[sizeof(files) / sizeof(files[0])];
        size_t index_buf[sizeof(files) / sizeof(files[0])];
        size_t kn = 0;
        for (size_t i = 1; i < file_count; i++) {
            char* idxstr = ARENA_ALLOC(&global_arena, 24);   // the entry's index into the arrays, as text
            u8 nc = format_u64(idxstr, kn);
            idxstr[nc] = 0;
            key[kn] = files[i].path + 13;   // 13 = strlen("libc/include/")
            val[kn] = idxstr; kn++;
        }
        emit_length_switch(sink, kn, key, val, index_buf, "-1", 0);
    }
    MUST(SINK_LITERAL(sink, "}\n"));
    MUST(sink_flush(sink));
    file_close(out_file);
}

// Generate out/keywords.c: identifier_tag(s, n) -> the TOKEN_KW_* tag for keyword s[0..n), else
// TOKEN_IDENTIFIER. Included by lex.c (which supplies the KW macro and the token enum). Same engine
// as lookup_include: switch on length, then a discriminating-position char trie.
void gen_keywords(void)
{
    struct { const char* word; const char* tag; } kws[] = {
        { "if",       "TOKEN_KW_IF"       },
        { "do",       "TOKEN_KW_DO"       },
        { "int",      "TOKEN_KW_INT"      },
        { "for",      "TOKEN_KW_FOR"      },
        { "long",     "TOKEN_KW_LONG"     },
        { "void",     "TOKEN_KW_VOID"     },
        { "else",     "TOKEN_KW_ELSE"     },
        { "enum",     "TOKEN_KW_ENUM"     },
        { "goto",     "TOKEN_KW_GOTO"     },
        { "char",     "TOKEN_KW_CHAR"     },
        { "case",     "TOKEN_KW_CASE"     },
        { "while",    "TOKEN_KW_WHILE"    },
        { "break",    "TOKEN_KW_BREAK"    },
        { "union",    "TOKEN_KW_UNION"    },
        { "short",    "TOKEN_KW_SHORT"    },
        { "const",    "TOKEN_KW_CONST"    },
        { "__asm",    "TOKEN_KW_ASM"      },
        { "signed",   "TOKEN_KW_SIGNED"   },
        { "sizeof",   "TOKEN_KW_SIZEOF"   },
        { "static",   "TOKEN_KW_STATIC"   },
        { "struct",   "TOKEN_KW_STRUCT"   },
        { "switch",   "TOKEN_KW_SWITCH"   },
        { "return",   "TOKEN_KW_RETURN"   },
        { "extern",   "TOKEN_KW_EXTERN"   },
        { "double",   "TOKEN_KW_DOUBLE"   },
        { "inline",   "TOKEN_KW_INLINE"   },
        { "default",  "TOKEN_KW_DEFAULT"  },
        { "typedef",  "TOKEN_KW_TYPEDEF"  },
        { "__asm__",  "TOKEN_KW_ASM"      },
        { "unsigned", "TOKEN_KW_UNSIGNED" },
        { "continue", "TOKEN_KW_CONTINUE" },
        { "volatile", "TOKEN_KW_VOLATILE" },
        { "register", "TOKEN_KW_REGISTER" },
    };
    size_t kw_count = sizeof(kws) / sizeof(kws[0]);
    const char* key[sizeof(kws) / sizeof(kws[0])];
    const char* val[sizeof(kws) / sizeof(kws[0])];
    for (size_t i = 0; i < kw_count; i++) { key[i] = kws[i].word; val[i] = kws[i].tag; }

    File out_file;
    #define SUB_PATH "generated/keywords.c"
    Error create_err = file_create_z(SUB_PATH, sizeof(SUB_PATH) - 1, 0, &out_file);
    if (create_err) die_file_error("cannot create", SUB_PATH, create_err);
    #undef SUB_PATH
    char out_buf[4096];
    FileSink file_sink = file_sink_init(out_file, out_buf, sizeof(out_buf));
    Sink* sink = &file_sink.base;

    MUST(SINK_LITERAL(sink,
        "#include \"../src/lex.h\"\n#include \"../ur/mem.h\"\n\n"
    ));
    MUST(SINK_LITERAL(sink, "u8 identifier_tag(const u8* s, size_t n)\n{\n"));
    size_t index_buf[sizeof(kws) / sizeof(kws[0])];
    emit_length_switch(sink, kw_count, key, val, index_buf, "TOKEN_IDENTIFIER", 0);
    MUST(SINK_LITERAL(sink, "}\n"));
    MUST(sink_flush(sink));
    file_close(out_file);
}

void gen_builtin_defines(void)
{
    const char* key[] = {
#define X(id, spelling) spelling,
        X_BUILTIN_VALUE_DEFINES
        X_BUILTIN_SPECIAL_DEFINES
#undef X
    };
    const char* val[] = {
#define X(id, spelling) "BUILTIN_DEFINE_" #id,
        X_BUILTIN_VALUE_DEFINES
        X_BUILTIN_SPECIAL_DEFINES
#undef X
    };
    size_t count = sizeof(key) / sizeof(key[0]);
    size_t index_buf[sizeof(key) / sizeof(key[0])];
    File out_file;
    #define SUB_PATH "generated/builtindefines.c"
    Error create_err = file_create_z(SUB_PATH, sizeof(SUB_PATH) - 1, 0, &out_file);
    if (create_err) die_file_error("cannot create", SUB_PATH, create_err);
    #undef SUB_PATH
    char out_buf[4096];
    FileSink file_sink = file_sink_init(out_file, out_buf, sizeof(out_buf));
    Sink* sink = &file_sink.base;
    MUST(SINK_LITERAL(sink,
        "#include \"../src/builtindefines.h\"\n#include \"../ur/mem.h\"\n\n"));
    MUST(SINK_LITERAL(sink, "int lookup_builtin_define(const u8* s, size_t n)\n{\n"));
    emit_length_switch(sink, count, key, val, index_buf, "-1", 0);
    MUST(SINK_LITERAL(sink, "}\n"));
    MUST(sink_flush(sink));
    file_close(out_file);
}

void gen_win32defs(void)
{
    struct { const char* path; const char* symbol; } files[] = {
        { "libc/win32/ntdll.def",    "embed_ntdll_def"    },
        { "libc/win32/kernel32.def", "embed_kernel32_def" },
        { "libc/win32/user32.def",   "embed_user32_def"   },
        { "libc/win32/gdi32.def",    "embed_gdi32_def"    },
    };
    size_t file_count = sizeof(files) / sizeof(files[0]);
    File out_file;
    #define SUB_PATH "generated/win32defs.c"
    Error create_err = file_create_z(SUB_PATH, sizeof(SUB_PATH) - 1, 0, &out_file);
    if (create_err) die_file_error("cannot create", SUB_PATH, create_err);
    #undef SUB_PATH
    char out_buf[4096];
    FileSink file_sink = file_sink_init(out_file, out_buf, sizeof(out_buf));
    Sink* sink = &file_sink.base;
    MUST(SINK_LITERAL(sink, "#include \"../ur/int.h\"\n#include \"../ur/size_t.h\"\n\n"));
    {
        u8* page = segment_reserve(segment_class(page_size()));
        for (size_t i = 0; i < file_count; i++)
            write_file_embed(sink, page, files[i].path, files[i].symbol, 1);   // static const char embed_X[]
        segment_release(segment_class(page_size()), page);
    }
    // one array of the builtin .def contents (NUL-terminated text) + a count, iterated at startup
    MUST(SINK_LITERAL(sink, "\nconst char* const win32_builtin_def[] = {\n"));
    for (size_t i = 0; i < file_count; i++) {
        MUST(SINK_LITERAL(sink, "    "));
        MUST(sink_put(sink, files[i].symbol, libc_strlen(files[i].symbol)));
        MUST(SINK_LITERAL(sink, ",\n"));
    }
    MUST(SINK_LITERAL(sink, "};\nconst size_t win32_builtin_def_count = sizeof(win32_builtin_def) / sizeof(win32_builtin_def[0]);\n"));
    MUST(sink_flush(sink));
    file_close(out_file);
}

void verify_int_sizes(void);

#if HEDLEY_HAS_ATTRIBUTE(used) || HEDLEY_GCC_VERSION_CHECK(3,1,0)
    __attribute__((__used__))
#endif
#if defined(_WIN32)
    #if __STDC_HOSTED__
        #define MAIN main
    #else
        #define MAIN crtmain
    #endif
#else
    #define MAIN main
#endif
int MAIN(void)
{
    verify_int_sizes();
    {
        Error error = dir_create_z("generated", 9);
        MUST(error);
    }
    gen_libc_embed();
    gen_keywords();
    gen_builtin_defines();
    gen_win32defs();
    return 0;
}
