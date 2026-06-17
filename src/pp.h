#ifndef _PP_H
#define _PP_H

#include "../ur/bool.h"
#include "../ur/int.h"
#include "../ur/size_t.h"

typedef struct struct_Defines Defines;

void preprocess(
    const u8* src, size_t len,
    const char* file, size_t file_len,
    const Defines* defines,
    Bool stdinc,
    u8** out, size_t* out_len
);

#endif // _PP_H
