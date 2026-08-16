#include "MemoryLoader.hpp"
#include <iostream>

typedef struct _RELOC_ENTRY {
    WORD Offset : 12;
    WORD Type : 4;
} RELOC_ENTRY, *PRELOC_ENTRY;

HMODULE MemoryLoader::Load(const std::vector<unsigned char>& buffer) {
    if (buffer.empty()) return NULL;

    PBYTE pRawData = (PBYTE)buffer.data();
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pRawData;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    PIMAGE_NT_HEADERS pNtHeaders = (PIMAGE_NT_HEADERS)(pRawData + pDosHeader->e_lfanew);
    if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE) return NULL;

    // Mimari kontrolü (x86 vs x64 mismatch engelleme)
#ifdef _WIN64
    if (pNtHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return NULL;
#else
    if (pNtHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return NULL;
#endif

    // 1. ImageBase için bellek ayır
    PBYTE pImageBase = (PBYTE)VirtualAlloc(NULL, pNtHeaders->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pImageBase) return NULL;

    // 2. Headerları kopyala
    memcpy(pImageBase, pRawData, pNtHeaders->OptionalHeader.SizeOfHeaders);

    // 3. Sectionları kopyala
    PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
    for (int i = 0; i < pNtHeaders->FileHeader.NumberOfSections; i++) {
        memcpy(pImageBase + pSectionHeader[i].VirtualAddress, pRawData + pSectionHeader[i].PointerToRawData, pSectionHeader[i].SizeOfRawData);
    }

    // 4. Relocation (Yeniden Konumlandırma) düzeltmesi
    auto& relocDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (relocDir.Size > 0 && relocDir.VirtualAddress != 0) {
        PIMAGE_BASE_RELOCATION pReloc = (PIMAGE_BASE_RELOCATION)(pImageBase + relocDir.VirtualAddress);
        DWORD_PTR delta = (DWORD_PTR)(pImageBase - (PBYTE)pNtHeaders->OptionalHeader.ImageBase);

        while (pReloc->VirtualAddress != 0 && pReloc->SizeOfBlock > 0) {
            DWORD count = (pReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            PWORD pRelativeReloc = (PWORD)(pReloc + 1);

            for (DWORD i = 0; i < count; i++) {
                WORD type = pRelativeReloc[i] >> 12;
                WORD offset = pRelativeReloc[i] & 0xFFF;

                if (type == IMAGE_REL_BASED_DIR64) {
                    PULONGLONG pPatch = (PULONGLONG)(pImageBase + pReloc->VirtualAddress + offset);
                    *pPatch += (ULONGLONG)delta;
                }
                else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    PDWORD pPatch = (PDWORD)(pImageBase + pReloc->VirtualAddress + offset);
                    *pPatch += (DWORD)delta;
                }
            }
            pReloc = (PIMAGE_BASE_RELOCATION)((PBYTE)pReloc + pReloc->SizeOfBlock);
        }
    }

    // 5. IAT (Import Address Table) çözümleme
    auto& importDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.Size > 0 && importDir.VirtualAddress != 0) {
        PIMAGE_IMPORT_DESCRIPTOR pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)(pImageBase + importDir.VirtualAddress);
        while (pImportDesc->Name != 0) {
            HMODULE hLib = LoadLibraryA((LPCSTR)(pImageBase + pImportDesc->Name));
            if (hLib) {
                PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)(pImageBase + pImportDesc->FirstThunk);
                // OriginalFirstThunk bazen 0 olabilir, bu durumda FirstThunk kullanılır
                PIMAGE_THUNK_DATA pOrigThunk = (pImportDesc->OriginalFirstThunk != 0) ?
                    (PIMAGE_THUNK_DATA)(pImageBase + pImportDesc->OriginalFirstThunk) : pThunk;

                while (pThunk->u1.AddressOfData != 0) {
                    if (IMAGE_SNAP_BY_ORDINAL(pOrigThunk->u1.Ordinal)) {
                        pThunk->u1.Function = (DWORD_PTR)GetProcAddress(hLib, (LPCSTR)IMAGE_ORDINAL(pOrigThunk->u1.Ordinal));
                    } else {
                        PIMAGE_IMPORT_BY_NAME pImportName = (PIMAGE_IMPORT_BY_NAME)(pImageBase + pOrigThunk->u1.AddressOfData);
                        pThunk->u1.Function = (DWORD_PTR)GetProcAddress(hLib, pImportName->Name);
                    }
                    pThunk++;
                    pOrigThunk++;
                }
            }
            pImportDesc++;
        }
    }

    // 6. Exception Directory (x64 için önemli)
#ifdef _WIN64
    auto& exceptionDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exceptionDir.Size > 0 && exceptionDir.VirtualAddress != 0) {
        RtlAddFunctionTable(
            (PRUNTIME_FUNCTION)(pImageBase + exceptionDir.VirtualAddress),
            exceptionDir.Size / sizeof(RUNTIME_FUNCTION),
            (DWORD64)pImageBase
        );
    }
#endif

    // 7. DllMain'i çağır
    if (pNtHeaders->OptionalHeader.AddressOfEntryPoint != 0) {
        typedef BOOL(WINAPI* PDLL_MAIN)(HMODULE, DWORD, LPVOID);
        PDLL_MAIN pDllMain = (PDLL_MAIN)(pImageBase + pNtHeaders->OptionalHeader.AddressOfEntryPoint);
        pDllMain((HMODULE)pImageBase, DLL_PROCESS_ATTACH, NULL);
    }

    return (HMODULE)pImageBase;
}

FARPROC MemoryLoader::GetExportAddress(HMODULE hMod, const char* name) {
    if (!hMod) return NULL;

    PBYTE pBase = (PBYTE)hMod;
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pBase;
    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)(pBase + pDos->e_lfanew);

    auto& exportDirAttr = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDirAttr.Size == 0) return NULL;

    PIMAGE_EXPORT_DIRECTORY pExportDir = (PIMAGE_EXPORT_DIRECTORY)(pBase + exportDirAttr.VirtualAddress);
    PDWORD pNames = (PDWORD)(pBase + pExportDir->AddressOfNames);
    PWORD pOrdinals = (PWORD)(pBase + pExportDir->AddressOfNameOrdinals);
    PDWORD pFunctions = (PDWORD)(pBase + pExportDir->AddressOfFunctions);

    for (DWORD i = 0; i < pExportDir->NumberOfNames; i++) {
        if (strcmp(name, (const char*)(pBase + pNames[i])) == 0) {
            return (FARPROC)(pBase + pFunctions[pOrdinals[i]]);
        }
    }

    return NULL;
}
