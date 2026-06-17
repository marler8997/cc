#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "../ur/log.h"
#include "../ur/processexit.h"
#include "arch.h"
#include "codegen.h"
#include "cursor.h"
#include "program.h"

// Static ELF: a PT_LOAD (R+X) mapping the headers and code, and -- when the program has file-scope
// statics -- a second PT_LOAD (R+W) for .data, floated onto the next page past the code. The code
// can span any number of pages.
#define ELF_BASE    0x400000ull
#define ELF_EHDR    64
#define ELF_PHDR    56
#define ELF_PAGE    0x1000

size_t emit_elf(Arch arch, const Program* prog, Pages* image)
{
    u16 machine;
    switch (arch) {
        case ARCH_x86_64:  machine = 0x3E; break;   // EM_X86_64
        case ARCH_aarch64: machine = 0xB7; break;   // EM_AARCH64
        default: LOG_STRING("emit_elf: unsupported arch"); process_exit(70); return 0;
    }

    // System V ABI: no shadow space, exit via syscall (no imports to resolve). Use exit_group, not
    // plain exit: exit (60) terminates only the calling thread, and its status isn't reported to
    // waiters as reliably as exit_group (231), which terminates the whole process.
    u32 exit_syscall = (arch == ARCH_x86_64) ? 231 : 94;   // Linux __NR_exit_group (x86_64 / aarch64)

    u32 ndata = program_static_size(prog);
    Bool have_data = ndata > 0;
    u32 phnum = have_data ? 2 : 1;
    u32 code_off = ELF_EHDR + phnum * ELF_PHDR;           // file offset == RVA of the code

    // ---- pass 1: measure the code. Its length is independent of the .data RVA (rip-relative refs
    //      encode a fixed 32-bit displacement), so a provisional data base suffices. ----
    Abi probe = { ABI_SYSV, 0, 0, code_off, 0, exit_syscall };
    Pages probe_code = PAGES_INIT();
    size_t code_len = gen_code(arch, &probe, prog, &probe_code, 0);

    // .data floats onto the next page past the code; its file offset equals its RVA, so the RW
    // segment's p_vaddr (ELF_BASE + data_off) stays congruent to p_offset modulo the page size.
    u32 data_off = (u32)round_up(code_off + code_len, ELF_PAGE);
    u32 data_vsize = ndata;

    // ---- pass 2: real code with the static base resolved (only needed when there are statics) ----
    const u8* code = probe_code.ptr;
    Pages code_pages = PAGES_INIT();
    ArenaPosition apos = arena_position(&global_arena);   // func_offsets is scratch; freed before return
    u32* func_offsets = 0;   // per-func code offset, for resolving .data->code static relocations
    if (have_data) {
        func_offsets = ARENA_ALLOC(&global_arena, (prog->func_id_count ? prog->func_id_count : 1) * sizeof(u32));
        Abi abi = { ABI_SYSV, 0, data_off, code_off, 0, exit_syscall };
        size_t code_len2 = gen_code(arch, &abi, prog, &code_pages, func_offsets);
        ASSERT(code_len2 == code_len);
        code = code_pages.ptr;
    }

    size_t total = have_data ? (data_off + data_vsize) : (code_off + code_len);

    Cursor c = { image, 0 };
    cursor_ensure(&c, total);
    for (size_t k = 0; k < total; k++) image->ptr[k] = 0;

    // ---- ELF header ----
    put8(&c, 0x7F); put_bytes(&c, "ELF", 3);   // e_ident magic
    put8(&c, 2);                                 // EI_CLASS = ELFCLASS64
    put8(&c, 1);                                 // EI_DATA = ELFDATA2LSB
    put8(&c, 1);                                 // EI_VERSION = EV_CURRENT
    put8(&c, 0);                                 // EI_OSABI = ELFOSABI_NONE
    put64(&c, 0);                                // EI_ABIVERSION + padding

    put16(&c, 2);                                // e_type = ET_EXEC
    put16(&c, machine);                          // e_machine
    put32(&c, 1);                                // e_version = EV_CURRENT
    put64(&c, ELF_BASE + code_off);              // e_entry
    put64(&c, ELF_EHDR);                         // e_phoff
    put64(&c, 0);                                // e_shoff (no section headers)
    put32(&c, 0);                                // e_flags
    put16(&c, ELF_EHDR);                         // e_ehsize
    put16(&c, ELF_PHDR);                         // e_phentsize
    put16(&c, (u16)phnum);                       // e_phnum
    put16(&c, 0x40);                             // e_shentsize
    put16(&c, 0);                                // e_shnum
    put16(&c, 0);                                // e_shstrndx = SHN_UNDEF

    // ---- Program header 1: PT_LOAD (R+X) mapping the headers and code ----
    u64 text_span = code_off + code_len;
    put32(&c, 1);                                // p_type = PT_LOAD
    put32(&c, 5);                                // p_flags = PF_R | PF_X
    put64(&c, 0);                                // p_offset
    put64(&c, ELF_BASE);                         // p_vaddr
    put64(&c, ELF_BASE);                         // p_paddr
    put64(&c, text_span);                        // p_filesz
    put64(&c, text_span);                        // p_memsz
    put64(&c, ELF_PAGE);                         // p_align

    // ---- Program header 2: PT_LOAD (R+W) for .data (statics) ----
    if (have_data) {
        put32(&c, 1);                            // p_type = PT_LOAD
        put32(&c, 6);                            // p_flags = PF_R | PF_W
        put64(&c, data_off);                     // p_offset
        put64(&c, ELF_BASE + data_off);          // p_vaddr
        put64(&c, ELF_BASE + data_off);          // p_paddr
        put64(&c, data_vsize);                   // p_filesz
        put64(&c, data_vsize);                   // p_memsz
        put64(&c, ELF_PAGE);                     // p_align
    }

    // ---- code ----
    seek(&c, code_off);
    for (size_t k = 0; k < code_len; k++) put8(&c, code[k]);

    // ---- .data: the static variables' initializer bytes, then the function/static-address fixups.
    //      ELF is non-PIE (ET_EXEC at a fixed ELF_BASE), so each fixup is just the target's absolute
    //      virtual address written in place -- no .reloc/dynamic relocations needed. ----
    if (have_data) {
        seek(&c, data_off);
        for (u32 i = 0; i < ndata; i++) put8(&c, program_static_byte(prog, i));
        STABLE_LIST_FOREACH(it, &prog->static_relocs) {
            ProgramReloc* r = (ProgramReloc*)it.elem;
            seek(&c, data_off + r->data_offset);
            if (r->to_static) {
                u64 addend = 0;   // the slot holds a byte offset into the target static (e.g. &arr[1]); add it in
                for (int k = 0; k < 8; k++) addend |= (u64)program_static_byte(prog, r->data_offset + k) << (k * 8);
                put64(&c, ELF_BASE + data_off + r->target + addend);  // .data->.data: another static's address (+offset)
            } else {
                u32 fo = func_offsets[r->target];
                if (fo == 0xFFFFFFFFu) { LOG_STRING("exe_elf: static relocation targets an unemitted function"); process_exit(72); }
                put64(&c, ELF_BASE + code_off + fo);                 // .data->code: a function's address
            }
        }
    }

    arena_reset(&global_arena, apos);
    return total;
}
