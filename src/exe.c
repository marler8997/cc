#include "exe.h"

#include "arch.h"
#include "../ur/log.h"
#include "../ur/processexit.h"

size_t emit_pe(Arch, const Program*, Pages*);
size_t emit_elf(Arch, const Program*, Pages*);
size_t emit_macho(Arch, const Program*, Pages*);

size_t emit_exe(Target target, const Program* prog, Pages* image)
{
    switch (target) {
        case TARGET_x86_64_windows:  return emit_pe(ARCH_x86_64, prog, image);
        case TARGET_aarch64_windows: return emit_pe(ARCH_aarch64, prog, image);
        case TARGET_x86_64_linux:    return emit_elf(ARCH_x86_64, prog, image);
        case TARGET_x86_64_darwin:   return emit_macho(ARCH_x86_64, prog, image);
        default:
            LOG_STRING("emit_exe: unsupported target");
            process_exit(70);
            return 0;   // unreachable
    }
}
