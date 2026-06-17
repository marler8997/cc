#include "codegen.h"

#include "arch.h"
#include "../ur/log.h"
#include "../ur/processexit.h"

size_t gen_code_x86_64(const Abi*, const Program*, Pages*, u32* func_offsets);
size_t gen_code_aarch64(const Abi*, const Program*, Pages*, u32* func_offsets);

size_t gen_code(Arch arch, const Abi* abi, const Program* prog, Pages* code, u32* func_offsets)
{
    switch (arch) {
        case ARCH_x86_64:  return gen_code_x86_64(abi, prog, code, func_offsets);
        case ARCH_aarch64: return gen_code_aarch64(abi, prog, code, func_offsets);
        default:
            LOG_STRING("gen_code: unsupported arch");
            process_exit(70);
            return 0;   // unreachable
    }
}
