#include "link.h"

#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "../ur/filesink.h"
#include "../ur/log.h"
#include "../ur/mem.h"
#include "../ur/processexit.h"
#include "../ur/sink.h"
#include "import.h"
#include "ir.h"
#include "namemap.h"
#include "program.h"

// A symbol in the linked program's namespace -- one per unique name across all input TUs. This
// is the shared link-time structure, so unlike a per-TU IrGlobal its name is an absolute pointer
// (resolved through the defining/referencing TU's ppd_source); matching is by these bytes.
typedef struct {
    const u8* name;
    size_t name_len;
    GlobalKind kind;
    u32 param_count;   // from the first occurrence; every other must agree
    const u8* init;    // GLOBAL_STATIC: initializer image (init_len bytes); NULL => zero-filled
    u32 init_len;      // GLOBAL_STATIC: the .data slot byte size
    const StaticReloc* relocs;   // GLOBAL_STATIC: function-address fixups (targets are TU-local ids)
    u32 reloc_count;
    Bool defined;
    Bool used;
    Bool internal;           // no linkage (a local static): never merged with another symbol
    Bool is_import;          // resolved to an ImportLib export rather than a TU definition
    Import import;
    u32 src_tu;        // TU that defines it (valid when `defined`)
    u32 src_local_id;  // its func_id within that TU (valid when `defined`)
} GlobalSym;

static Bool resolve_import(NameMap* import_map, SegList* import_list, const u8* name, size_t name_len, Import* out_import)
{
    u32 import_index;
    if (name_map_get(import_map, name, name_len, &import_index)) {
        *out_import = SEG_LIST_VAL(Import, import_list, import_index);   // a custom .def import
        return 1;
    }
    return 0;
}

static void report(const char* kind, const u8* name, size_t len)
{
    char buf[1000];
    Sink w = sink_init(&stderr_vtable, buf, sizeof(buf));
    MUST(sink_put(&w, kind, libc_strlen(kind)));
    MUST(sink_put(&w, (const char*)name, len));
    MUST(SINK_LITERAL(&w, "\n"));
    MUST(sink_flush(&w));
}

Bool link_program(
    TranslationUnit* tus, size_t tu_count,
    NameMap* import_map, SegList* import_list,
    const char* exit_import, Program* out
) {
    // one global slot per input sym (+1 for a possibly injected exit import); a localmap slot per func_id
    size_t max_globals = 1;
    size_t total_locals = 0;
    for (size_t t = 0; t < tu_count; t++) {
        max_globals += tus[t].global_count;
        total_locals += tus[t].func_id_count;
    }

    ArenaPosition arena_pos = arena_position(&global_arena);
    GlobalSym* g = ARENA_ALLOC(&global_arena, max_globals * sizeof(GlobalSym));
    size_t gn = 0;

    // localmap[base[t] + local_id] -> global id, so a TU's FUNCALL.callee can be rewritten
    u32* localmap = ARENA_ALLOC(&global_arena, (total_locals ? total_locals : 1) * sizeof(u32));
    size_t* base = ARENA_ALLOC(&global_arena, (tu_count ? tu_count : 1) * sizeof(size_t));
    {
        size_t b = 0;
        for (size_t t = 0; t < tu_count; t++) { base[t] = b; b += tus[t].func_id_count; }
    }

    // resolve: intern every TU's symbols by name, merging declarations with their definition. A
    // by-name hash makes interning O(1) per symbol (the linear scan was the linker's O(n^2)).
    NameMap gmap = name_map_init();
    for (size_t t = 0; t < tu_count; t++) {
        TranslationUnit* tu = &tus[t];
        for (u32 i = 0; i < tu->func_id_count; i++) {
            IrGlobal* s = tu_global_at(tu, i);
            const u8* nm = tu->ppd_source + s->name_off;
            size_t gi = (size_t)-1;
            u32 found_gi;
            if (!s->internal && name_map_get(&gmap, nm, s->name_len, &found_gi)) gi = found_gi;
            if (gi == (size_t)-1) {
                gi = gn++;
                g[gi].name = nm;
                g[gi].name_len = s->name_len;
                g[gi].kind = s->kind;
                g[gi].param_count = s->param_count;
                g[gi].init = s->init;
                g[gi].init_len = s->init_len;
                g[gi].relocs = s->relocs;
                g[gi].reloc_count = s->reloc_count;
                g[gi].defined = 0;
                g[gi].used = 0;
                g[gi].internal = s->internal;
                g[gi].is_import = 0;
                if (!s->internal) name_map_put(&gmap, nm, s->name_len, (u32)gi);
            } else if (s->param_count != g[gi].param_count) {
                report("conflicting parameter count for: ", nm, s->name_len);
                return 0;
            }
            if (s->defined) {
                if (g[gi].defined) { report("duplicate symbol: ", nm, s->name_len); return 0; }
                g[gi].defined = 1;
                g[gi].init = s->init;       // the initializer comes from the definition, not whichever
                g[gi].init_len = s->init_len;  // declaration was interned first (a bare `extern` has init 0)
                g[gi].relocs = s->relocs;
                g[gi].reloc_count = s->reloc_count;
                g[gi].src_tu = (u32)t;
                g[gi].src_local_id = i;
            }
            localmap[base[t] + i] = (u32)gi;
        }
    }

    // resolve names against the import libs. Functions and imports are one namespace, so a name
    // that is both defined by a TU and provided by an import lib is a conflict.
    for (size_t gi = 0; gi < gn; gi++) {
        if (g[gi].internal) continue;   // a local static is never an import
        if (!resolve_import(import_map, import_list, g[gi].name, g[gi].name_len, &g[gi].import)) continue;
        if (g[gi].defined) { report("symbol both defined and imported: ", g[gi].name, g[gi].name_len); return 0; }
        g[gi].is_import = 1;
    }

    // find the TU that defines main (its main_func_id is a real id, not the no-main sentinel)
    u32 main_global = 0;
    Bool have_main = 0;
    for (size_t t = 0; t < tu_count; t++) {
        if (tus[t].main_func_id < tus[t].func_id_count) {
            main_global = localmap[base[t] + tus[t].main_func_id];
            have_main = 1;
            break;
        }
    }
    if (!have_main) { report("undefined symbol: ", (const u8*)"main", 4); return 0; }

    // the runtime always exits through the exit import; ensure a used global exists for it
    size_t exit_global = (size_t)-1;
    if (exit_import) {
        u32 found_gi;
        if (name_map_get(&gmap, (const u8*)exit_import, libc_strlen(exit_import), &found_gi)) exit_global = found_gi;
        if (exit_global == (size_t)-1) {
            exit_global = gn++;
            g[exit_global].name = (const u8*)exit_import;
            g[exit_global].name_len = libc_strlen(exit_import);
            g[exit_global].param_count = 0;
            g[exit_global].defined = 0;
            g[exit_global].used = 0;
            g[exit_global].internal = 0;
            if (!resolve_import(import_map, import_list, g[exit_global].name, g[exit_global].name_len, &g[exit_global].import)) {
                LOG_STRING("internal error: exit import not in any import lib");
                process_exit(70);
            }
            g[exit_global].is_import = 1;
        }
        g[exit_global].used = 1;
    }

    // index global id -> its defined IrFunc body, so the sweep resolves bodies in O(1)
    IrFunc** gfunc = ARENA_ALLOC(&global_arena, (gn ? gn : 1) * sizeof(IrFunc*));
    for (size_t t = 0; t < tu_count; t++) {
        TranslationUnit* tu = &tus[t];
        for (size_t i = 0; i < tu->func_count; i++) {
            IrFunc* f = tu_at(tu, i);
            gfunc[localmap[base[t] + f->func_id]] = f;
        }
    }

    // mark-sweep DCE: from main, follow each used function's call/static edges. A worklist (each
    // global enqueued exactly once, when it first becomes used) keeps this linear in symbols+edges;
    // a fixpoint re-scan would be O(n^3) on a deep call chain.
    u32* worklist = ARENA_ALLOC(&global_arena, (gn ? gn : 1) * sizeof(u32));
    size_t wn = 0;
    g[main_global].used = 1;
    worklist[wn++] = main_global;
    while (wn) {
        u32 gi = worklist[--wn];
        if (!g[gi].defined) continue;
        if (g[gi].kind == GLOBAL_FUNC) {
            IrFunc* f = gfunc[gi];
            for (u32 ii = 0; ii < f->instr_count; ii++) {
                IrInstr* in = ir_instr_at(f, ii);
                // a static reference (LOAD/STORE_STATIC) or a function-address (ADDROF_FUNC) marks its
                // target used, like a call
                if (in->kind != IR_FUNCALL && in->kind != IR_ADDROF_FUNC && in->kind != IR_LOAD_STATIC && in->kind != IR_STORE_STATIC && in->kind != IR_ADDROF_STATIC) continue;
                u32 cg = localmap[base[g[gi].src_tu] + in->u.fun.callee];
                if (!g[cg].used) { g[cg].used = 1; worklist[wn++] = cg; }
            }
        } else {   // GLOBAL_STATIC: a function whose address sits in this static's image is used
            for (u32 r = 0; r < g[gi].reloc_count; r++) {
                u32 cg = localmap[base[g[gi].src_tu] + g[gi].relocs[r].target];
                if (!g[cg].used) { g[cg].used = 1; worklist[wn++] = cg; }
            }
        }
    }

    // a reachable reference that nothing defines and no import lib provides is an error
    Bool failed = 0;
    for (size_t gi = 0; gi < gn; gi++) {
        if (g[gi].used && !g[gi].defined && !g[gi].is_import) { report("undefined symbol: ", g[gi].name, g[gi].name_len); failed = 1; }
    }
    if (failed) return 0;

    // emit the merged program: global ids are the func_id space; names are spent, not carried
    program_init(out, (u32)gn);
    out->main_func_id = main_global;
    for (size_t gi = 0; gi < gn; gi++) program_set_used(out, (u32)gi, g[gi].used);

    // record the used imports, exit import first so codegen reaches it at a fixed slot
    if (exit_import) {
        u32 idx = program_add_import(out, g[exit_global].import);
        program_set_import_of(out, (u32)exit_global, idx);
    }
    for (size_t gi = 0; gi < gn; gi++) {
        if (!g[gi].used || !g[gi].is_import) continue;
        if (exit_import && gi == exit_global) continue;
        u32 idx = program_add_import(out, g[gi].import);
        program_set_import_of(out, (u32)gi, idx);
    }

    // lay out the used statics in .data (pass 1), then resolve each static's fixups (pass 2, once every
    // static has a .data offset). A fixup naming a function becomes a .data->code reloc; one naming
    // another static becomes a .data->.data reloc. (.data offset = the static's base + the fixup offset.)
    for (size_t gi = 0; gi < gn; gi++) {
        if (!g[gi].used || g[gi].kind != GLOBAL_STATIC) continue;
        u32 idx = g[gi].init ? program_add_static_bytes(out, g[gi].init, g[gi].init_len)
                             : program_add_static_zeros(out, g[gi].init_len);
        program_set_static_of(out, (u32)gi, idx);
    }
    for (size_t gi = 0; gi < gn; gi++) {
        if (!g[gi].used || g[gi].kind != GLOBAL_STATIC) continue;
        u32 idx = program_static_of(out, (u32)gi);
        for (u32 r = 0; r < g[gi].reloc_count; r++) {
            u32 target = localmap[base[g[gi].src_tu] + g[gi].relocs[r].target];
            u32 data_off = idx + g[gi].relocs[r].offset;
            if (g[target].kind == GLOBAL_STATIC) program_add_static_reloc(out, data_off, program_static_of(out, target), 1);
            else                                 program_add_static_reloc(out, data_off, target, 0);
        }
    }

    for (size_t gi = 0; gi < gn; gi++) {
        if (!g[gi].defined || g[gi].kind != GLOBAL_FUNC) continue;
        IrFunc* f = gfunc[gi];
        for (u32 ii = 0; ii < f->instr_count; ii++) {
            IrInstr* in = ir_instr_at(f, ii);
            // rewrite every symbol reference (calls, static accesses, function addresses) to its global id
            if (in->kind == IR_FUNCALL || in->kind == IR_ADDROF_FUNC || in->kind == IR_LOAD_STATIC || in->kind == IR_STORE_STATIC || in->kind == IR_ADDROF_STATIC)
                in->u.fun.callee = localmap[base[g[gi].src_tu] + in->u.fun.callee];
        }
        IrFunc nf = *f;   // shares the TU's lists (pointer + ranges); the inputs are spent by linking
        nf.func_id = (u32)gi;
        program_add(out, nf);
    }

    arena_reset(&global_arena, arena_pos);
    return 1;
}
