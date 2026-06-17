#include "../ur/abortmacros.h"
#include "../ur/alloc_stats_enabled.h"
#include "../ur/arena.h"
#include "../ur/cli.h"
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
#include "defines.h"
#include "exe.h"
#include "import.h"
#include "importlib.h"
#include "link.h"
#include "namemap.h"
#include "obj.h"
#include "parse.h"
#include "pp.h"
#include "program.h"
#include "ir.h"
#include "target.h"

void read_file(File file, u8** out_mem, size_t* out_len)
{
    size_t size;
    MUST(file_size(file, &size));

    u8* mem;
    MUST(pages_alloc_fixed((void**)&mem, size + 1));

    size_t total = 0;
    while (total < size) {
        size_t last_read;
        MUST(file_read(file, mem + total, size - total, &last_read));
        if (last_read == 0) break;
        total += last_read;
    }

    *out_mem = mem;
    *out_len = total;
}

/*
static u32 get_line_col(const u8* src, size_t offset, u32* out_col)
{
    u32 line = 1;
    u32 col = 1;
    for (size_t i = 0; i < offset; i++) {
        if (src[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    *out_col = col;
    return line;
}
*/

typedef struct {
    const u8* file;      // points into src (linemarker filename); NULL if none seen
    u32 file_len;
    u32 line;
    u32 col;
} SourceLoc;

// Map a byte `offset` in the preprocessed source back to an original (file, line, col). The pp emits
// GCC-style linemarkers `# <line> "<file>"`; scan BACKWARD from the offset to the nearest preceding
// marker (cheap -- usually within the current file's run) and count source lines forward from it.
static SourceLoc get_ppd_source_loc(const u8* src, size_t offset)
{
    // start of the line containing `offset`
    size_t line_begin = offset;
    while (line_begin > 0 && src[line_begin - 1] != '\n') line_begin--;

    size_t pos = line_begin;
    u32 lines_below = 0;   // physical lines from `pos`'s line down to the offset's line
    for (;;) {
        // a linemarker at the start of this line?  `# <num> "<file>"`
        if (src[pos] == '#') {
            size_t j = pos + 1;
            while (src[j] == ' ' || src[j] == '\t') j++;
            if (src[j] >= '0' && src[j] <= '9') {
                u32 n = 0;
                while (src[j] >= '0' && src[j] <= '9') n = n * 10 + (u32)(src[j++] - '0');
                while (src[j] == ' ' || src[j] == '\t') j++;
                SourceLoc loc;
                loc.file = 0;
                loc.file_len = 0;
                if (src[j] == '"') {
                    j++;
                    const u8* f = &src[j];
                    while (src[j] != 0 && src[j] != '"' && src[j] != '\n') j++;
                    loc.file = f;
                    loc.file_len = (u32)(&src[j] - f);
                }
                // the marker's following line is logical line `n`; the offset's line is `lines_below`
                // physical lines below the marker (so `lines_below - 1` lines past line `n`).
                loc.line = n + (lines_below ? lines_below - 1 : 0);
                loc.col = (u32)(offset - line_begin) + 1;
                return loc;
            }
        }
        if (pos == 0) break;
        // step back to the start of the previous line
        size_t e = pos - 1;   // the '\n' terminating the previous line
        while (e > 0 && src[e - 1] != '\n') e--;
        pos = e;
        lines_below++;
    }
    // no linemarker found: the 1-based physical line number
    SourceLoc loc;
    loc.file = 0;
    loc.file_len = 0;
    loc.line = lines_below + 1;
    loc.col = (u32)(offset - line_begin) + 1;
    return loc;
}

extern const char embed_libc_c[];
extern size_t embed_libc_c_size;

extern const char* const win32_builtin_def[];   // the builtin Windows import libs, as embedded .def text
extern const size_t win32_builtin_def_count;

// report a parse error at `err_off` within preprocessed source `src`; `what`/`what_len` labels the
// source when no linemarker is present (the input file's path, or "libc")
static void report_parse_error(const u8* src, const char* err, size_t err_off, const char* what, size_t what_len)
{
    SourceLoc loc = get_ppd_source_loc(src, err_off);
    char buf[1000];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    if (loc.file) MUST(sink_put(&w, (const char*)loc.file, loc.file_len));
    else          MUST(sink_put(&w, what, what_len));
    MUST(SINK_LITERAL(&w, ":"));
    MUST(sink_format_u64(&w, loc.line));
    MUST(SINK_LITERAL(&w, ":"));
    MUST(sink_format_u64(&w, loc.col));
    MUST(SINK_LITERAL(&w, ": error: "));
    MUST(sink_put(&w, err, libc_strlen(err)));
    MUST(SINK_LITERAL(&w, "\n"));
    MUST(sink_flush(&w));
}

// report a failed file_open with the path and the OS error code
static void report_open_error(const char* kind, CliArg path, Error err)
{
    char buf[1000];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    MUST(SINK_LITERAL(&w, "cannot open "));
    MUST(sink_put(&w, kind, libc_strlen(kind)));
    MUST(SINK_LITERAL(&w, " '"));
    MUST(cli_put_arg(&w, path));
    MUST(SINK_LITERAL(&w, "': "));
    MUST(sink_format_error(&w, err));
    MUST(SINK_LITERAL(&w, "\n"));
    MUST(sink_flush(&w));
}

// open `path`, read it, and preprocess it -> src/src_size (one contiguous buffer). Also returns the
// input's path narrowed to ASCII with '/' separators (arena-allocated, sized exactly to the path --
// no PATH_MAX cap), via *out_path/*out_path_len: its `dir` prefix feeds relative #include and the whole
// path labels parse errors. Reports + returns 0 on open failure.
static Bool open_read_preprocess(
    CliArg path,
    const Defines* defines,
    Bool stdinc,
    const char** out_path, size_t* out_path_len,
    u8** src, size_t* src_size
) {
    File in;
    Error open_err = file_open_native(path.ptr, path.count, &in);
    if (open_err) {
        report_open_error("input file", path, open_err);
        return 0;
    }
    u8* content; size_t size;
    read_file(in, &content, &size);
    file_close(in);

    char* pathbuf = ARENA_ALLOC(&global_arena, path.count ? path.count : 1);
    for (size_t i = 0; i < path.count; i++) {
        char ch = (char)(u8)path.ptr[i];
        if (ch == '\\') ch = '/';
        pathbuf[i] = ch;
    }
    *out_path = pathbuf;
    *out_path_len = path.count;
    preprocess(content, size, pathbuf, path.count, defines, stdinc, src, src_size);
    return 1;
}

// open `path`, read+preprocess+parse it into `out`; reports and returns 0 on failure
static Bool parse_c_input(CliArg path, const Defines* defines, Bool stdinc, TranslationUnit* out)
{
    const char* pathbuf; size_t pn;
    u8* src; size_t src_size;
    if (!open_read_preprocess(path, defines, stdinc, &pathbuf, &pn, &src, &src_size)) return 0;

    const char* err; size_t err_off;
    if (!parse(src, src_size, out, &err, &err_off)) { report_parse_error(src, err, err_off, pathbuf, pn); return 0; }
    return 1;
}

// does the CLI path end in `.o` (a cc object file) rather than `.c`?
static Bool has_suffix(CliArg p, const char* suf, size_t suflen)
{
    if (p.count < suflen) return 0;
    for (size_t i = 0; i < suflen; i++)
        if (p.ptr[p.count - suflen + i] != (FilenameChar)suf[i]) return 0;
    return 1;
}

// cc's object format is its own (serialized IR, not COFF/ELF). `.ccobj` names it honestly; `.o`
// is also accepted because the wacc suite's gcc shim hardcodes it.
static Bool is_object_path(CliArg p)
{
    return has_suffix(p, ".o", 2) || has_suffix(p, ".ccobj", 6);
}

static Bool is_def_path(CliArg p) { return has_suffix(p, ".def", 4); }

static Bool parse_def_input(NameMap* import_map, SegList* import_list, const u8* content, size_t content_len)
{
    const char* dll = 0; u16 dll_len = 0;
    Bool in_exports = 0;
    size_t i = 0;
    while (i < content_len) {
        size_t ls = i;
        while (i < content_len && content[i] != '\n') i++;
        size_t eol = i;
        if (i < content_len) i++;
        if (eol > ls && content[eol - 1] == '\r') eol--;
        size_t p = ls;
        while (p < eol && (content[p] == ' ' || content[p] == '\t')) p++;
        if (p >= eol || content[p] == ';') continue;
        if (!in_exports) {
            if (eol - p >= 7 && libc_memcmp(content + p, "LIBRARY", 7) == 0) {
                size_t q = p + 7;
                while (q < eol && (content[q] == ' ' || content[q] == '\t' || content[q] == '"')) q++;
                size_t ds = q;
                while (q < eol && content[q] != ' ' && content[q] != '\t' && content[q] != '"') q++;
                size_t de = q;
                if (de - ds > 4) {   // strip a trailing ".dll" (any case) -- the loader appends it
                    const u8* s = content + de - 4;
                    if (s[0] == '.' && (s[1] | 0x20) == 'd' && (s[2] | 0x20) == 'l' && (s[3] | 0x20) == 'l') de -= 4;
                }
                dll = (const char*)(content + ds);
                dll_len = (u16)(de - ds);
            } else if (eol - p == 7 && libc_memcmp(content + p, "EXPORTS", 7) == 0) {
                in_exports = 1;
            }
            continue;
        }
        size_t te = p;
        while (te < eol && content[te] != ' ' && content[te] != '\t') te++;
        size_t ns = p;
        if (content[ns] == '@' && te - ns > 1) ns++;
        size_t d = te;
        while (d > ns && content[d - 1] >= '0' && content[d - 1] <= '9') d--;
        size_t ne = (d > ns && d < te && content[d - 1] == '@') ? d - 1 : te;
        if (ne <= ns) continue;
        if (!dll) { LOG_STRING("def file: EXPORTS before LIBRARY"); return 0; }
        Import imp;
        imp.name_ptr = (const char*)(content + ns);
        imp.name_len = (u16)(ne - ns);
        imp.dll_ptr = dll;
        imp.dll_len = dll_len;
        u32 idx = (u32)import_list->count;
        SEG_LIST_APPEND(Import, import_list, imp);
        name_map_put(import_map, content + ns, ne - ns, idx);
    }
    if (!dll) { LOG_STRING("def file has no LIBRARY line"); return 0; }
    return 1;
}

static void update_file_native(const FilenameChar* path, size_t path_len, u32 flags, const u8* data, size_t len)
{
    assert_native_path(path, path_len);
    File out;
    Error err = file_create_native(path, path_len, flags, &out);
    if (err) {
        char buf[1000];
        Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
        MUST(SINK_LITERAL(&w, "failed to create output file '"));
        MUST(sink_put_filename(&w, path, path_len));
        MUST(SINK_LITERAL(&w, "': error "));
        MUST(sink_format_error(&w, err));
        MUST(SINK_LITERAL(&w, "\n"));
        MUST(sink_flush(&w));
        process_exit(91);
    }
    size_t total = 0;
    while (total < len) {
        size_t written;
        Error error = file_write(out, data + total, len - total, &written);
        if (error) {
            char buf[1000];
            Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
            MUST(SINK_LITERAL(&w, "failed to write output file '"));
            MUST(sink_put_filename(&w, path, path_len));
            MUST(SINK_LITERAL(&w, "': error "));
            MUST(sink_format_error(&w, error));
            MUST(SINK_LITERAL(&w, "\n"));
            MUST(sink_flush(&w));
            process_exit(81);
        }
        total += written;
    }
    file_close(out);
}

static void update_file_dot_o(CliArg src, const u8* data, size_t len)
{
    if (src.count < 2 || src.ptr[src.count - 2] != '.' || src.ptr[src.count - 1] != 'c') {
        char buf[1000];
        Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
        MUST(SINK_LITERAL(&w, "-c input must be a .c file (or pass -o): '"));
        MUST(sink_put_filename(&w, src.ptr, src.count));
        MUST(SINK_LITERAL(&w, "'\n"));
        MUST(sink_flush(&w));
        process_exit(64);
    }
    ArenaPosition arena_pos = arena_position(&global_arena);
    FilenameChar* dot_o = ARENA_ALLOC(&global_arena, (src.count + 1) * sizeof(FilenameChar));
    libc_memcpy(dot_o, src.ptr, src.count * sizeof(FilenameChar));
    dot_o[src.count - 1] = 'o';
    dot_o[src.count] = 0;   // _native create needs a NUL terminator
    update_file_native(dot_o, src.count, 0, data, len);
    arena_reset(&global_arena, arena_pos);
}

// append a CLI arg to a growable list (used for the positional-input list; no fixed limit)
static void push_cli_arg(SegList* list, size_t* count, CliArg arg)
{
    SEG_LIST_APPEND(CliArg, list, arg);
    (*count)++;
}

void alloc_stats_dump(void);
void verify_int_sizes(void);

#if defined(_WIN32)
    #define MAIN_ARGS void
    #if __STDC_HOSTED__
        #define MAIN main
    #else
        #define MAIN crtmain
    #endif
#else
    #define MAIN main                    // hosted (CRT calls main), or freestanding Linux (_start calls main)
    #define MAIN_ARGS int argc, char** argv
#endif

#if HEDLEY_HAS_ATTRIBUTE(used) || HEDLEY_GCC_VERSION_CHECK(3,1,0)
    __attribute__((__used__))
#endif
int MAIN(MAIN_ARGS)
{
    verify_int_sizes();

    Bool link_libc = 1;
    Bool freestanding = 0;          // -ffreestanding: __STDC_HOSTED__ becomes 0 (like clang/gcc)
    Bool stdinc = 1;                // -nostdinc clears: stop resolving #include <...> from embedded headers
    Bool compile_only = 0;          // -c: compile each source to an object, do not link
    Bool preprocess_only = 0;       // -E: write the preprocessed source to stdout, do not compile
    CliArg out_path_native;
    Bool has_o = 0;
    SegList input_pages = SEG_LIST_INIT(CliArg);   // positional inputs: source (.c) or cc object (.o) files
    size_t input_count = 0;
    Target target = TARGET_HOST;
    NameMap import_map = name_map_init();
    SegList import_list = SEG_LIST_INIT(Import);

    {
        CliIterator cli_iterator;
#ifdef _WIN32
        cli_iterator_init(&cli_iterator);
#else
        (void)argc;
        cli_iterator_init(&cli_iterator, argv);
#endif
        while (1) {
            CliArg arg;
            if (!cli_next(&cli_iterator, &arg)) break;

            if (0) {
            } else if (CLI_ARG_MATCH(arg, "--alloc-stats")) {
                alloc_stats_enabled = 1;
            } else if (CLI_ARG_MATCH(arg, "-c")) {
                compile_only = 1;
            } else if (CLI_ARG_MATCH(arg, "-nostdlib")) {
                link_libc = 0;
            } else if (CLI_ARG_MATCH(arg, "-ffreestanding")) {
                freestanding = 1;
            } else if (CLI_ARG_MATCH(arg, "-nostdinc")) {
                stdinc = 0;
            } else if (CLI_ARG_MATCH(arg, "-E")) {
                preprocess_only = 1;
            } else if (CLI_ARG_MATCH(arg, "-v")) {
                // version/existence probe (the test driver runs `gcc -v` before using it)
                LOG_STRING("cc (self-contained)");
                return 0;
            } else if (CLI_ARG_MATCH(arg, "-D")) {
                // gcc-style macro definition. The only one the suite passes is SUPPRESS_WARNINGS,
                // which merely gates #pragma lines cc already ignores, so accept and drop it.
                CliArg ignored;
                cli_next(&cli_iterator, &ignored);
            } else if (arg.count >= 2 && arg.ptr[0] == '-' && arg.ptr[1] == 'l') {
                // ignore a -l<lib> link flag: cc has no external libraries to link against
            } else if (CLI_ARG_MATCH(arg, "-o")) {
                has_o = 1;
                if (!cli_next(&cli_iterator, &out_path_native)) {
                    LOG_STRING("-o requires an argument");
                    process_exit(-1);
                }
            } else if (CLI_ARG_MATCH(arg, "--target")) {
                CliArg name;
                if (!cli_next(&cli_iterator, &name)) {
                    LOG_STRING("--target requires an argument");
                    process_exit(-1);
                }
                if (0) {
                } else if (CLI_ARG_MATCH(name, "x86_64-windows")) {
                    target = TARGET_x86_64_windows;
                } else if (CLI_ARG_MATCH(name, "x86_64-linux")) {
                    target = TARGET_x86_64_linux;
                } else if (CLI_ARG_MATCH(name, "x86_64-darwin")) {
                    target = TARGET_x86_64_darwin;
                } else if (CLI_ARG_MATCH(name, "aarch64-windows")) {
                    target = TARGET_aarch64_windows;
                } else if (CLI_ARG_MATCH(name, "aarch64-linux")) {
                    target = TARGET_aarch64_linux;
                } else if (CLI_ARG_MATCH(name, "aarch64-darwin")) {
                    target = TARGET_aarch64_darwin;
                } else {
                    char stderr_buf[1000];
                    Sink stderr = sink_init(&stderr_vtable, stderr_buf, sizeof(stderr_buf));
                    MUST(SINK_LITERAL(&stderr, "unknown --target '"));
                    MUST(cli_put_arg(&stderr, name));
                    MUST(SINK_LITERAL(&stderr, "' (want {x86_64,aarch64}-{windows,linux,darwin})\n"));
                    MUST(sink_flush(&stderr));
                    return -1;
                }
            } else if (arg.count > 0 && arg.ptr[0] == '-') {
                char stderr_buf[1000];
                Sink stderr = sink_init(&stderr_vtable, stderr_buf, sizeof(stderr_buf));
                MUST(SINK_LITERAL(&stderr, "unknown option '"));
                MUST(cli_put_arg(&stderr, arg));
                MUST(SINK_LITERAL(&stderr, "'\n"));
                MUST(sink_flush(&stderr));
                return -1;
            } else {
                push_cli_arg(&input_pages, &input_count, arg);   // positional: a .c source or .o object
            }
        }
    }

    if (input_count == 0) {
        char stderr_buf[2048];
        Sink stderr = sink_init(&stderr_vtable, stderr_buf, sizeof(stderr_buf));
        MUST(SINK_LITERAL(&stderr,
            "usage: cc [options] <input.c | input.o> ...\n"
            "\n"
            "Options:\n"
            "  -o <path>       write output to <path> (an executable, or an object with -c)\n"
            "  -c              compile each source to a cc object file; do not link\n"
            "  -E              preprocess only; write the result to stdout\n"
            "  -nostdinc       do not resolve #include <...> from the built-in headers\n"
            "  -nostdlib       do not link the built-in libc\n"
            "  -ffreestanding  compile freestanding (__STDC_HOSTED__ == 0)\n"
            "  --target <t>    target {x86_64,aarch64}-{windows,linux,darwin} (default: host)\n"
            "  --alloc-stats   print allocator statistics on exit\n"
            "  -v              print the version and exit\n"
            "  -D <name>       accepted for gcc compatibility (ignored)\n"
            "  -l<lib>         accepted for gcc compatibility (cc links nothing external)\n"
        ));
        MUST(sink_flush(&stderr));
        return 2;
    }

    BuiltinDefineValues builtin_defines = {0};
    switch (target) {
    case TARGET_x86_64_windows: builtin_defines.os_win32 = ""; builtin_defines.arch_x86_64 = ""; break;
    case TARGET_x86_64_linux:   builtin_defines.os_linux = ""; builtin_defines.arch_x86_64 = ""; break;
    case TARGET_x86_64_darwin:  builtin_defines.os_mach = "";  builtin_defines.arch_x86_64 = ""; break;
    case TARGET_aarch64_windows: builtin_defines.os_win32 = ""; builtin_defines.arch_aarch64 = ""; break;
    case TARGET_aarch64_linux:   builtin_defines.os_linux = ""; builtin_defines.arch_aarch64 = ""; break;
    case TARGET_aarch64_darwin:  builtin_defines.os_mach = "";  builtin_defines.arch_aarch64 = ""; break;
    }
    builtin_defines.size_type    = "unsigned long";
    builtin_defines.int64_type   = "long long";
    builtin_defines.uint64_type  = "unsigned long long";
    builtin_defines.intptr_type  = "long long";
    builtin_defines.uintptr_type = "unsigned long long";
    builtin_defines.stdc_hosted  = freestanding ? "0" : "1";
    const char* custom_define_buf[8];
    Defines defines;
    defines.builtin = &builtin_defines;
    defines.custom = custom_define_buf;
    defines.custom_count = 0;

    // -E: write each input's preprocessed source straight to stdout (for debugging line numbers /
    // linemarkers against the original). preprocess() yields one contiguous buffer, so point the
    // stdout sink at it and flush -- the drain loops over the buffer, no copy or allocation.
    if (preprocess_only) {
        for (size_t i = 0; i < input_count; i++) {
            ArenaPosition pos = arena_position(&global_arena);
            CliArg input = SEG_LIST_VAL(CliArg, &input_pages, i);
            const char* pathbuf; size_t pn;
            u8* src; size_t src_size;
            if (!open_read_preprocess(input, &defines, stdinc, &pathbuf, &pn, &src, &src_size)) return -1;
            (void)pathbuf; (void)pn;
            Sink w = sink_init(&stdout_vtable, (char*)src, src_size);
            w.end = src_size;
            MUST(sink_flush(&w));
            arena_reset(&global_arena, pos);   // free this input's scratch before the next
        }
        return 0;
    }

    // -c: compile each source to its own object file (default name: the input with .c -> .o)
    if (compile_only) {
        for (size_t i = 0; i < input_count; i++) {
            CliArg input = SEG_LIST_VAL(CliArg, &input_pages, i);
            TranslationUnit tu;
            if (!parse_c_input(input, &defines, stdinc, &tu)) return -1;
            Pages image = PAGES_INIT();
            size_t image_size = obj_write(&tu, &image);
            if (has_o) {
                update_file_native(out_path_native.ptr, out_path_native.count, 0, image.ptr, image_size);
            } else {
                update_file_dot_o(input, image.ptr, image_size);
            }
        }
        alloc_stats_dump();
        return 0;
    }

    // link: parse each .c input and read each .o input into a TU, append libc, link, emit the exe
    TranslationUnit* tus;
    MUST(pages_alloc_fixed((void**)&tus, (input_count + 1) * sizeof(TranslationUnit)));   // inputs + libc
    size_t tu_count = 0;
    for (size_t i = 0; i < input_count; i++) {
        CliArg input = SEG_LIST_VAL(CliArg, &input_pages, i);
        if (is_def_path(input)) {
            u8* def_content;
            size_t def_content_len;
            {
                File in;
                Error e = file_open_native(input.ptr, input.count, &in);
                if (e) { report_open_error("def file", input, e); return 0; }
                read_file(in, &def_content, &def_content_len);
            }
            if (!parse_def_input(&import_map, &import_list, def_content, def_content_len))
                return -1;
        } else if (is_object_path(input)) {
            File in;
            Error open_err = file_open_native(input.ptr, input.count, &in);
            if (open_err) { report_open_error("object file", input, open_err); return -1; }
            u8* bytes; size_t len; read_file(in, &bytes, &len); file_close(in);
            obj_read(bytes, len, &tus[tu_count++]);
        } else {
            if (!parse_c_input(input, &defines, stdinc, &tus[tu_count++])) return -1;
        }
    }

    if (link_libc) {
        u8* libc_ppd; size_t libc_ppd_size;
        preprocess(
            (const unsigned char*)embed_libc_c, embed_libc_c_size,
            "<libc.c>", 8,
            &defines,
            1,   // stdinc: the built-in libc always resolves its own <...> headers, regardless of -nostdinc
            &libc_ppd, &libc_ppd_size
        );
        const char* libc_err; size_t libc_err_off;
        if (!parse(libc_ppd, libc_ppd_size, &tus[tu_count], &libc_err, &libc_err_off)) {
            report_parse_error(libc_ppd, libc_err, libc_err_off, "<libc.c>", 8);
            return -1;
        }
        tu_count++;
    }

    // output path: -o <name> (a NUL-terminated CLI arg), else the first input with its ".c" dropped.
    // The latter is a prefix of the input (its byte at [len] is '.', not a terminator), so it needs
    // its own NUL-terminated buffer for the _native create.
    const FilenameChar* exe_ptr; size_t exe_len;
    if (has_o) {
        exe_ptr = out_path_native.ptr; exe_len = out_path_native.count;
    } else {
        CliArg f = SEG_LIST_VAL(CliArg, &input_pages, 0);
        if (f.count < 2 || f.ptr[f.count - 2] != '.' || f.ptr[f.count - 1] != 'c') {
            LOG_STRING("input file must end in .c (or pass -o)");
            return -1;
        }
        size_t n = f.count - 2;
        FilenameChar* p = ARENA_ALLOC(&global_arena, (n + 1) * sizeof(FilenameChar));
        libc_memcpy(p, f.ptr, n * sizeof(FilenameChar));
        p[n] = 0;
        exe_ptr = p;
        exe_len = n;
    }

    Bool is_windows = target_is_windows(target);
    if (is_windows) {
        for (size_t i = 0; i < win32_builtin_def_count; i++) {
            const char* def = win32_builtin_def[i];
            if (!parse_def_input(&import_map, &import_list, (const u8*)def, libc_strlen(def))) {
                LOG_STRING("internal error: failed to parse a builtin .def");
                process_exit(70);
            }
        }
    }

    const char* exit_import = is_windows ? "RtlExitUserProcess" : 0;
    Program linked;
    if (!link_program(tus, tu_count, &import_map, &import_list, exit_import, &linked)) return -1;

    Pages image = PAGES_INIT();
    size_t image_size = emit_exe(target, &linked, &image);
    update_file_native(exe_ptr, exe_len, FILE_FLAG_EXECUTABLE, image.ptr, image_size);
    alloc_stats_dump();
    return 0;
}
