#include "codegen.h"

#include "../ur/log.h"
#include "../ur/processexit.h"
#include "cursor.h"
#include "ir.h"

// The aarch64 backend predates the multi-function program structure and hasn't been ported
// yet; it fails loud rather than emitting wrong code. (x86_64 is the tested target.)
size_t gen_code_aarch64(const Abi* abi, const Program* prog, Pages* code, u32* func_offsets)
{
    (void)abi; (void)prog; (void)code; (void)func_offsets;
    LOG_STRING("codegen_aarch64: not implemented");
    process_exit(71);
    return 0;
}
