#pragma once
#include <windows.h>

typedef HINSTANCE(WINAPI* LoadLibraryA_t)(const char*);
typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, const char*);

struct DllInfo {
    void* base;
    LoadLibraryA_t load_library_a;
    GetProcAddress_t get_proc_address;
    bool relocation_required;
};

extern "C" {

__attribute__((section(".inject"), noinline))
inline void WINAPI realign_pe(DllInfo* dll_info) {
    void* base = dll_info->base;
    LoadLibraryA_t load_library_a = dll_info->load_library_a;
    GetProcAddress_t get_proc_address = dll_info->get_proc_address;

    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt_headers = (IMAGE_NT_HEADERS64*)((size_t)base + dos_header->e_lfanew);

    // 1. Relocations
    if (dll_info->relocation_required) {
        IMAGE_DATA_DIRECTORY* reloc_dir = &nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir->VirtualAddress != 0) {
            size_t delta = (size_t)base - (size_t)nt_headers->OptionalHeader.ImageBase;
            IMAGE_BASE_RELOCATION* block_ptr = (IMAGE_BASE_RELOCATION*)((size_t)base + reloc_dir->VirtualAddress);

            while (block_ptr->SizeOfBlock >= 8 && block_ptr->VirtualAddress != 0) {
                size_t block_size = block_ptr->SizeOfBlock;
                size_t entry_count = (block_size - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(unsigned short);
                unsigned short* entries = (unsigned short*)((size_t)block_ptr + sizeof(IMAGE_BASE_RELOCATION));

                for (size_t i = 0; i < entry_count; i++) {
                    unsigned short entry = entries[i];
                    unsigned short rel_type = entry >> 12;
                    unsigned short offset = entry & 0x0FFF;

                    if (rel_type == IMAGE_REL_BASED_DIR64) {
                        size_t* patch = (size_t*)((size_t)base + block_ptr->VirtualAddress + offset);
                        *patch += delta;
                    }
                }
                block_ptr = (IMAGE_BASE_RELOCATION*)((size_t)block_ptr + block_size);
            }
        }
    }

    // 2. Imports
    IMAGE_DATA_DIRECTORY* import_dir = &nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir->VirtualAddress != 0) {
        IMAGE_IMPORT_DESCRIPTOR* import_desc = (IMAGE_IMPORT_DESCRIPTOR*)((size_t)base + import_dir->VirtualAddress);

        while (import_desc->Name != 0) {
            const char* lib_name = (const char*)((size_t)base + import_desc->Name);
            HMODULE h_module = load_library_a(lib_name);

            DWORD oft = import_desc->OriginalFirstThunk;
            IMAGE_THUNK_DATA64* orig_thunk = (IMAGE_THUNK_DATA64*)((size_t)base + (oft ? oft : import_desc->FirstThunk));
            IMAGE_THUNK_DATA64* first_thunk = (IMAGE_THUNK_DATA64*)((size_t)base + import_desc->FirstThunk);

            while (orig_thunk->u1.AddressOfData != 0) {
                if (orig_thunk->u1.Ordinal & (1ULL << 63)) {
                    const char* ordinal = (const char*)(orig_thunk->u1.Ordinal & 0xFFFF);
                    first_thunk->u1.Function = (size_t)get_proc_address(h_module, ordinal);
                } else {
                    IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)((size_t)base + (size_t)orig_thunk->u1.AddressOfData);
                    first_thunk->u1.Function = (size_t)get_proc_address(h_module, (const char*)ibn->Name);
                }
                orig_thunk++;
                first_thunk++;
            }
            import_desc++;
        }
    }

    // 3. Entry Point
    DWORD ep_rva = nt_headers->OptionalHeader.AddressOfEntryPoint;
    if (ep_rva != 0) {
        typedef BOOL(WINAPI* DllMain_t)(HINSTANCE, DWORD, LPVOID);
        DllMain_t entry_point = (DllMain_t)((size_t)base + ep_rva);
        entry_point((HINSTANCE)base, DLL_PROCESS_ATTACH, NULL);
    }
}

__attribute__((section(".inject"), noinline))
inline void WINAPI realign_pe_end() {}

}
