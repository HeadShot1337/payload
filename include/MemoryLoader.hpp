#ifndef MEMORYLOADER_HPP
#define MEMORYLOADER_HPP

#include <vector>
#include <windows.h>

class MemoryLoader {
public:
    // DLL buffer'ını belleğe yükler ve HMODULE döner
    static HMODULE Load(const std::vector<unsigned char>& buffer);

    // Manüel yüklenen modülden export adresini çeker
    static FARPROC GetExportAddress(HMODULE hMod, const char* name);
};

#endif