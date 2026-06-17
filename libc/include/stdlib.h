#ifndef _STDLIB_H
#define _STDLIB_H

#ifndef NULL
    #define NULL ((void *)0)
#endif
#ifndef _SIZE_T
    #define _SIZE_T
    typedef unsigned long size_t;
#endif
#ifndef _WCHAR_T
    #define _WCHAR_T
    #ifdef _WIN32
        typedef unsigned short wchar_t;
    #else
        typedef int wchar_t;
    #endif
#endif

void *malloc(size_t size);
void free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t size);
void *aligned_alloc(size_t align, size_t size);
int atoi(char *s);
void exit(int code);
char *getenv(const char *name);
int _putenv(const char *envstring);

#endif // _STDLIB_H
