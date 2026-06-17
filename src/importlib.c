#include "importlib.h"

const char* const win32_lib_name[] = {
#define X(name) #name,
    X_BUILTIN_IMPORT_LIBS
#undef X
};
