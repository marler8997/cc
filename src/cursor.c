#include "cursor.h"

#include "../ur/abortmacros.h"
#include "../ur/pagesize.h"

void cursor_ensure(Cursor* c, size_t need)
{
    if (need > c->pages->size) {
        size_t grown = c->pages->size ? c->pages->size : page_size();
        while (grown < need) grown *= 2;
        MUST(alloc_pages(c->pages, grown));
    }
}
