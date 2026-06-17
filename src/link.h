#ifndef _LINK_H
#define _LINK_H

#include "../ur/bool.h"
#include "../ur/size_t.h"

typedef struct struct_TranslationUnit TranslationUnit;
typedef struct struct_NameMap NameMap;
typedef struct struct_SegList SegList;
typedef struct struct_Program Program;

Bool link_program(
    TranslationUnit* tus, size_t tu_count,
    NameMap* import_map, SegList* import_list,
    const char* exit_import, Program* out
);

#endif // _LINK_H
