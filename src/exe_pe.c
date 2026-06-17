#include "../ur/abortmacros.h"
#include "../ur/arena.h"
#include "../ur/log.h"
#include "../ur/mem.h"
#include "../ur/processexit.h"
#include "arch.h"
#include "codegen.h"
#include "cursor.h"
#include "program.h"

// amd64-windows PE. Headers occupy the first file-alignment block; then .text (one or more blocks
// of code), .rdata (the import table), and .data (statics). .rdata/.data float past the code, so
// the program size is unbounded -- the code can span any number of pages.
#define IMAGE_BASE        0x140000000ull
#define FILE_ALIGN        0x200
#define SECT_ALIGN        0x1000
#define SIZE_OF_HEADERS   0x200

#define TEXT_RVA          0x1000
#define TEXT_FILE         0x200

// An application manifest declaring asInvoker, embedded as an RT_MANIFEST resource. Without it,
// Windows' installer-detection heuristic elevates (and so refuses to launch under a non-elevated
// parent) any exe whose name contains "update"/"setup"/"install"/"patch".
static const char MANIFEST_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
    "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>"
    "<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>"
    "</requestedPrivileges></security></trustInfo></assembly>";

// .rdata file offset of an RVA that lives in .rdata
static size_t rdata_off(size_t rdata_file, u32 rdata_rva, u32 rva) { return rdata_file + (rva - rdata_rva); }

// a hint/name entry is hint(2) + name + NUL, padded to an even length
static u32 hintname_size(u32 name_len) { return (u32)((2 + (size_t)name_len + 1 + 1) & ~(size_t)1); }

// import-table layout scratch: one Dll per distinct DLL, one Imp per import
typedef struct {
    const char* name_ptr;
    u32 name_len;
    u32 name_rva;
    u32 run_start;
} Dll;  // run_start: first IAT slot of its run
typedef struct { u32 dll; u32 hint_rva; } Imp;                           // dll: index into the Dll array

size_t emit_pe(Arch arch, const Program* prog, Pages* image)
{
    u16 machine;
    switch (arch) {
        case ARCH_x86_64:  machine = 0x8664; break;
        case ARCH_aarch64: machine = 0xAA64; break;
        default: LOG_STRING("emit_pe: unsupported arch"); process_exit(70); return 0;
    }

    // ---- group the program's used imports by DLL (RVA-independent): each DLL gets one import
    //      descriptor and a null-terminated ILT/IAT run, so a slot isn't simply index*8 -- per-import
    //      slots feed the Abi. ----
    u32 n = (u32)prog->import_count;
    ASSERT(n >= 1);

    // working arrays, sized to the import count (#DLLs <= #imports). `import_slot` is kept a plain
    // u32[] (not folded into Imp) because codegen indexes it contiguously through the Abi.
    ArenaPosition arena_pos = arena_position(&global_arena);
    Dll* dlls = ARENA_ALLOC(&global_arena, n * sizeof(Dll));
    Imp* imps = ARENA_ALLOC(&global_arena, n * sizeof(Imp));
    u32* import_slot = ARENA_ALLOC(&global_arena, n * sizeof(u32));   // import i -> its IAT slot (fed to the Abi)

    // distinct DLLs in first-seen order. import 0 is the exit import, so its DLL is first and exit
    // keeps IAT slot 0 (where the entry stub's emit_exit expects it).
    u32 D = 0;
    for (u32 i = 0; i < n; i++) {
        Import import_ref = program_import_at(prog, i);
        u32 di = D;
        for (u32 k = 0; k < D; k++) if (dlls[k].name_ptr == import_ref.dll_ptr) { di = k; break; }
        if (di == D) {
            dlls[D].name_ptr = import_ref.dll_ptr;
            dlls[D].name_len = (u32)import_ref.dll_len;
            D++;
        }
        imps[i].dll = di;
    }

    // per-import IAT slot: DLLs in first-seen order, imports in index order within a DLL, a null
    // terminator after each DLL's run. `import_slot[i]` is what the Abi hands codegen.
    u32 slot = 0;
    for (u32 d = 0; d < D; d++) {
        dlls[d].run_start = slot;
        for (u32 i = 0; i < n; i++) if (imps[i].dll == d) import_slot[i] = slot++;
        slot++;                                 // null terminator
    }
    u32 iat_entries = n + D;                    // imports + one null per DLL

    // ---- pass 1: measure the code. Its length doesn't depend on the IAT/.data RVAs (every
    //      rip-relative ref encodes a fixed 32-bit displacement), so a throwaway Abi suffices. ----
    Abi probe = { ABI_WIN64, TEXT_RVA, TEXT_RVA, TEXT_RVA, import_slot, 0 };
    Pages probe_code = PAGES_INIT();
    size_t code_len = gen_code(arch, &probe, prog, &probe_code, 0);

    // ---- section layout: .rdata and .data float past the (possibly multi-page) code ----
    size_t text_raw = round_up(code_len, FILE_ALIGN);
    if (text_raw == 0) text_raw = FILE_ALIGN;
    u32 rdata_rva = TEXT_RVA + (u32)round_up(code_len, SECT_ALIGN);
    size_t rdata_file = TEXT_FILE + text_raw;

    u32 import_dir_rva = rdata_rva;             // D descriptors + a null terminator
    u32 ilt_rva = import_dir_rva + (D + 1) * 20;
    u32 iat_rva = ilt_rva + iat_entries * 8;    // ILT then IAT (identical shapes)
    u32 hint_cursor = iat_rva + iat_entries * 8;
    for (u32 i = 0; i < n; i++) { imps[i].hint_rva = hint_cursor; hint_cursor += hintname_size(program_import_at(prog, i).name_len); }
    for (u32 d = 0; d < D; d++) { dlls[d].name_rva = hint_cursor; hint_cursor += dlls[d].name_len + 1; }
    u32 rdata_vsize = hint_cursor - rdata_rva;
    size_t rdata_raw = round_up(rdata_vsize, FILE_ALIGN);   // the import table can span several raw blocks
    ASSERT(rdata_vsize <= SECT_ALIGN);                      // ...but stays within one virtual page

    // .data (used statics) follows .rdata, one page later. Unlike .rdata it can span many pages
    // (large arrays), so it occupies round_up(ndata) of both virtual and raw space.
    u32 ndata = program_static_size(prog);
    Bool have_data = ndata > 0;
    u32 data_rva = rdata_rva + SECT_ALIGN;
    u32 data_vspan = have_data ? (u32)round_up(ndata, SECT_ALIGN) : 0;
    size_t data_raw = round_up(ndata, FILE_ALIGN);   // 0 when no data

    // .rsrc (the asInvoker manifest) is the last section, one page past .rdata/.data. Its tree is a
    // fixed 3-level chain (type 24 -> id 1 -> lang) of 24-byte dirs + a 16-byte data entry = 88 bytes.
    u32 rsrc_rva = (have_data ? data_rva + data_vspan : rdata_rva + SECT_ALIGN);
    u32 manifest_len = (u32)(sizeof(MANIFEST_XML) - 1);
    u32 manifest_off = 88;
    u32 rsrc_size = manifest_off + manifest_len;
    ASSERT(rsrc_size <= FILE_ALIGN);             // the resource fits one raw .rsrc block

    // .reloc (base relocations for .data->code function-address pointers, e.g. vtable slots) is the
    // last section. Collect the fixups (sorted by .data offset), then size them as base-reloc blocks:
    // one per 4 KB page, each an 8-byte header + a 2-byte entry per fixup (padded to a 4-byte boundary).
    u32 nreloc = (u32)prog->static_relocs.count;
    ProgramReloc* relocs = 0;
    u32 reloc_size = 0;
    if (nreloc) {
        relocs = ARENA_ALLOC(&global_arena, nreloc * sizeof(ProgramReloc));
        u32 ri = 0;
        STABLE_LIST_FOREACH(it, &prog->static_relocs) relocs[ri++] = *(ProgramReloc*)it.elem;
        for (u32 a = 1; a < nreloc; a++) {       // insertion sort by .data offset (few entries)
            ProgramReloc t = relocs[a]; u32 b = a;
            while (b && relocs[b - 1].data_offset > t.data_offset) { relocs[b] = relocs[b - 1]; b--; }
            relocs[b] = t;
        }
        for (u32 i = 0; i < nreloc; ) {
            u32 page = (data_rva + relocs[i].data_offset) & ~0xFFFu;
            u32 cnt = 0;
            while (i + cnt < nreloc && ((data_rva + relocs[i + cnt].data_offset) & ~0xFFFu) == page) cnt++;
            reloc_size += 8 + (cnt + (cnt & 1)) * 2;   // header + entries, padded to a 4-byte boundary
            i += cnt;
        }
        ASSERT(reloc_size <= SECT_ALIGN);        // fixups stay within one virtual page (raw may span blocks)
    }
    Bool have_reloc = nreloc > 0;
    u32 reloc_raw = (u32)round_up(reloc_size, FILE_ALIGN);   // .reloc can span several raw blocks
    u32 reloc_rva = rsrc_rva + SECT_ALIGN;

    u32 nsections = (have_data ? 4 : 3) + (have_reloc ? 1 : 0);
    u32 image_size = (have_reloc ? reloc_rva + SECT_ALIGN : rsrc_rva + SECT_ALIGN);

    // ---- pass 2: the real code, now that the IAT/.data RVAs are known. Windows ABI: shadow space;
    //      imports (exit at slot 0) via their IAT slots; statics in .data. `func_offsets` lets us
    //      resolve each function's address for the .data->code relocations above. ----
    u32* func_offsets = ARENA_ALLOC(&global_arena, (prog->func_id_count ? prog->func_id_count : 1) * sizeof(u32));
    Abi abi = { ABI_WIN64, iat_rva, data_rva, TEXT_RVA, import_slot, 0 };
    Pages code = PAGES_INIT();
    size_t code_len2 = gen_code(arch, &abi, prog, &code, func_offsets);
    ASSERT(code_len2 == code_len);              // the layout above assumed this length

    size_t data_file = rdata_file + rdata_raw;               // valid only if have_data
    size_t rsrc_file = (have_data ? data_file + data_raw : rdata_file + rdata_raw - FILE_ALIGN) + FILE_ALIGN;
    size_t reloc_file = rsrc_file + FILE_ALIGN;              // valid only if have_reloc
    size_t total = (have_reloc ? reloc_file + reloc_raw : rsrc_file + FILE_ALIGN);

    Cursor c = { image, 0 };
    cursor_ensure(&c, total);
    for (size_t k = 0; k < total; k++) image->ptr[k] = 0;

    // ---- DOS header ----
    put8(&c, 'M'); put8(&c, 'Z');
    seek(&c, 0x3C);
    put32(&c, 0x40);                 // e_lfanew -> PE header immediately after the DOS header

    // ---- PE signature + COFF file header ----
    seek(&c, 0x40);
    put_bytes(&c, "PE\0\0", 4);
    put16(&c, machine);              // Machine
    put16(&c, (u16)nsections);       // NumberOfSections
    put32(&c, 0);                    // TimeDateStamp
    put32(&c, 0);                    // PointerToSymbolTable
    put32(&c, 0);                    // NumberOfSymbols
    put16(&c, 0xF0);                 // SizeOfOptionalHeader (PE32+)
    put16(&c, 0x0022);               // Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    // ---- Optional header (PE32+): standard fields ----
    put16(&c, 0x020B);               // Magic = PE32+
    put8(&c, 0); put8(&c, 0);        // linker version
    put32(&c, (u32)text_raw);        // SizeOfCode
    put32(&c, FILE_ALIGN);           // SizeOfInitializedData
    put32(&c, 0);                    // SizeOfUninitializedData
    put32(&c, TEXT_RVA);             // AddressOfEntryPoint
    put32(&c, TEXT_RVA);             // BaseOfCode

    // ---- Optional header: windows-specific fields ----
    put64(&c, IMAGE_BASE);
    put32(&c, SECT_ALIGN);
    put32(&c, FILE_ALIGN);
    put16(&c, 6); put16(&c, 0);      // OS version 6.0
    put16(&c, 0); put16(&c, 0);      // image version
    put16(&c, 6); put16(&c, 0);      // subsystem version 6.0
    put32(&c, 0);                    // Win32VersionValue
    put32(&c, image_size);
    put32(&c, SIZE_OF_HEADERS);
    put32(&c, 0);                    // CheckSum
    put16(&c, 3);                    // Subsystem = WINDOWS_CUI
    put16(&c, 0x140);                // DllCharacteristics: DYNAMIC_BASE | NX_COMPAT (ARM64 won't load without both)
    put64(&c, 0x1000000);            // SizeOfStackReserve (16 MB): cc's unoptimized codegen gives every
                                     // temp its own stack slot, so frames are large; self-hosting (deep
                                     // recursive-descent parsing) needs ample stack (the 1 MB default is
                                     // too small). Pages commit lazily; codegen probes large frames.
    put64(&c, 0x1000);               // SizeOfStackCommit
    put64(&c, 0x100000);             // SizeOfHeapReserve
    put64(&c, 0x1000);               // SizeOfHeapCommit
    put32(&c, 0);                    // LoaderFlags
    put32(&c, 16);                   // NumberOfRvaAndSizes

    // ---- Optional header: data directories (16 entries) ----
    for (u32 i = 0; i < 16; i++) {
        if (i == 1) {                // Import Table
            put32(&c, import_dir_rva);
            put32(&c, (D + 1) * 20);
        } else if (i == 2) {         // Resource Table
            put32(&c, rsrc_rva);
            put32(&c, rsrc_size);
        } else if (i == 5 && have_reloc) {   // Base Relocation Table
            put32(&c, reloc_rva);
            put32(&c, reloc_size);
        } else if (i == 12) {        // Import Address Table
            put32(&c, iat_rva);
            put32(&c, iat_entries * 8);
        } else {
            put32(&c, 0);
            put32(&c, 0);
        }
    }

    // ---- Section headers ----
    put_bytes(&c, ".text", 5); put8(&c, 0); put8(&c, 0); put8(&c, 0);  // 8-byte name
    put32(&c, (u32)code_len);        // VirtualSize
    put32(&c, TEXT_RVA);             // VirtualAddress
    put32(&c, (u32)text_raw);        // SizeOfRawData
    put32(&c, TEXT_FILE);            // PointerToRawData
    put32(&c, 0); put32(&c, 0);      // PointerToRelocations / PointerToLinenumbers
    put16(&c, 0); put16(&c, 0);      // NumberOfRelocations / NumberOfLinenumbers
    put32(&c, 0x60000020);           // CNT_CODE | MEM_EXECUTE | MEM_READ

    put_bytes(&c, ".rdata", 6); put8(&c, 0); put8(&c, 0);             // 8-byte name
    put32(&c, rdata_vsize);
    put32(&c, rdata_rva);
    put32(&c, (u32)rdata_raw);       // SizeOfRawData
    put32(&c, (u32)rdata_file);      // PointerToRawData
    put32(&c, 0); put32(&c, 0);
    put16(&c, 0); put16(&c, 0);
    put32(&c, 0xC0000040);           // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE (loader patches the IAT)

    if (have_data) {
        put_bytes(&c, ".data", 5); put8(&c, 0); put8(&c, 0); put8(&c, 0);
        put32(&c, ndata);            // VirtualSize
        put32(&c, data_rva);
        put32(&c, (u32)data_raw);    // SizeOfRawData
        put32(&c, (u32)data_file);   // PointerToRawData
        put32(&c, 0); put32(&c, 0);
        put16(&c, 0); put16(&c, 0);
        put32(&c, 0xC0000040);       // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE
    }

    put_bytes(&c, ".rsrc", 5); put8(&c, 0); put8(&c, 0); put8(&c, 0);
    put32(&c, rsrc_size);            // VirtualSize
    put32(&c, rsrc_rva);
    put32(&c, FILE_ALIGN);           // SizeOfRawData
    put32(&c, (u32)rsrc_file);       // PointerToRawData
    put32(&c, 0); put32(&c, 0);
    put16(&c, 0); put16(&c, 0);
    put32(&c, 0x40000040);           // CNT_INITIALIZED_DATA | MEM_READ

    if (have_reloc) {
        put_bytes(&c, ".reloc", 6); put8(&c, 0); put8(&c, 0);
        put32(&c, reloc_size);       // VirtualSize
        put32(&c, reloc_rva);
        put32(&c, reloc_raw);        // SizeOfRawData
        put32(&c, (u32)reloc_file);  // PointerToRawData
        put32(&c, 0); put32(&c, 0);
        put16(&c, 0); put16(&c, 0);
        put32(&c, 0x42000040);       // CNT_INITIALIZED_DATA | MEM_DISCARDABLE | MEM_READ
    }

    // ---- .text: the generated code ----
    seek(&c, TEXT_FILE);
    for (size_t k = 0; k < code_len; k++) put8(&c, code.ptr[k]);

    // ---- .rdata: import directory table (one descriptor per DLL, then the null terminator) ----
    seek(&c, rdata_off(rdata_file, rdata_rva, import_dir_rva));
    for (u32 d = 0; d < D; d++) {
        put32(&c, ilt_rva + dlls[d].run_start * 8);  // OriginalFirstThunk -> this DLL's ILT run
        put32(&c, 0);                                // TimeDateStamp
        put32(&c, 0);                                // ForwarderChain
        put32(&c, dlls[d].name_rva);                 // Name
        put32(&c, iat_rva + dlls[d].run_start * 8);  // FirstThunk -> this DLL's IAT run
    }
    // (the null-terminator descriptor's 20 bytes are already zero)

    // import lookup table and import address table (identical; the loader overwrites the IAT). Each
    // DLL's run holds its imports (in index order) followed by a null, matching `import_slot`.
    seek(&c, rdata_off(rdata_file, rdata_rva, ilt_rva));
    for (u32 d = 0; d < D; d++) {
        for (u32 i = 0; i < n; i++) if (imps[i].dll == d) put64(&c, imps[i].hint_rva);
        put64(&c, 0);
    }
    seek(&c, rdata_off(rdata_file, rdata_rva, iat_rva));
    for (u32 d = 0; d < D; d++) {
        for (u32 i = 0; i < n; i++) if (imps[i].dll == d) put64(&c, imps[i].hint_rva);
        put64(&c, 0);
    }

    // hint/name table, one entry per import
    for (u32 i = 0; i < n; i++) {
        Import ir = program_import_at(prog, i);
        seek(&c, rdata_off(rdata_file, rdata_rva, imps[i].hint_rva));
        put16(&c, 0);                // hint
        put_bytes(&c, ir.name_ptr, ir.name_len);
        put8(&c, 0);                 // name terminator (the following pad byte is already zero)
    }

    // dll names, one per DLL
    for (u32 d = 0; d < D; d++) {
        seek(&c, rdata_off(rdata_file, rdata_rva, dlls[d].name_rva));
        put_bytes(&c, dlls[d].name_ptr, dlls[d].name_len);
        put8(&c, 0);
    }

    // ---- .data: the static variables' initializer bytes, then the function-address fixups (each slot
    //      gets the function's preferred absolute address; .reloc lets the loader adjust if relocated) ----
    if (have_data) {
        seek(&c, data_file);
        for (u32 i = 0; i < ndata; i++) put8(&c, program_static_byte(prog, i));
        for (u32 r = 0; r < nreloc; r++) {
            seek(&c, data_file + relocs[r].data_offset);
            if (relocs[r].to_static) {
                u64 addend = 0;   // the slot holds a byte offset into the target static (e.g. &arr[1]); add it in
                for (int k = 0; k < 8; k++) addend |= (u64)program_static_byte(prog, relocs[r].data_offset + k) << (k * 8);
                put64(&c, IMAGE_BASE + data_rva + relocs[r].target + addend);   // .data->.data: address (+offset) of another static
            } else {
                u32 fo = func_offsets[relocs[r].target];
                if (fo == 0xFFFFFFFFu) { LOG_STRING("exe_pe: static relocation targets an unemitted function"); process_exit(72); }
                put64(&c, IMAGE_BASE + TEXT_RVA + fo);
            }
        }
    }

    // ---- .rsrc: a 3-level resource tree (type 24 = RT_MANIFEST -> id 1 -> lang) -> the manifest.
    //      Each directory is a 16-byte header (last field NumberOfIdEntries=1) + one 8-byte entry;
    //      a subdirectory link sets the high bit of the offset, a leaf points at the data entry. ----
    seek(&c, rsrc_file);
    put32(&c, 0); put32(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 1);  // type dir
    put32(&c, 24);  put32(&c, 0x80000000u | 24);                                          // RT_MANIFEST -> name dir
    put32(&c, 0); put32(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 1);  // name dir
    put32(&c, 1);   put32(&c, 0x80000000u | 48);                                          // resource id 1 -> lang dir
    put32(&c, 0); put32(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 0); put16(&c, 1);  // lang dir
    put32(&c, 0x409); put32(&c, 72);                                                      // en-US -> data entry (leaf)
    put32(&c, rsrc_rva + manifest_off); put32(&c, manifest_len); put32(&c, 0); put32(&c, 0);  // data entry
    put_bytes(&c, MANIFEST_XML, manifest_len);

    // ---- .reloc: base-relocation blocks (one per 4 KB page) for the .data->code fixups. Each entry is
    //      IMAGE_REL_BASED_DIR64 (type 10) so the loader adds (actual_base - IMAGE_BASE) to the 8 bytes. ----
    if (have_reloc) {
        seek(&c, reloc_file);
        for (u32 i = 0; i < nreloc; ) {
            u32 page = (data_rva + relocs[i].data_offset) & ~0xFFFu;
            u32 cnt = 0;
            while (i + cnt < nreloc && ((data_rva + relocs[i + cnt].data_offset) & ~0xFFFu) == page) cnt++;
            put32(&c, page);                              // PageRVA
            put32(&c, 8 + (cnt + (cnt & 1)) * 2);         // BlockSize (header + entries, 4-byte aligned)
            for (u32 k = 0; k < cnt; k++)
                put16(&c, (u16)((10u << 12) | ((data_rva + relocs[i + k].data_offset) - page)));
            if (cnt & 1) put16(&c, 0);                    // pad entry (IMAGE_REL_BASED_ABSOLUTE, ignored)
            i += cnt;
        }
    }

    arena_reset(&global_arena, arena_pos);
    return total;
}
