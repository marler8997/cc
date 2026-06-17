#ifndef _STRING_H
#define _STRING_H

#ifndef NULL
    #define NULL ((void *)0)
#endif
#ifndef _SIZE_T
    #define _SIZE_T
    typedef unsigned long size_t;
#endif

size_t strlen(char *s);
int strcmp(char *a, char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif // _STRING_H
