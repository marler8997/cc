#ifndef _IMPORT_H
#define _IMPORT_H

#include "../ur/int.h"

typedef struct structImport {
    const char* name_ptr;
    const char* dll_ptr;
    u16 name_len;
    u16 dll_len;
} Import;

#endif // _IMPORT_H
