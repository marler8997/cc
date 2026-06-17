#include "namemap.h"

#include "../ur/abortmacros.h"
#include "../ur/pages.h"

static u64 name_hash(const u8* s, size_t n)
{
    u64 h = 1469598103934665603ULL;            // FNV-1a
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ULL; }
    return h;
}

static Bool slot_eq(const NameSlot* s, const u8* name, size_t len)
{
    if (s->len != len) return 0;
    for (size_t i = 0; i < len; i++) if (s->name[i] != name[i]) return 0;
    return 1;
}

NameMap name_map_init(void)
{
    NameMap m;
    m.slots = 0;
    m.cap = 0;
    m.count = 0;
    return m;
}

static void name_map_grow(NameMap* m)
{
    size_t ncap = m->cap ? m->cap * 2 : 64;
    NameSlot* ns;
    MUST(pages_alloc_fixed((void**)&ns, ncap * sizeof(NameSlot)));   // zero-filled: name == 0 is empty
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->slots[i].name) continue;
        size_t j = name_hash(m->slots[i].name, m->slots[i].len) & (ncap - 1);
        while (ns[j].name) j = (j + 1) & (ncap - 1);
        ns[j] = m->slots[i];
    }
    if (m->slots) pages_free_fixed(m->slots, m->cap * sizeof(NameSlot));
    m->slots = ns;
    m->cap = ncap;
}

Bool name_map_get(const NameMap* m, const u8* name, size_t len, u32* out_value)
{
    if (!m->cap) return 0;
    size_t mask = m->cap - 1;
    for (size_t j = name_hash(name, len) & mask; m->slots[j].name; j = (j + 1) & mask) {
        if (slot_eq(&m->slots[j], name, len)) { *out_value = m->slots[j].value; return 1; }
    }
    return 0;
}

void name_map_put(NameMap* m, const u8* name, size_t len, u32 value)
{
    if ((m->count + 1) * 2 >= m->cap) name_map_grow(m);   // keep load factor below 1/2
    size_t mask = m->cap - 1;
    size_t j = name_hash(name, len) & mask;
    while (m->slots[j].name) {
        if (slot_eq(&m->slots[j], name, len)) { m->slots[j].value = value; return; }
        j = (j + 1) & mask;
    }
    m->slots[j].name = name;
    m->slots[j].len = len;
    m->slots[j].value = value;
    m->count++;
}
