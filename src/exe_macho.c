#include "../ur/log.h"
#include "../ur/processexit.h"
#include "arch.h"
#include "codegen.h"
#include "cursor.h"

// Minimal static x86_64 Mach-O: __PAGEZERO + __TEXT (mapping the whole file) + LC_UNIXTHREAD.
// No dyld and no imports -- the program enters at the code directly (rip set by the thread state)
// and exits via a raw macOS syscall, so nothing here needs the dynamic linker or libSystem.
//
// Apple Silicon runs unsigned x86_64 Mach-O under Rosetta, so no code signature is emitted.
#define MACHO_VM_BASE  0x100000000ull   // __TEXT vmaddr; __PAGEZERO occupies [0, this)
#define MACHO_HDR      32               // mach_header_64
#define MACHO_SEG      72               // segment_command_64 with no sections
#define MACHO_THREAD   (16 + 21 * 8)    // LC_UNIXTHREAD: cmd/cmdsize/flavor/count + x86_THREAD_STATE64

// write a 16-byte Mach-O segment/section name, NUL-padded
static void put_name16(Cursor* c, const char* name)
{
    size_t i = 0;
    while (name[i]) { put8(c, (u8)name[i]); i++; }
    while (i < 16) { put8(c, 0); i++; }
}

size_t emit_macho(Arch arch, const Program* prog, Pages* image)
{
    if (arch != ARCH_x86_64) { LOG_STRING("emit_macho: unsupported arch"); process_exit(70); return 0; }

    // System V calling convention (same as Linux); exit via the macOS Unix-class exit syscall.
    Abi abi = { ABI_SYSV, 0, 0, 0, 0, 0x2000001 };
    Pages code = PAGES_INIT();
    size_t code_len = gen_code(arch, &abi, prog, &code, 0);

    size_t cmds = MACHO_SEG * 2 + MACHO_THREAD;     // __PAGEZERO, __TEXT, LC_UNIXTHREAD
    size_t code_off = MACHO_HDR + cmds;             // code follows the header + load commands
    size_t total = code_off + code_len;
    u64 entry = MACHO_VM_BASE + code_off;

    Cursor c = { image, 0 };
    cursor_ensure(&c, total);
    for (size_t k = 0; k < total; k++) image->ptr[k] = 0;

    // ---- mach_header_64 ----
    put32(&c, 0xFEEDFACF);              // magic = MH_MAGIC_64
    put32(&c, 0x01000007);              // cputype = CPU_TYPE_X86_64
    put32(&c, 0x00000003);              // cpusubtype = CPU_SUBTYPE_X86_64_ALL
    put32(&c, 2);                       // filetype = MH_EXECUTE
    put32(&c, 3);                       // ncmds
    put32(&c, (u32)cmds);               // sizeofcmds
    put32(&c, 1);                       // flags = MH_NOUNDEFS
    put32(&c, 0);                       // reserved

    // ---- LC_SEGMENT_64 __PAGEZERO: [0, VM_BASE), no file content (traps NULL derefs) ----
    put32(&c, 0x19);                    // LC_SEGMENT_64
    put32(&c, MACHO_SEG);               // cmdsize
    put_name16(&c, "__PAGEZERO");
    put64(&c, 0);                       // vmaddr
    put64(&c, MACHO_VM_BASE);           // vmsize
    put64(&c, 0);                       // fileoff
    put64(&c, 0);                       // filesize
    put32(&c, 0);                       // maxprot = VM_PROT_NONE
    put32(&c, 0);                       // initprot = VM_PROT_NONE
    put32(&c, 0);                       // nsects
    put32(&c, 0);                       // flags

    // ---- LC_SEGMENT_64 __TEXT: maps the whole file (header + code), read+execute ----
    put32(&c, 0x19);                    // LC_SEGMENT_64
    put32(&c, MACHO_SEG);               // cmdsize
    put_name16(&c, "__TEXT");
    put64(&c, MACHO_VM_BASE);           // vmaddr
    put64(&c, total);                   // vmsize
    put64(&c, 0);                       // fileoff
    put64(&c, total);                   // filesize
    put32(&c, 5);                       // maxprot = VM_PROT_READ | VM_PROT_EXECUTE
    put32(&c, 5);                       // initprot = VM_PROT_READ | VM_PROT_EXECUTE
    put32(&c, 0);                       // nsects (header and code share the segment; no section needed)
    put32(&c, 0);                       // flags

    // ---- LC_UNIXTHREAD: initial register state; rip = entry, everything else zero ----
    put32(&c, 0x5);                     // LC_UNIXTHREAD
    put32(&c, MACHO_THREAD);            // cmdsize
    put32(&c, 4);                       // flavor = x86_THREAD_STATE64
    put32(&c, 42);                      // count = 42 u32s (sizeof x86_thread_state64_t / 4)
    for (int k = 0; k < 21; k++) put64(&c, k == 16 ? entry : 0);   // rip is register index 16

    // ---- code ----
    seek(&c, code_off);
    for (size_t k = 0; k < code_len; k++) put8(&c, code.ptr[k]);

    return total;
}
