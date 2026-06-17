#ifndef _IMPORTLIB_H
#define _IMPORTLIB_H

#include "../ur/int.h"
#include "../ur/size_t.h"

#define X_BUILTIN_IMPORT_LIBS \
    X(ntdll) \
    X(kernel32) \
    X(user32) \
    X(gdi32) \

typedef enum {
#define X(id) BUILTIN_IMPORT_LIB_##id,
    X_BUILTIN_IMPORT_LIBS
#undef X
} BuiltinImportLib;

extern const char* const win32_lib_name[];
int lookup_win32_import(const u8* s, size_t n);
BuiltinImportLib win32_import_lib(u16 import);
extern const char* const win32_import_name_table[];

#endif // _IMPORTLIB_H
