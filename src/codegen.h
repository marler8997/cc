#ifndef _CODEGEN_H
#define _CODEGEN_H

#include "arch.h"
#include "../ur/int.h"
#include "../ur/size_t.h"

typedef struct struct_Program Program;
typedef struct struct_Pages Pages;

// How a target's ABI shapes code generation: the calling convention (frame layout) and how
// a function exits the process. The container passes this as data; each arch backend turns
// it into concrete instructions, so the codegen never references PE/ELF specifics.
typedef enum {
    ABI_WIN64,   // Windows x64/ARM64: shadow space; exit by calling an imported function
    ABI_SYSV,    // System V / Linux: no shadow space; exit via syscall
} AbiKind;

typedef struct struct_Abi {
    AbiKind kind;
    u32 iat_base_rva;    // ABI_WIN64: RVA of the IAT; import i's slot is iat_base_rva + import_iat_slot[i]*8
    u32 data_base_rva;   // ABI_WIN64: RVA of .data; static i is at data_base_rva + i*4
    u32 code_base_rva;   // ABI_WIN64: RVA where the code is mapped (for rip-relative refs)
    const u32* import_iat_slot;  // ABI_WIN64: import index -> its IAT slot (DLLs are grouped with a
                                 // null terminator between them, so slots aren't dense). exit is slot 0.
    u32 exit_syscall;    // ABI_SYSV: the OS exit syscall number (Linux x86_64 = 60, macOS x86_64 = 0x2000001)
} Abi;

static inline size_t round_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

// Lower a function's IR into machine code for `arch` at the start of `code`; return its
// byte length. Dispatches to the per-arch backend. `func_offsets` (nullable, sized to func_id_count)
// is filled with each function's byte offset within `code`, or 0xFFFFFFFF if not emitted.
size_t gen_code(Arch, const Abi*, const Program*, Pages*, u32* func_offsets);

#endif // _CODEGEN_H
