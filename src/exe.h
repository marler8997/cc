#ifndef _EXE_H
#define _EXE_H

#include "../ur/size_t.h"
#include "target.h"

typedef struct struct_Program Program;
typedef struct struct_Pages Pages;

size_t emit_exe(Target, const Program*, Pages*);

#endif // _EXE_H
