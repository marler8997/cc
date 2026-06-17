#ifndef _PARSE_H
#define _PARSE_H

#include "../ur/bool.h"
#include "../ur/int.h"
#include "../ur/size_t.h"

typedef struct struct_TranslationUnit TranslationUnit;

// Parse `src` (preprocessed, NUL-terminated at src[len]), emitting the program's IR
// into `out` (already initialized). Returns 1 on success. On the first syntax error
// returns 0 and sets *err_msg to a static description and *err_off to the byte offset.
Bool parse(const u8* src, size_t len, TranslationUnit* out, const char** err_msg, size_t* err_off);

#endif // _PARSE_H
