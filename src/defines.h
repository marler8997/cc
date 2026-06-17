#ifndef _DEFINES_H
#define _DEFINES_H

#include "../ur/size_t.h"

typedef struct struct_BuiltinDefineValues BuiltinDefineValues;

typedef struct struct_Defines {
    // TODO: might use a map instead?
    const char* const* custom;
    size_t custom_count;
    const BuiltinDefineValues* builtin;
} Defines;

#endif // _DEFINES_H
