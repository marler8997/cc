#ifndef _BUILTINDEFINES_H
#define _BUILTINDEFINES_H

#include "../ur/int.h"
#include "../ur/size_t.h"

#define X_BUILTIN_VALUE_DEFINES \
    X(size_type,    "__SIZE_TYPE__") \
    X(int64_type,   "__INT64_TYPE__") \
    X(uint64_type,  "__UINT64_TYPE__") \
    X(intptr_type,  "__INTPTR_TYPE__") \
    X(uintptr_type, "__UINTPTR_TYPE__") \
    X(stdc_hosted,  "__STDC_HOSTED__") \
    X(os_win32,     "_WIN32") \
    X(os_linux,     "__linux__") \
    X(os_mach,      "__MACH__") \
    X(arch_x86_64,  "__x86_64__") \
    X(arch_aarch64, "__aarch64__") \

#define X_BUILTIN_SPECIAL_DEFINES \
    X(line,         "__LINE__") \
    X(file,         "__FILE__") \
    X(has_builtin,  "__has_builtin") \

typedef enum {
#define X(id, spelling) BUILTIN_DEFINE_##id,
    X_BUILTIN_VALUE_DEFINES
    X_BUILTIN_SPECIAL_DEFINES
#undef X
} BuiltinDefine;

typedef enum {
#define X(id, spelling) BUILTIN_VALUE_DEFINE_##id,
    X_BUILTIN_VALUE_DEFINES
#undef X
} BuiltinValueDefine;

int lookup_builtin_define(const u8* s, size_t n);

typedef struct struct_BuiltinDefineValues {
#define X(id, spelling) const char* id;
    X_BUILTIN_VALUE_DEFINES
#undef X
} BuiltinDefineValues;

static inline const char* builtin_define_value(const BuiltinDefineValues* v, BuiltinValueDefine d)
{
    return ((const char* const*)v)[d];
}

#endif // _BUILTINDEFINES_H
