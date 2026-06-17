#ifndef _STDIO_H
#define _STDIO_H

#ifndef NULL
    #define NULL ((void *)0)
#endif
#ifndef _SIZE_T
    #define _SIZE_T
    typedef unsigned long size_t;
#endif

// A FILE wraps the OS handle plus an EOF flag (must match libc.c's definition).
typedef struct { void *handle; int eof; } FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifdef _WIN32
    FILE *__get_stdout();
    FILE *__get_stderr();
    #define stdout __get_stdout()
    #define stderr __get_stderr()
#else
    #define stdout 1
    #define stderr 2
#endif

int putchar(int);
int puts(char *);
FILE *fopen(const char *name, const char *mode);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int feof(FILE *f);
int fputs(const char *s, FILE *f);
int fflush(FILE *f);

#endif // _STDIO_H
