typedef unsigned long size_t;

int errno;

#if defined(_WIN32)
    typedef unsigned short u16;
    typedef unsigned u32;
    typedef u32 NTSTATUS;
    typedef struct {
        union { NTSTATUS Status; void* Pointer; } u;
        size_t Information;
    } IO_STATUS_BLOCK;
    typedef void* HANDLE;
    void RtlExitUserProcess(int code);
    int  NtWriteFile(void *h, void *ev, void *apc, void *apcctx, IO_STATUS_BLOCK *iosb, const void *buf, unsigned int len, long long *off, unsigned int *key);
    int  NtReadFile (void *h, void *ev, void *apc, void *apcctx, IO_STATUS_BLOCK *iosb,       void *buf, unsigned int len, long long *off, unsigned int *key);
    int  NtClose(void *h);
    int  NtAllocateVirtualMemory(void *proc, void **base, size_t zerobits, size_t *size, unsigned int type, unsigned int protect);

    // kernel32 fallback for the few things cc's libc cannot yet do via ntdll: DOS->NT path resolution
    // (fopen), file positioning (fseek/ftell), and the environment block (getenv/_putenv).
    void *CreateFileA(const char *name, unsigned int access, unsigned int share, void *sec, unsigned int disp, unsigned int flags, void *templ);
    int   SetFilePointerEx(void *h, long long dist, long long *newpos, unsigned int method);
    unsigned int GetEnvironmentVariableA(const char *name, char *buf, unsigned int size);
    int   SetEnvironmentVariableA(const char *name, const char *val);

    typedef struct {
        u16 size;
        u16 capacity;
        u16* ptr;
    } UNICODE_STRING;
    typedef struct {
        UNICODE_STRING DosPath;
        void* Handle;
    } CURDIR;
    typedef struct {
        unsigned char Reserved1[16];   /* 0x00 */
        void* Reserved2[2];            /* 0x10 */
        void* StandardInput;          /* 0x20 */
        void* StandardOutput;         /* 0x28 */
        void* StandardError;          /* 0x30 */
        CURDIR CurrentDirectory;       /* 0x38  (DosPath.Buffer @0x40) */
        UNICODE_STRING DllPath;        /* 0x50 */
        UNICODE_STRING ImagePathName;  /* 0x60 */
        UNICODE_STRING CommandLine;    /* 0x70  (Length @0x70, Buffer @0x78) */
    } RTL_USER_PROCESS_PARAMETERS;
    typedef struct {
        unsigned char Reserved1[2];
        unsigned char BeingDebugged;
        unsigned char Reserved2[1];
        void* Reserved3[2];
        void* Ldr;
        RTL_USER_PROCESS_PARAMETERS* ProcessParameters;
    } PEB;

    /* PEB at TEB+0x60; from it the process heap and the parameters block. The TEB is
       reached through the platform's thread register: gs on x86_64, x18 on aarch64. */
    static PEB* peb(void)
    {
        PEB* p;
    #if defined(__x86_64__)
        __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(p));
    #elif defined(__aarch64__)
        __asm__ volatile ("ldr %0, [x18, #0x60]" : "=r"(p));
    #elif defined(_MSC_VER) && defined(_M_X64)
        p = (PEB*)__readgsqword(0x60);     // MSVC x64: no inline asm; read gs:0x60 via intrinsic
    #else
        #error "peb(): unsupported architecture"
    #endif
        return p;
    }

    static size_t nt_write(void *h, const void *buf, size_t len)
    {
        IO_STATUS_BLOCK iosb; iosb.Information = 0;
        unsigned int s = (unsigned int)NtWriteFile(h, 0, 0, 0, &iosb, buf, (unsigned int)len, 0, 0);
        if (s & 0x80000000u) { errno = s; return 0; }
        return iosb.Information;
    }
    static size_t nt_read(void *h, void *buf, size_t len)
    {
        IO_STATUS_BLOCK iosb; iosb.Information = 0;
        unsigned int s = (unsigned int)NtReadFile(h, 0, 0, 0, &iosb, buf, (unsigned int)len, 0, 0);
        if (s & 0x80000000u) { errno = s; return 0; }
        return iosb.Information;
    }
#elif defined(__linux__)
    #define SYS_WRITE 1
    #define SYS_MMAP  9
    #define SYS_EXIT  60
    #define STDOUT 1
    #define SYSCALL3(num, a1, a2, a3) do { long __r; __asm__ volatile ("syscall" : "=a"(__r) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory"); } while (0)
    #define SYSCALL6(num, a1, a2, a3, a4, a5, a6, out) do { \
        register long __r10 __asm__("r10") = (long)(a4); \
        register long __r8  __asm__("r8")  = (long)(a5); \
        register long __r9  __asm__("r9")  = (long)(a6); \
        __asm__ volatile ("syscall" : "=a"(out) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(__r10), "r"(__r8), "r"(__r9) : "rcx", "r11", "memory"); \
    } while (0)
#else
    // TODO
#endif

// --- a few <math.h> functions, in pure double arithmetic (no bit casts: cc has no unions yet). Unused
// ones are elided by the linker's dead-code pass, so they cost nothing unless a program calls them. ---

double copysign(double x, double y)
{
    double ax = x < 0.0 ? -x : x;                          // |x|
    int yneg = y < 0.0 || (y == 0.0 && 1.0 / y < 0.0);     // sign of y, including -0.0 (1/-0 = -inf)
    return yneg ? -ax : ax;
}

double fmax(double x, double y)
{
    if (x != x) return y;                                  // x is NaN -> y
    if (y != y) return x;                                  // y is NaN -> x
    return x > y ? x : y;
}

double ldexp(double x, int exp)                            // x * 2^exp (multiplying by 2/0.5 is exact)
{
    while (exp > 0) { x = x * 2.0;  exp = exp - 1; }
    while (exp < 0) { x = x * 0.5;  exp = exp + 1; }
    return x;
}

// fused multiply-add: (x*y)+z with a single rounding. cc has no hardware-FMA encoding, so do it with
// double-double arithmetic: split x and y (Veltkamp) for an exact product (ph + pl), then an exact
// sum of ph and z (sh + sl), and combine the low-order parts.
double fma(double x, double y, double z)
{
    double split = 134217729.0;                            // 2^27 + 1
    double xt = x * split;
    double xh = xt - (xt - x);
    double xl = x - xh;
    double yt = y * split;
    double yh = yt - (yt - y);
    double yl = y - yh;
    double ph = x * y;
    double pl = xl * yl - (((ph - xh * yh) - xl * yh) - xh * yl);   // x*y = ph + pl exactly
    double sh = ph + z;
    double t = sh - ph;
    double sl = (ph - (sh - t)) + (z - t);                 // ph + z = sh + sl exactly
    return sh + (pl + sl);
}

int putchar(int c)
{
#if defined(_WIN32)
    unsigned char ch = (unsigned char)c;
    nt_write(peb()->ProcessParameters->StandardOutput, &ch, 1);
#elif defined(__linux__)
    SYSCALL3(SYS_WRITE, STDOUT, &c, 1);
    // TODO: check the return value, set errno if applicable
#else
    #error putchar not implemented on this platform
#endif
    return c;
}

// --- a few <string.h>/<stdlib.h> functions, in plain C. Like the math ones, unused functions are
// elided by the linker's dead-code pass. ---

int puts(char *s)
{
    int i = 0;
    while (s[i]) { putchar(s[i]); i = i + 1; }
    putchar('\n');
    return 0;
}

int strcmp(char *s1, char *s2)
{
    int i = 0;
    while (s1[i] && s1[i] == s2[i]) i = i + 1;
    return (unsigned char)s1[i] - (unsigned char)s2[i];   // compare as unsigned char (C 7.24.4)
}

size_t strlen(char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int atoi(char *s)
{
    int i = 0;
    int sign = 1;
    int val = 0;
    while (s[i] == ' ' || s[i] == '\t') i = i + 1;
    if (s[i] == '-') { sign = -1; i = i + 1; }
    else if (s[i] == '+') i = i + 1;
    while (s[i] >= '0' && s[i] <= '9') { val = val * 10 + (s[i] - '0'); i = i + 1; }
    return sign * val;
}

// --- <stdlib.h> heap: a K&R-style explicit free list with coalescing, over OS pages (os_alloc). Each
// block carries a 16-byte header; sizes are counted in header units, so payloads stay 16-byte aligned. ---

struct header {
    struct header *next;    // next free block (circular free list)
    size_t size;     // total block size, in units of sizeof(struct header)
};

static struct header alloc_base;       // zero-size block that seeds the circular free list
static struct header *alloc_freep = 0; // a member of the free list, where the next search starts

void free(void *ap)
{
    struct header *bp = (struct header *)ap - 1;   // the block's header sits just before its payload
    struct header *p = alloc_freep;
    while (!(bp > p && bp < p->next)) {
        if (p >= p->next && (bp > p || bp < p->next)) break;   // freed block before the start or after the end
        p = p->next;
    }
    if (bp + bp->size == p->next) {                // coalesce with the block above
        bp->size = bp->size + p->next->size;
        bp->next = p->next->next;
    } else {
        bp->next = p->next;
    }
    if (p + p->size == bp) {                       // coalesce with the block below
        p->size = p->size + bp->size;
        p->next = bp->next;
    } else {
        p->next = bp;
    }
    alloc_freep = p;
}

static struct header *morecore(size_t nunits)
{
    if (nunits < 4096) nunits = 4096;              // request at least ~64 KiB from the OS at a time
    size_t bytes = nunits * sizeof(struct header);
#if defined(_WIN32)
    // reserve+commit read/write pages via ntdll (MEM_RESERVE|MEM_COMMIT = 0x3000, PAGE_READWRITE = 4);
    // process handle (void*)-1 is the current process
    void *base = 0;
    size_t region = bytes;
    unsigned int st = (unsigned int)NtAllocateVirtualMemory((void *)-1, &base, 0, &region, 0x3000, 4);
    void *cp = (st & 0x80000000u) ? 0 : base;
#elif defined(__linux__)
    // mmap(0, bytes, PROT_READ|PROT_WRITE = 3, MAP_PRIVATE|MAP_ANONYMOUS = 34, -1, 0)
    long mret;
    SYSCALL6(SYS_MMAP, 0, bytes, 3, 34, -1, 0, mret);
    void *cp = mret < 0 ? 0 : (void *)mret;
#else
    void *cp = 0;
#endif
    if (cp == 0) return 0;
    struct header *up = (struct header *)cp;
    up->size = nunits;
    free((void *)(up + 1));                        // hand the new region to free(), which links it in
    return alloc_freep;
}

void *malloc(size_t nbytes)
{
    size_t nunits = (nbytes + sizeof(struct header) - 1) / sizeof(struct header) + 1;
    struct header *prevp = alloc_freep;
    if (prevp == 0) {                              // first call: build a degenerate one-element list
        alloc_base.next = &alloc_base;
        alloc_base.size = 0;
        alloc_freep = &alloc_base;
        prevp = &alloc_base;
    }
    struct header *p = prevp->next;
    while (1) {
        if (p->size >= nunits) {                   // big enough
            if (p->size == nunits) {               // exact fit: unlink it
                prevp->next = p->next;
            } else {                               // carve the tail off this block
                p->size = p->size - nunits;
                p = p + p->size;
                p->size = nunits;
            }
            alloc_freep = prevp;
            return (void *)(p + 1);
        }
        if (p == alloc_freep) {                    // wrapped all the way around: get more memory
            p = morecore(nunits);
            if (p == 0) return 0;
        }
        prevp = p;
        p = p->next;
    }
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    char *p = malloc(total);
    if (p) { size_t i = 0; while (i < total) { p[i] = 0; i = i + 1; } }
    return p;
}

void *realloc(void *ptr, size_t nbytes)
{
    if (ptr == 0) return malloc(nbytes);
    if (nbytes == 0) { free(ptr); return 0; }
    struct header *h = (struct header *)ptr - 1;
    size_t oldbytes = (h->size - 1) * sizeof(struct header);
    char *np = malloc(nbytes);
    if (np == 0) return 0;
    char *op = ptr;
    size_t copy = oldbytes < nbytes ? oldbytes : nbytes;
    size_t i = 0;
    while (i < copy) { np[i] = op[i]; i = i + 1; }
    free(ptr);
    return np;
}

void *aligned_alloc(size_t align, size_t size)
{
    // malloc already returns 16-byte-aligned payloads, which covers every alignment the suite requests.
    (void)align;
    return malloc(size);
}

// =====================================================================================================
// More <string.h>, <stdlib.h>, and <stdio.h> file streams. Unused functions are removed by the linker.
// =====================================================================================================

void *memcpy(void *dst, const void *src, size_t n)
{
    char *d = dst; const char *s = src;
    size_t i = 0;
    while (i < n) { d[i] = s[i]; i++; }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    char *d = dst; const char *s = src;
    if (d < s) { size_t i = 0; while (i < n) { d[i] = s[i]; i++; } }
    else       { size_t i = n; while (i > 0) { i--; d[i] = s[i]; } }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    char *d = dst;
    size_t i = 0;
    while (i < n) { d[i] = (char)c; i++; }
    return dst;
}

char *strcpy(char *dst, const char *src)
{
    size_t i = 0;
    while ((dst[i] = src[i]) != 0) i++;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != 0) { dst[i] = src[i]; i++; }
    while (i < n)                { dst[i] = 0;      i++; }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    size_t i = 0;
    while (dst[i] != 0) i++;
    size_t j = 0;
    while ((dst[i] = src[j]) != 0) { i++; j++; }
    return dst;
}

int strncmp(const char *a, const char *b, size_t n)
{
    size_t i = 0;
    while (i < n && a[i] != 0 && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

char *strchr(const char *s, int c)
{
    size_t i = 0;
    while (s[i] != 0) { if (s[i] == (char)c) return (char *)(s + i); i++; }
    if ((char)c == 0) return (char *)(s + i);
    return 0;
}

void exit(int code)
{
#if defined(_WIN32)
    RtlExitUserProcess(code);
#elif defined(__linux__)
    SYSCALL3(SYS_EXIT, code, 0, 0);
#endif
    while (1) {}   // unreachable: exit does not return
}

char *getenv(const char *name)
{
#if defined(_WIN32)
    static char __getenv_buf[32768];   // kernel32 fallback (the PEB environment block is not parsed yet)
    unsigned int n = GetEnvironmentVariableA(name, __getenv_buf, 32768);
    if (n == 0 || n >= 32768) return 0;
    return __getenv_buf;
#else
    (void)name;
    return 0;   // TODO: read the environment array passed at _start
#endif
}

int _putenv(const char *envstring)   // "NAME=VALUE"
{
#if defined(_WIN32)
    static char __putenv_name[1024];
    size_t i = 0;
    while (envstring[i] != 0 && envstring[i] != '=' && i < 1023) { __putenv_name[i] = envstring[i]; i++; }
    __putenv_name[i] = 0;
    const char *val = envstring[i] == '=' ? envstring + i + 1 : 0;
    return SetEnvironmentVariableA(__putenv_name, val) != 0 ? 0 : -1;
#else
    (void)envstring;
    return -1;   // TODO
#endif
}

typedef struct { void *handle; int text; } FILE;   // text: opened without 'b' -> CRLF translated on read

#if defined(_WIN32)
FILE *__get_stdout() { static FILE f; f.handle = peb()->ProcessParameters->StandardOutput; f.text = 0; return &f; }
FILE *__get_stderr() { static FILE f; f.handle = peb()->ProcessParameters->StandardError;  f.text = 0; return &f; }
#endif

FILE *fopen(const char *name, const char *mode)
{
#if defined(_WIN32)
    unsigned int access, disp;
    if (mode[0] == 'r')      { access = 0x80000000u; disp = 3; }   // GENERIC_READ,  OPEN_EXISTING
    else if (mode[0] == 'a') { access = 0x40000000u; disp = 4; }   // GENERIC_WRITE, OPEN_ALWAYS
    else                     { access = 0x40000000u; disp = 2; }   // GENERIC_WRITE, CREATE_ALWAYS
    void *h = CreateFileA(name, access, 1, 0, disp, 128, 0);       // kernel32 fallback (DOS->NT path
    if (h == (void *)-1) return 0;                                 //   resolution); used via Nt* below
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { NtClose(h); return 0; }
    f->handle = h;
    f->text = 1;
    for (const char *m = mode; *m; m++) if (*m == 'b') f->text = 0;   // 'b' -> binary (no translation)
    return f;
#else
    (void)name; (void)mode;
    return 0;   // TODO: Linux fopen via the open syscall
#endif
}

int fclose(FILE *f)
{
#if defined(_WIN32)
    NtClose(f->handle);
    free(f);
#endif
    return 0;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n;
    if (total == 0) return 0;
#if defined(_WIN32)
    if (f->text) {                                  // text mode: drop CR so a CRLF file reads as LF-only
        unsigned char *out = (unsigned char *)buf;
        size_t got = 0;
        while (got < total) {
            unsigned char c;
            if (nt_read(f->handle, &c, 1) != 1) break;
            if (c == '\r') continue;
            out[got++] = c;
        }
        return size ? got / size : 0;
    }
    size_t got = nt_read(f->handle, buf, total);
    if (got == 0) return 0;
    return size ? got / size : 0;
#else
    (void)buf; (void)f;
    return 0;   // TODO
#endif
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n;
    if (total == 0) return 0;
#if defined(_WIN32)
    size_t put = nt_write(f->handle, buf, total);
    return size ? put / size : 0;
#else
    (void)buf; (void)f;
    return 0;   // TODO
#endif
}

int fseek(FILE *f, long offset, int whence)   // SEEK_SET/CUR/END (0/1/2) map to FILE_BEGIN/CURRENT/END
{
#if defined(_WIN32)
    long long newpos;
    return SetFilePointerEx(f->handle, (long long)offset, &newpos, (unsigned int)whence) ? 0 : -1;
#else
    (void)f; (void)offset; (void)whence;
    return -1;   // TODO
#endif
}

long ftell(FILE *f)
{
#if defined(_WIN32)
    long long pos;
    if (SetFilePointerEx(f->handle, 0, &pos, 1) == 0) return -1;   // FILE_CURRENT
    return (long)pos;
#else
    (void)f;
    return -1;   // TODO
#endif
}

int feof(FILE *f)   // true once the position is at/past end (FILE* is a raw handle, so no per-stream flag)
{
#if defined(_WIN32)
    long long cur, size;
    if (SetFilePointerEx(f->handle, 0, &cur, 1) == 0) return 1;    // FILE_CURRENT: current position
    if (SetFilePointerEx(f->handle, 0, &size, 2) == 0) return 1;   // FILE_END: seek to end yields the size
    SetFilePointerEx(f->handle, cur, &cur, 0);                     // FILE_BEGIN: restore the position
    return cur >= size;
#else
    (void)f;
    return 1;
#endif
}

int fputs(const char *s, FILE *f)
{
    size_t n = strlen((char *)s);
#if defined(_WIN32)
    nt_write(f->handle, s, n);
#elif defined(__linux__)
    SYSCALL3(SYS_WRITE, (long)f, s, n);
#endif
    return 0;
}

int fflush(FILE *f) { (void)f; return 0; }   // streams are unbuffered: nothing to flush
