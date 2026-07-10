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

// Media Foundation minimal headers we can include or define
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

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
static mutex g_encoderMutex; // Protects g_videoEncoder and its session tracking state

static SOCKET g_captureSocket = INVALID_SOCKET;
static int g_monitorIndex = 0;
static int g_scalePercent = 50;
static int g_targetFps = 20;
static int g_videoFormat = 1; // 1 = JPEG, 2 = H.264, 3 = H.265/HEVC
static RECT g_captureRect{};
static bool g_hasCaptureRect = false;

static int g_lastWidth = 0;
static int g_lastHeight = 0;
static int g_lastFormat = 0;
static bool g_encoderInitialized = false;

static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

// Manual definition of IMFActivate to support all compiler environments
#ifndef __IMFActivate_INTERFACE_DEFINED__
#define __IMFActivate_INTERFACE_DEFINED__
interface IMFActivate : public IMFAttributes {
    virtual HRESULT STDMETHODCALLTYPE ActivateObject(REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE ShutdownObject(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE DetachObject(void) = 0;
};
#endif

// Dynamic loader for Media Foundation functions
typedef HRESULT (WINAPI *MFStartupFn)(ULONG Version, DWORD dwFlags);
typedef HRESULT (WINAPI *MFShutdownFn)();
typedef HRESULT (WINAPI *MFCreateMediaTypeFn)(IMFMediaType** ppMFType);
typedef HRESULT (WINAPI *MFCreateMemoryBufferFn)(DWORD cbMaxLength, IMFMediaBuffer** ppBuffer);
typedef HRESULT (WINAPI *MFCreateSampleFn)(IMFSample** ppSample);
typedef HRESULT (WINAPI *MFTEnumExFn)(
    GUID guidCategory,
    UINT32 Flags,
    const MFT_REGISTER_TYPE_INFO *pInputType,
    const MFT_REGISTER_TYPE_INFO *pOutputType,
    IMFActivate ***pppMFTActivate,
    UINT32 *pnumMFTActivate
);

static MFStartupFn pMFStartup = nullptr;
static MFShutdownFn pMFShutdown = nullptr;
static MFCreateMediaTypeFn pMFCreateMediaType = nullptr;
static MFCreateMemoryBufferFn pMFCreateMemoryBuffer = nullptr;
static MFCreateSampleFn pMFCreateSample = nullptr;
static MFTEnumExFn pMFTEnumEx = nullptr;
static bool g_mfLoaded = false;

static bool load_media_foundation() {
    if (g_mfLoaded) return true;
    HMODULE hMfplat = LoadLibraryA("mfplat.dll");
    if (!hMfplat) return false;

    pMFStartup = (MFStartupFn)GetProcAddress(hMfplat, "MFStartup");
    pMFShutdown = (MFShutdownFn)GetProcAddress(hMfplat, "MFShutdown");
    pMFCreateMediaType = (MFCreateMediaTypeFn)GetProcAddress(hMfplat, "MFCreateMediaType");
    pMFCreateMemoryBuffer = (MFCreateMemoryBufferFn)GetProcAddress(hMfplat, "MFCreateMemoryBuffer");
    pMFCreateSample = (MFCreateSampleFn)GetProcAddress(hMfplat, "MFCreateSample");
    pMFTEnumEx = (MFTEnumExFn)GetProcAddress(hMfplat, "MFTEnumEx");

    if (pMFStartup && pMFShutdown && pMFCreateMediaType && pMFCreateMemoryBuffer && pMFCreateSample) {
        g_mfLoaded = true;
        return true;
    }
    return false;
}

// Manual GUID definitions to avoid linker errors
#define OUR_DEFINE_MEDIATYPE_GUID(name, format) \
    static const GUID name = { format, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }

OUR_DEFINE_MEDIATYPE_GUID(My_MFMediaType_Video, 0x73646976); // 'vids'
OUR_DEFINE_MEDIATYPE_GUID(My_MFVideoFormat_NV12, 0x3231564e); // 'NV12'
OUR_DEFINE_MEDIATYPE_GUID(My_MFVideoFormat_H264, 0x34363248); // 'H264'
OUR_DEFINE_MEDIATYPE_GUID(My_MFVideoFormat_HEVC, 0x43564548); // 'HEVC'

static const GUID My_MF_MT_MAJOR_TYPE = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
static const GUID My_MF_MT_SUBTYPE = { 0xf7e34c9a, 0x425f, 0x4e15, { 0xaa, 0x3c, 0x5a, 0x22, 0x4d, 0x3f, 0xd0, 0xbc } };
static const GUID My_MF_MT_FRAME_SIZE = { 0x16a0eb0e, 0x6e40, 0x4e0a, { 0x9d, 0xdb, 0x05, 0xfa, 0x91, 0xa1, 0x07, 0x7f } };
static const GUID My_MF_MT_FRAME_RATE = { 0xc459a2e5, 0x4358, 0x452c, { 0x8b, 0x00, 0xdb, 0x97, 0xc2, 0x53, 0x7e, 0x22 } };
static const GUID My_MF_MT_PIXEL_ASPECT_RATIO = { 0xc63db510, 0xc8e4, 0x4243, { 0x99, 0x0d, 0x56, 0x07, 0x3d, 0xf0, 0xff, 0xd4 } };
static const GUID My_MF_MT_INTERLACE_MODE = { 0xe2724d48, 0xae60, 0x4758, { 0x94, 0xb8, 0xbc, 0xda, 0x8e, 0x3e, 0x24, 0xe1 } };
static const GUID My_MF_MT_AVG_BITRATE = { 0x20332624, 0xc891, 0x4705, { 0xa0, 0x78, 0xbc, 0x85, 0x75, 0x2f, 0x10, 0x0d } };
static const GUID My_MF_MT_VIDEO_PROFILE = { 0x96f54c4a, 0xca68, 0x450b, { 0x97, 0xbc, 0x5d, 0x2a, 0x5d, 0x43, 0xbc, 0x99 } };

static const GUID My_IID_IMFTransform = { 0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d } };
static const GUID My_IID_ICodecAPI = { 0x901db749, 0xf86f, 0x4560, { 0x96, 0xd7, 0x8a, 0x35, 0x2f, 0x0d, 0x2d, 0xb9 } };
static const GUID My_CODECAPI_AVEncMPVGOPSize = { 0x951a7a7e, 0xdcee, 0x4630, { 0xb2, 0xc4, 0x98, 0xf1, 0xb1, 0x37, 0x04, 0x16 } };
static const GUID My_CODECAPI_AVLowLatencyMode = { 0x9c3893c6, 0x7538, 0x4a92, { 0xa5, 0x50, 0x60, 0xaf, 0xcb, 0x54, 0x88, 0x5a } };

static const GUID My_MFT_CATEGORY_VIDEO_ENCODER = { 0xf79e49c1, 0x00dd, 0x432f, { 0x99, 0x08, 0x27, 0xc8, 0x13, 0x4c, 0x40, 0xf6 } };

static const CLSID My_CLSID_CMSH264EncoderMFT = { 0x6ca50380, 0x1114, 0x4159, { 0x83, 0x93, 0x44, 0xfe, 0x3e, 0x1a, 0x3c, 0xe9 } };
static const CLSID My_CLSID_CMSH265EncoderMFT = { 0x2c417f4d, 0x1abd, 0x433c, { 0xab, 0xb5, 0x97, 0xb1, 0xc4, 0x69, 0x71, 0xe2 } };

// Manually define ICodecAPI interface if not declared in current compiler environment
#ifndef __ICodecAPI_INTERFACE_DEFINED__
#define __ICodecAPI_INTERFACE_DEFINED__
interface ICodecAPI : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID* Api, BOOL* IsSupported) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID* Api, BOOL* IsModifiable) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID* Api, VARIANT* ValueMin, VARIANT* ValueMax, VARIANT* SteppingDelta) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID* Api, VARIANT** Values, ULONG* ValuesCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID* Api, LONG_PTR UserData) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID* Api) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllDefaults(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValueWithChangeSet(const GUID* Api, VARIANT* Value, const GUID* ChangeSet, ULONG ChangeSetCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullChangeSet(VARIANT* Value, VARIANT* ChangeSet, ULONG* ChangeSetCount) = 0;
};
#endif

static string hr_to_hex(HRESULT hr) {
    char buf[32];
    sprintf(buf, "0x%08X", (unsigned int)hr);
    return string(buf);
}

// Video conversion helper: BGRX to NV12
static void BGRX_to_NV12(const uint8_t* bgrx, int width, int height, uint8_t* nv12) {
    int y_size = width * height;
    uint8_t* y_plane = nv12;
    uint8_t* uv_plane = nv12 + y_size;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_idx = (y * width + x) * 4;
            uint8_t b = bgrx[src_idx];
            uint8_t g = bgrx[src_idx + 1];
            uint8_t r = bgrx[src_idx + 2];

            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[y * width + x] = (uint8_t)max(0, min(Y, 255));

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int uv_idx = (y / 2) * width + x;
                uv_plane[uv_idx] = (uint8_t)max(0, min(U, 255));
                uv_plane[uv_idx + 1] = (uint8_t)max(0, min(V, 255));
            }
        }
    }
}

class VideoEncoder {
private:
    IMFTransform* m_mft = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_format = 0; // 2 = H.264, 3 = H.265/HEVC
    uint64_t m_frameCount = 0;
    bool m_initialized = false;

public:
    VideoEncoder() {}
    ~VideoEncoder() {
        Cleanup();
    }

    void Cleanup() {
        if (m_mft) {
            m_mft->Release();
            m_mft = nullptr;
        }
        m_initialized = false;
        m_width = 0;
        m_height = 0;
        m_format = 0;
    }

    bool Init(int width, int height, int format, int fps, string& errorMsg) {
        Cleanup();

        if (format != 2 && format != 3) {
            errorMsg = "Invalid video format";
            return false;
        }

        if (!load_media_foundation()) {
            errorMsg = "Failed to load mfplat.dll";
            return false;
        }

        CoInitialize(NULL);
        HRESULT hr = pMFStartup(MF_VERSION, 0);
        if (FAILED(hr)) {
            errorMsg = "MFStartup failed: " + hr_to_hex(hr);
            return false;
        }

        bool activated = false;

        // Try dual-lookup: First attempt via official MFTEnumEx (best for synchronous/hardware/software portability)
        if (pMFTEnumEx) {
            MFT_REGISTER_TYPE_INFO input_info = { My_MFMediaType_Video, My_MFVideoFormat_NV12 };
            MFT_REGISTER_TYPE_INFO output_info = { My_MFMediaType_Video, (format == 3) ? My_MFVideoFormat_HEVC : My_MFVideoFormat_H264 };

            // Compatibility defines for older/minimal Mingw SDK headers
            #ifndef MFT_ENUM_FLAG_SYNCMFT
            #define MFT_ENUM_FLAG_SYNCMFT 0x00000001
            #endif
            #ifndef MFT_ENUM_FLAG_ASYNCMFT
            #define MFT_ENUM_FLAG_ASYNCMFT 0x00000002
            #endif
            #ifndef MFT_ENUM_FLAG_HARDWARE
            #define MFT_ENUM_FLAG_HARDWARE 0x00000004
            #endif
            #ifndef MFT_ENUM_FLAG_FIELDOFUSE
            #define MFT_ENUM_FLAG_FIELDOFUSE 0x00000008
            #endif

            UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_FIELDOFUSE;

            IMFActivate** activate_array = nullptr;
            UINT32 count = 0;
            hr = pMFTEnumEx(My_MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info, &output_info, &activate_array, &count);
            if (SUCCEEDED(hr) && count > 0) {
                hr = activate_array[0]->ActivateObject(My_IID_IMFTransform, (void**)&m_mft);
                if (SUCCEEDED(hr) && m_mft) {
                    activated = true;
                }
                for (UINT32 i = 0; i < count; i++) {
                    activate_array[i]->Release();
                }
                CoTaskMemFree(activate_array);
            }
        }

        // Fallback to CoCreateInstance on standard Microsoft Software Encoders
        if (!activated) {
            CLSID encoderClsid = (format == 3) ? My_CLSID_CMSH265EncoderMFT : My_CLSID_CMSH264EncoderMFT;
            hr = CoCreateInstance(encoderClsid, NULL, CLSCTX_INPROC_SERVER, My_IID_IMFTransform, (void**)&m_mft);
            if (FAILED(hr) || !m_mft) {
                errorMsg = "CoCreateInstance and MFTEnumEx both failed. HRESULT: " + hr_to_hex(hr);
                return false;
            }
        }

        // Configure Output Media Type
        IMFMediaType* out_type = nullptr;
        if (FAILED(pMFCreateMediaType(&out_type))) {
            errorMsg = "pMFCreateMediaType failed";
            Cleanup();
            return false;
        }

        out_type->SetGUID(My_MF_MT_MAJOR_TYPE, My_MFMediaType_Video);
        out_type->SetGUID(My_MF_MT_SUBTYPE, (format == 3) ? My_MFVideoFormat_HEVC : My_MFVideoFormat_H264);
        out_type->SetUINT64(My_MF_MT_FRAME_SIZE, ((uint64_t)width << 32) | height);
        out_type->SetUINT64(My_MF_MT_FRAME_RATE, ((uint64_t)fps << 32) | 1);
        out_type->SetUINT64(My_MF_MT_PIXEL_ASPECT_RATIO, ((uint64_t)1 << 32) | 1);
        out_type->SetUINT32(My_MF_MT_INTERLACE_MODE, 2); // Progressive

        uint32_t bitrate = width * height * 2;
        if (bitrate < 500000) bitrate = 500000;
        if (bitrate > 4000000) bitrate = 4000000;
        out_type->SetUINT32(My_MF_MT_AVG_BITRATE, bitrate);

        if (format == 3) {
            out_type->SetUINT32(My_MF_MT_VIDEO_PROFILE, 1); // Main Profile
        }

        hr = m_mft->SetOutputType(0, out_type, 0);
        out_type->Release();
        if (FAILED(hr)) {
            errorMsg = "SetOutputType failed: " + hr_to_hex(hr);
            Cleanup();
            return false;
        }

        // Configure Input Media Type
        IMFMediaType* in_type = nullptr;
        if (FAILED(pMFCreateMediaType(&in_type))) {
            errorMsg = "pMFCreateMediaType for input failed";
            Cleanup();
            return false;
        }

        in_type->SetGUID(My_MF_MT_MAJOR_TYPE, My_MFMediaType_Video);
        in_type->SetGUID(My_MF_MT_SUBTYPE, My_MFVideoFormat_NV12);
        in_type->SetUINT64(My_MF_MT_FRAME_SIZE, ((uint64_t)width << 32) | height);
        in_type->SetUINT64(My_MF_MT_FRAME_RATE, ((uint64_t)fps << 32) | 1);
        in_type->SetUINT64(My_MF_MT_PIXEL_ASPECT_RATIO, ((uint64_t)1 << 32) | 1);
        in_type->SetUINT32(My_MF_MT_INTERLACE_MODE, 2); // Progressive

        hr = m_mft->SetInputType(0, in_type, 0);
        in_type->Release();
        if (FAILED(hr)) {
            errorMsg = "SetInputType failed: " + hr_to_hex(hr);
            Cleanup();
            return false;
        }

        // Configure Codec API (GOP & Low Latency)
        ICodecAPI* codec_api = nullptr;
        if (SUCCEEDED(m_mft->QueryInterface(My_IID_ICodecAPI, (void**)&codec_api))) {
            VARIANT var;
            memset(&var, 0, sizeof(VARIANT));
            var.vt = VT_UI4;
            var.ulVal = 30; // GOP size = 30
            codec_api->SetValue(&My_CODECAPI_AVEncMPVGOPSize, &var);

            memset(&var, 0, sizeof(VARIANT));
            var.vt = VT_BOOL;
            var.boolVal = VARIANT_TRUE;
            codec_api->SetValue(&My_CODECAPI_AVLowLatencyMode, &var);
            codec_api->Release();
        }

        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_width = width;
        m_height = height;
        m_format = format;
        m_frameCount = 0;
        m_initialized = true;
        return true;
    }

    bool EncodeFrame(const uint8_t* nv12Data, int fps, vector<uint8_t>& outBytes, string& errorMsg) {
        if (!m_initialized || !m_mft) {
            errorMsg = "Encoder not initialized";
            return false;
        }

        outBytes.clear();

        // Create Input Sample
        IMFMediaBuffer* in_buffer = nullptr;
        int nv12_size = m_width * m_height * 1.5;
        HRESULT hr = pMFCreateMemoryBuffer(nv12_size, &in_buffer);
        if (FAILED(hr)) {
            errorMsg = "pMFCreateMemoryBuffer failed: " + hr_to_hex(hr);
            return false;
        }

        BYTE* pData = nullptr;
        if (SUCCEEDED(in_buffer->Lock(&pData, nullptr, nullptr))) {
            memcpy(pData, nv12Data, nv12_size);
            in_buffer->Unlock();
        }
        in_buffer->SetCurrentLength(nv12_size);

        IMFSample* in_sample = nullptr;
        hr = pMFCreateSample(&in_sample);
        if (FAILED(hr)) {
            in_buffer->Release();
            errorMsg = "pMFCreateSample failed: " + hr_to_hex(hr);
            return false;
        }
        in_sample->AddBuffer(in_buffer);
        in_buffer->Release();

        uint64_t timestamp = m_frameCount * (10000000ULL / fps);
        uint64_t duration = 10000000ULL / fps;
        in_sample->SetSampleTime(timestamp);
        in_sample->SetSampleDuration(duration);
        m_frameCount++;

        // Feed input
        hr = m_mft->ProcessInput(0, in_sample, 0);
        in_sample->Release();
        if (FAILED(hr)) {
            errorMsg = "ProcessInput failed: " + hr_to_hex(hr);
            return false;
        }

        // Read output
        MFT_OUTPUT_STREAM_INFO stream_info{};
        m_mft->GetOutputStreamInfo(0, &stream_info);

        while (true) {
            MFT_OUTPUT_DATA_BUFFER output_buffer{};
            output_buffer.dwStreamID = 0;

            IMFMediaBuffer* out_mem_buffer = nullptr;
            IMFSample* out_sample = nullptr;

            if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if (FAILED(pMFCreateSample(&out_sample))) {
                    break;
                }
                if (FAILED(pMFCreateMemoryBuffer(stream_info.cbSize, &out_mem_buffer))) {
                    out_sample->Release();
                    break;
                }
                out_sample->AddBuffer(out_mem_buffer);
                output_buffer.pSample = out_sample;
            }

            DWORD status = 0;
            hr = m_mft->ProcessOutput(0, 1, &output_buffer, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (out_mem_buffer) out_mem_buffer->Release();
                if (out_sample) out_sample->Release();
                break;
            }

            if (FAILED(hr)) {
                if (out_mem_buffer) out_mem_buffer->Release();
                if (out_sample) out_sample->Release();
                errorMsg = "ProcessOutput failed: " + hr_to_hex(hr);
                return false;
            }

            IMFSample* sample = output_buffer.pSample;
            if (sample) {
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(sample->GetBufferByIndex(0, &buf))) {
                    BYTE* dataPtr = nullptr;
                    DWORD currentLen = 0;
                    if (SUCCEEDED(buf->Lock(&dataPtr, nullptr, &currentLen))) {
                        outBytes.insert(outBytes.end(), dataPtr, dataPtr + currentLen);
                        buf->Unlock();
                    }
                    buf->Release();
                }

                if (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                    sample->Release();
                }
            }

            if (output_buffer.pEvents) {
                output_buffer.pEvents->Release();
            }

            if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if (out_mem_buffer) out_mem_buffer->Release();
                if (out_sample) out_sample->Release();
            }
        }

        return true;
    }
};

static VideoEncoder g_videoEncoder;

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
    frameHeader.format = (uint32_t)format; // 1 = JPEG, 2 = H.264, 3 = H.265
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

static bool get_raw_bitmap_pixels(HBITMAP bitmapHandle, int width, int height, vector<uint8_t>& bgrxBytes) {
    bgrxBytes.resize(width * height * 4);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(NULL);
    int lines = GetDIBits(hdc, bitmapHandle, 0, height, bgrxBytes.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);

    return lines == height;
}

static bool capture_monitor_frame(const RECT& rect,
                                  int scalePercent,
                                  int format,
                                  int fps,
                                  vector<unsigned char>& outBytes,
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

    // Media Foundation H.264/H.265 encoders require standard dimensions (at least 128x128)
    outputWidth = max(128, (sourceWidth * scalePercent) / 100);
    outputHeight = max(128, (sourceHeight * scalePercent) / 100);

    // Media Foundation H.264/H.265 encoders require even dimensions
    outputWidth = (outputWidth / 2) * 2;
    outputHeight = (outputHeight / 2) * 2;

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

    bool success = false;
    if (format == 1) { // JPEG
        ULONG jpegQuality = automatic_jpeg_quality(outputWidth, outputHeight, scalePercent);
        success = bitmap_to_jpeg(bitmap, jpegQuality, outBytes, error);
    } else { // H.264 or H.265
        vector<uint8_t> bgrx;
        if (get_raw_bitmap_pixels(bitmap, outputWidth, outputHeight, bgrx)) {
            vector<uint8_t> nv12(outputWidth * outputHeight * 3 / 2);
            BGRX_to_NV12(bgrx.data(), outputWidth, outputHeight, nv12.data());

            // Protect videoEncoder initialization and encoding against thread-race
            lock_guard<mutex> lock(g_encoderMutex);

            // Lazily initialize/reinitialize encoder if resolution or format changed
            if (!g_encoderInitialized || outputWidth != g_lastWidth || outputHeight != g_lastHeight || format != g_lastFormat) {
                g_videoEncoder.Cleanup();
                if (!g_videoEncoder.Init(outputWidth, outputHeight, format, fps, error)) {
                    DeleteObject(bitmap);
                    return false;
                }
                g_lastWidth = outputWidth;
                g_lastHeight = outputHeight;
                g_lastFormat = format;
                g_encoderInitialized = true;
            }

            success = g_videoEncoder.EncodeFrame(nv12.data(), fps, outBytes, error);
        } else {
            error = "Failed to retrieve raw pixels";
        }
    }

    DeleteObject(bitmap);
    return success;
}

static void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    CoInitialize(NULL);

    while (g_captureRunning.load()) {
        DWORD frameStart = now_ms();
        SOCKET sock = INVALID_SOCKET;
        int monitor = 0;
        int scale = 50;
        int targetFps = 20;
        int videoFormat = 1;
        RECT captureRect{};
        bool hasCaptureRect = false;

        {
            lock_guard<mutex> lock(g_captureMutex);
            sock = g_captureSocket;
            monitor = g_monitorIndex;
            scale = g_scalePercent;
            targetFps = g_targetFps;
            videoFormat = g_videoFormat;
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

        if (capture_monitor_frame(captureRect, scale, videoFormat, targetFps, frameBytes, width, height, error)) {
            // If frame was successfully processed, send it (if not buffered)
            if (frameBytes.empty()) {
                // Encoder is buffering, skip sending this frame but continue
            } else {
                if (!safe_send_monitor_frame(sock, monitor, scale, targetFps, width, height, videoFormat, frameBytes)) {
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

    // Secure the encoder cleanup and session state variables against thread-race conditions
    {
        lock_guard<mutex> enc_lock(g_encoderMutex);
        g_videoEncoder.Cleanup();
        g_lastWidth = 0;
        g_lastHeight = 0;
        g_lastFormat = 0;
        g_encoderInitialized = false;
    }
}

static void start_capture(SOCKET sock, int monitorIndex, int scalePercent, int requestedFps, int videoFormat) {
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
        g_videoFormat = videoFormat;
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
    response["format"] = videoFormat;
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
            int format = command.value("format", 1); // 1 = JPEG, 2 = H.264, 3 = H.265
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
