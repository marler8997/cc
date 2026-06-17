#ifndef _NAMEMAP_H
#define _NAMEMAP_H

#include "../ur/bool.h"
#include "../ur/int.h"
#include "../ur/size_t.h"

// Open-addressing hash from a name (a byte span) to a u32 id. Used to index the parser's symbol
// table and the linker's global table so resolving a name is O(1) instead of a linear scan. Keys
// are borrowed pointers into stable source (the names never move while the map is live).
typedef struct { const u8* name; size_t len; u32 value; } NameSlot;
typedef struct struct_NameMap {
    NameSlot* slots;   // power-of-two array; an empty slot has name == 0
    size_t cap;
    size_t count;
} NameMap;

NameMap name_map_init(void);
Bool name_map_get(const NameMap*, const u8* name, size_t len, u32* out_value);   // 1 + *out on hit
void name_map_put(NameMap*, const u8* name, size_t len, u32 value);              // insert or update

#endif // _NAMEMAP_H
