#ifndef _TARGET_H
#define _TARGET_H

#include "../ur/bool.h"

#define X_TARGETS \
    X(x86_64, windows) \
    X(x86_64, linux) \
    X(x86_64, darwin) \
    X(aarch64, windows) \
    X(aarch64, linux) \
    X(aarch64, darwin) \

typedef enum {
    #define X(arch, os) TARGET_##arch##_##os,
        X_TARGETS
    #undef X
} Target;

enum {
#define X(arch,os) _IGNORE_ME_TARGET_##arch##os,
    X_TARGETS
#undef X
    TARGET_COUNT
};

const char* target_string(Target);
Bool target_is_windows(Target);

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    #define TARGET_HOST TARGET_x86_64_windows
#elif defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
    #define TARGET_HOST TARGET_aarch64_windows
#elif defined(__linux__) && defined(__x86_64__)
    #define TARGET_HOST TARGET_x86_64_linux
#elif defined(__linux__) && defined(__aarch64__)
    #define TARGET_HOST TARGET_aarch64_linux
#elif defined(__MACH__) && defined(__x86_64__)
    #define TARGET_HOST TARGET_x86_64_darwin
#elif defined(__MACH__) && defined(__aarch64__)
    #define TARGET_HOST TARGET_aarch64_darwin
#else
    #error "unsupported architecture/os"
#endif

#endif // _TARGET_H
