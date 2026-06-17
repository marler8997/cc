#include "../ur/int.h"
#include "../ur/size_t.h"

// cc's own object-file format: a serialized TranslationUnit (the size-agnostic IR IR plus the
// symbol table). `cc -c` writes one; the linker reads it back into a TU and merges it with the
// other inputs exactly like a freshly parsed source file. This keeps separate compilation fully
// self-contained -- no external object format, assembler, or linker -- since cc both emits and
// consumes it. (A real COFF/ELF object would only be needed to interoperate with other toolchains.)

typedef struct struct_Pages Pages;
typedef struct struct_TranslationUnit TranslationUnit;

// serialize `tu` into `out`; returns the byte length.
size_t obj_write(const TranslationUnit* tu, Pages* out);

// reconstruct a TU from `bytes` (length `len`). Allocates its own backing storage, so `bytes`
// need not outlive the call.
void obj_read(const u8* bytes, size_t len, TranslationUnit* out);
