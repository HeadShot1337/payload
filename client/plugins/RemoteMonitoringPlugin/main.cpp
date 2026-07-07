#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <propidl.h>
#include <objidl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using json = nlohmann::json;
using namespace std;

// GUIDs
// MFVideoFormat_HEVC: {35363248-0000-0010-8000-00AA00389B71}
static const GUID CLSID_CMSH265EncoderMFT = { 0x2c417f4d, 0x194d, 0x47e1, { 0xb2, 0x01, 0x3d, 0xb8, 0x48, 0x3a, 0x7c, 0x98 } };
static const GUID MyMFVideoFormat_HEVC = { 0x35363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct MonitorData {
    RECT rect{};
    string deviceName;
    string displayName;
    bool primary = false;
};

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t signature;
    uint8_t type;
    uint32_t size;
};

struct MonitorFrameHeader {
    uint32_t monitor;
    uint32_t scale;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t dataSize;
};
#pragma pack(pop)

static const uint16_t PACKET_SIGNATURE = 0x524E;
static const uint8_t PACKET_TYPE_MONITOR_FRAME = 0x03;
static const uint32_t MONITOR_FRAME_FORMAT_H265 = 3;

static atomic_bool g_captureRunning(false);
static thread g_captureThread;
static mutex g_captureMutex;
static mutex g_sendMutex;
static SOCKET g_captureSocket = INVALID_SOCKET;
static int g_monitorIndex = 0;
static int g_scalePercent = 50;
static int g_targetFps = 20;
static RECT g_captureRect{};
static bool g_hasCaptureRect = false;

class H265Encoder {
    IMFTransform* m_pTransform = nullptr;
    IMFMediaType* m_pInputType = nullptr;
    IMFMediaType* m_pOutputType = nullptr;
    DWORD m_inputStreamID = 0;
    DWORD m_outputStreamID = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;

public:
    H265Encoder() {}
    ~H265Encoder() { Shutdown(); }

    bool Initialize(int width, int height, int fps) {
        Shutdown();
        m_width = (width + 1) & ~1;
        m_height = (height + 1) & ~1;

        HRESULT hr = CoCreateInstance(CLSID_CMSH265EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)&m_pTransform);
        if (FAILED(hr)) return false;

        hr = MFCreateMediaType(&m_pOutputType);
        if (FAILED(hr)) return false;
        m_pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_pOutputType->SetGUID(MF_MT_SUBTYPE, MyMFVideoFormat_HEVC);
        m_pOutputType->SetUINT32(MF_MT_AVG_BITRATE, 1000000); // 1 Mbps fallback
        MFSetAttributeSize(m_pOutputType, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(m_pOutputType, MF_MT_FRAME_RATE, fps, 1);
        m_pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = m_pTransform->SetOutputType(m_outputStreamID, m_pOutputType, 0);
        if (FAILED(hr)) return false;

        hr = MFCreateMediaType(&m_pInputType);
        if (FAILED(hr)) return false;
        m_pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(m_pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(m_pInputType, MF_MT_FRAME_RATE, fps, 1);
        m_pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = m_pTransform->SetInputType(m_inputStreamID, m_pInputType, 0);
        if (FAILED(hr)) return false;

        hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        if (m_pTransform) {
            m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_pTransform->Release();
            m_pTransform = nullptr;
        }
        if (m_pInputType) { m_pInputType->Release(); m_pInputType = nullptr; }
        if (m_pOutputType) { m_pOutputType->Release(); m_pOutputType = nullptr; }
        m_initialized = false;
    }

    bool Encode(const vector<uint8_t>& nv12Data, vector<uint8_t>& output) {
        if (!m_initialized) return false;

        IMFSample* pSample = nullptr;
        IMFMediaBuffer* pBuffer = nullptr;
        HRESULT hr = MFCreateSample(&pSample);
        if (FAILED(hr)) return false;

        hr = MFCreateMemoryBuffer((DWORD)nv12Data.size(), &pBuffer);
        if (FAILED(hr)) { pSample->Release(); return false; }

        BYTE* pData = nullptr;
        hr = pBuffer->Lock(&pData, NULL, NULL);
        if (SUCCEEDED(hr)) {
            memcpy(pData, nv12Data.data(), nv12Data.size());
            pBuffer->Unlock();
            pBuffer->SetCurrentLength((DWORD)nv12Data.size());
        }
        pSample->AddBuffer(pBuffer);
        pBuffer->Release();

        hr = m_pTransform->ProcessInput(m_inputStreamID, pSample, 0);
        pSample->Release();
        if (FAILED(hr)) return false;

        MFT_OUTPUT_DATA_BUFFER outputDataBuffer = { 0 };
        outputDataBuffer.dwStreamID = m_outputStreamID;

        DWORD status = 0;
        hr = m_pTransform->ProcessOutput(0, 1, &outputDataBuffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
        if (FAILED(hr)) return false;

        if (outputDataBuffer.pSample) {
            IMFMediaBuffer* pOutBuffer = nullptr;
            hr = outputDataBuffer.pSample->ConvertToContiguousBuffer(&pOutBuffer);
            if (SUCCEEDED(hr)) {
                BYTE* pOutData = nullptr;
                DWORD outLen = 0;
                hr = pOutBuffer->Lock(&pOutData, NULL, &outLen);
                if (SUCCEEDED(hr)) {
                    output.assign(pOutData, pOutData + outLen);
                    pOutBuffer->Unlock();
                }
                pOutBuffer->Release();
            }
            outputDataBuffer.pSample->Release();
        }
        if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();

        return !output.empty();
    }
};

static void BGRXToNV12(int width, int height, const uint8_t* bgrx, uint8_t* nv12) {
    int y_size = width * height;
    uint8_t* y_plane = nv12;
    uint8_t* uv_plane = nv12 + y_size;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t* p = bgrx + (y * width + x) * 4;
            uint8_t B = p[0];
            uint8_t G = p[1];
            uint8_t R = p[2];

            y_plane[y * width + x] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);

            if (y % 2 == 0 && x % 2 == 0) {
                int uv_idx = (y / 2) * width + x;
                uv_plane[uv_idx] = (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);   // U
                uv_plane[uv_idx + 1] = (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128); // V
            }
        }
    }
}

static bool safe_send_raw(SOCKET sock, const string& data) {
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

static bool safe_send_json(SOCKET sock, const json& data) {
    lock_guard<mutex> lock(g_sendMutex);
    string serialized = data.dump(-1, ' ', false, json::error_handler_t::replace);
    return safe_send_raw(sock, serialized + "\r\n");
}

static bool safe_send_monitor_frame(SOCKET sock, int monitor, int scale, int fps, int width, int height, const vector<unsigned char>& data) {
    if (data.empty()) return false;
    MonitorFrameHeader frameHeader{};
    frameHeader.monitor = (uint32_t)max(0, monitor);
    frameHeader.scale = (uint32_t)max(0, scale);
    frameHeader.fps = (uint32_t)max(0, fps);
    frameHeader.width = (uint32_t)max(0, width);
    frameHeader.height = (uint32_t)max(0, height);
    frameHeader.format = MONITOR_FRAME_FORMAT_H265;
    frameHeader.dataSize = (uint32_t)data.size();
    PacketHeader packetHeader{};
    packetHeader.signature = PACKET_SIGNATURE;
    packetHeader.type = PACKET_TYPE_MONITOR_FRAME;
    packetHeader.size = (uint32_t)(sizeof(MonitorFrameHeader) + data.size());
    string packet;
    packet.resize(sizeof(PacketHeader) + packetHeader.size);
    memcpy(&packet[0], &packetHeader, sizeof(PacketHeader));
    memcpy(&packet[sizeof(PacketHeader)], &frameHeader, sizeof(MonitorFrameHeader));
    memcpy(&packet[sizeof(PacketHeader) + sizeof(MonitorFrameHeader)], data.data(), data.size());
    lock_guard<mutex> lock(g_sendMutex);
    return safe_send_raw(sock, packet);
}

static void send_monitor_error(SOCKET sock, const string& message) {
    json response;
    response["action"] = "monitorerror";
    response["error"] = message;
    safe_send_json(sock, response);
}

static string get_display_name(const char* deviceName) {
    if (!deviceName || deviceName[0] == '\0') return "";
    DISPLAY_DEVICEA displayDevice{};
    displayDevice.cb = sizeof(displayDevice);
    if (EnumDisplayDevicesA(deviceName, 0, &displayDevice, 0) && displayDevice.DeviceString[0] != '\0')
        return string(displayDevice.DeviceString);
    return "";
}

static BOOL CALLBACK enum_monitor_proc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    vector<MonitorData>* monitors = reinterpret_cast<vector<MonitorData>*>(userData);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) return TRUE;
    MonitorData item;
    item.rect = info.rcMonitor;
    item.deviceName = info.szDevice;
    item.displayName = get_display_name(info.szDevice);
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (item.displayName.empty()) item.displayName = item.deviceName.empty() ? "Monitor" : item.deviceName;
    monitors->push_back(item);
    return TRUE;
}

static vector<MonitorData> enumerate_monitors() {
    vector<MonitorData> monitors;
    EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        MonitorData fallback;
        fallback.rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        fallback.rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        fallback.rect.right = fallback.rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        fallback.rect.bottom = fallback.rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        fallback.displayName = "Virtual Screen";
        fallback.primary = true;
        monitors.push_back(fallback);
    }
    return monitors;
}

static void send_monitor_list(SOCKET sock) {
    vector<MonitorData> monitors = enumerate_monitors();
    json response;
    response["action"] = "monitorlistresponse";
    response["monitors"] = json::array();
    for (size_t i = 0; i < monitors.size(); ++i) {
        const MonitorData& monitor = monitors[i];
        int width = monitor.rect.right - monitor.rect.left;
        int height = monitor.rect.bottom - monitor.rect.top;
        string name = monitor.displayName.empty() ? ("Monitor " + to_string(i + 1)) : monitor.displayName;
        if (monitor.primary) name += " (Primary)";
        response["monitors"].push_back({
            {"index", (int)i}, {"name", name}, {"device", monitor.deviceName},
            {"left", monitor.rect.left}, {"top", monitor.rect.top},
            {"width", width}, {"height", height}, {"primary", monitor.primary}
        });
    }
    safe_send_json(sock, response);
}

static int clamp_int(int value, int minValue, int maxValue) {
    return max(minValue, min(value, maxValue));
}

static bool capture_monitor_pixels(const RECT& rect, int scalePercent, vector<uint8_t>& bgrx, int& outW, int& outH, string& error) {
    int sw = rect.right - rect.left;
    int sh = rect.bottom - rect.top;
    outW = (sw * scalePercent) / 100;
    outH = (sh * scalePercent) / 100;
    outW = (outW + 1) & ~1;
    outH = (outH + 1) & ~1;

    HDC sDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(sDC);
    HBITMAP bmp = CreateCompatibleBitmap(sDC, outW, outH);
    HGDIOBJ old = SelectObject(mDC, bmp);
    SetStretchBltMode(mDC, COLORONCOLOR);
    StretchBlt(mDC, 0, 0, outW, outH, sDC, rect.left, rect.top, sw, sh, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outW;
    bmi.bmiHeader.biHeight = -outH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    bgrx.resize(outW * outH * 4);
    GetDIBits(mDC, bmp, 0, outH, bgrx.data(), &bmi, DIB_RGB_COLORS);

    SelectObject(mDC, old);
    DeleteObject(bmp);
    DeleteDC(mDC);
    ReleaseDC(NULL, sDC);
    return true;
}

static void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    H265Encoder encoder;
    int curW = 0, curH = 0;

    while (g_captureRunning.load()) {
        DWORD start = GetTickCount();
        SOCKET sock; int monitor, scale, fps; RECT rect; bool has;
        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket; monitor = g_monitorIndex; scale = g_scalePercent;
            fps = g_targetFps; rect = g_captureRect; has = g_hasCaptureRect;
        }
        if (!has) { g_captureRunning.store(false); break; }

        vector<uint8_t> bgrx, nv12, h265;
        int w, h; string err;
        if (capture_monitor_pixels(rect, scale, bgrx, w, h, err)) {
            if (w != curW || h != curH) {
                encoder.Initialize(w, h, fps);
                curW = w; curH = h;
            }
            nv12.resize(w * h * 3 / 2);
            BGRXToNV12(w, h, bgrx.data(), nv12.data());
            if (encoder.Encode(nv12, h265)) {
                if (!safe_send_monitor_frame(sock, monitor, scale, fps, w, h, h265)) {
                    g_captureRunning.store(false); break;
                }
            }
        }
        DWORD elapsed = GetTickCount() - start;
        DWORD interval = 1000 / clamp_int(fps, 1, 60);
        if (elapsed < interval) Sleep(interval - elapsed);
    }
}

static void stop_capture_thread() {
    g_captureRunning.store(false);
    if (g_captureThread.joinable()) {
        if (g_captureThread.get_id() == this_thread::get_id()) g_captureThread.detach();
        else g_captureThread.join();
    }
    lock_guard<mutex> lock(g_captureMutex);
    g_hasCaptureRect = false;
}

static void start_capture(SOCKET sock, int monitorIndex, int scalePercent, int requestedFps) {
    scalePercent = clamp_int(scalePercent, 10, 100);
    int fps = requestedFps > 0 ? clamp_int(requestedFps, 1, 60) : 25;
    vector<MonitorData> monitors = enumerate_monitors();
    if (monitors.empty()) { send_monitor_error(sock, "No monitors"); return; }
    int idx = clamp_int(monitorIndex, 0, (int)monitors.size() - 1);
    {
        lock_guard<mutex> lock(g_captureMutex);
        g_captureSocket = sock; g_monitorIndex = idx; g_scalePercent = scalePercent;
        g_targetFps = fps; g_captureRect = monitors[idx].rect; g_hasCaptureRect = true;
    }
    if (!g_captureRunning.exchange(true)) {
        if (g_captureThread.joinable()) g_captureThread.join();
        g_captureThread = thread(capture_loop);
    }
    json res; res["action"] = "monitorstatus"; res["status"] = "started";
    res["monitor"] = idx; res["scale"] = scalePercent; res["fps"] = fps;
    res["codec"] = "h265";
    safe_send_json(sock, res);
}

static void simulate_mouse(const string& event, int button, int x, int y) {
    RECT rect{};
    { lock_guard<mutex> lock(g_captureMutex); if (!g_hasCaptureRect) return; rect = g_captureRect; }
    int sx = rect.left + MulDiv(x, rect.right - rect.left, 65535);
    int sy = rect.top + MulDiv(y, rect.bottom - rect.top, 65535);
    int vl = GetSystemMetrics(SM_XVIRTUALSCREEN); int vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN); int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return;
    INPUT in = { 0 }; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((sx - vl) * (65535.0 / (vw - 1)));
    in.mi.dy = (LONG)((sy - vt) * (65535.0 / (vh - 1)));
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    if (event == "move") in.mi.dwFlags |= MOUSEEVENTF_MOVE;
    else if (event == "down") {
        if (button == 0) in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        else if (button == 1) in.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
        else if (button == 2) in.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
    } else if (event == "up") {
        if (button == 0) in.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        else if (button == 1) in.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        else if (button == 2) in.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
    }
    SendInput(1, &in, sizeof(INPUT));
}

static void simulate_key(const string& event, int vk) {
    INPUT in = { 0 }; in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)vk;
    if (event == "up") in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) { send_monitor_list(sock); }
extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* jsonStr) {
    try {
        json cmd = json::parse(jsonStr ? jsonStr : "{}");
        string act = cmd.value("action", "");
        if (act == "monitorlist") send_monitor_list(sock);
        else if (act == "monitorstart") start_capture(sock, cmd.value("monitor", 0), cmd.value("scale", 50), cmd.value("fps", 0));
        else if (act == "monitorstop") {
            stop_capture_thread();
            json res; res["action"] = "monitorstatus"; res["status"] = "stopped";
            safe_send_json(sock, res);
        } else if (act == "mouseevent") simulate_mouse(cmd.value("event", ""), cmd.value("button", 0), cmd.value("x", 0), cmd.value("y", 0));
        else if (act == "keyevent") simulate_key(cmd.value("event", ""), cmd.value("key", 0));
    } catch (...) {}
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { MFStartup(0x00020070); }
    if (reason == DLL_PROCESS_DETACH) { g_captureRunning.store(false); MFShutdown(); }
    return TRUE;
}
