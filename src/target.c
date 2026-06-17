#include "target.h"

#include "../ur/abortmacros.h"

const char* target_string(Target t)
{
    switch (t) {
    #define X(arch, os) case TARGET_##arch##_##os: return #arch "-" #os;
        X_TARGETS
    #undef X
    }
    UNREACHABLE();
}

Bool target_is_windows(Target target)
{
    switch (target) {
    case TARGET_x86_64_windows: return 1;
    case TARGET_x86_64_linux: return 0;
    case TARGET_x86_64_darwin: return 0;
    case TARGET_aarch64_windows: return 1;
    case TARGET_aarch64_linux: return 0;
    case TARGET_aarch64_darwin: return 0;
    }
    UNREACHABLE();
}
