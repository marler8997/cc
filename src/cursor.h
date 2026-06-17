#ifndef _CURSOR_H
#define _CURSOR_H

#include "../ur/size_t.h"
#include "../ur/int.h"
#include "../ur/pages.h"

// A write cursor over a growable region, used by both the exe-file emitters (headers) and
// the code generators (instructions). Positions may be revisited (seek), so the backing
// store must be random-access; Pages grows in place without a fixed ceiling.
typedef struct {
    Pages* pages;
    size_t pos;
} Cursor;

void cursor_ensure(Cursor* c, size_t need);

static inline void put8(Cursor* c, u8 v)   { cursor_ensure(c, c->pos + 1); c->pages->ptr[c->pos++] = v; }
static inline void put16(Cursor* c, u16 v) { put8(c, (u8)v); put8(c, (u8)(v >> 8)); }
static inline void put32(Cursor* c, u32 v) { put16(c, (u16)v); put16(c, (u16)(v >> 16)); }
static inline void put64(Cursor* c, u64 v) { put32(c, (u32)v); put32(c, (u32)(v >> 32)); }
static inline void put_bytes(Cursor* c, const char* s, size_t n) { for (size_t k = 0; k < n; k++) put8(c, (u8)s[k]); }
static inline void seek(Cursor* c, size_t pos) { cursor_ensure(c, pos); c->pos = pos; }

#endif // _CURSOR_H
