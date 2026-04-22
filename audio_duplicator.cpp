// audio_duplicator.cpp
// Duplicates audio from the default Windows playback device to any secondary
// audio device — with near-zero latency, no virtual cables, no extra drivers.
//
// Uses WASAPI loopback capture + event-driven render.
// Runs silently in the background with a system tray icon to stop it.
//
// BUILD (MSVC — Developer Command Prompt):
//   cl /EHsc /O2 audio_duplicator.cpp /link ole32.lib mmdevapi.lib avrt.lib
//
// BUILD (MinGW / g++):
//   g++ -std=c++17 -O2 -o audio_duplicator.exe audio_duplicator.cpp -lole32 -lmmdevapi -lavrt -luuid
//
// USAGE:
//   audio_duplicator.exe [device-name-substring]
//   e.g.:  audio_duplicator.exe "USB Audio"
//   If no argument is given, defaults to searching for "Speakers".

// ── Force Unicode and WASAPI / Vista+ headers ────────────────────────────────
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#ifndef WINVER
#  define WINVER       0x0600
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif

// ── Run as a silent background app (no console window) ──────────────────────
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

// ── Required libraries ───────────────────────────────────────────────────────
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// ── Windows / COM headers ────────────────────────────────────────────────────
#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

// ── Standard headers ─────────────────────────────────────────────────────────
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

#define CHECK_HR(hr, msg)                                                        \
    if (FAILED(hr)) {                                                            \
        wchar_t _buf[256];                                                       \
        swprintf_s(_buf, L"Error: %hs\n(hr=0x%08X)", msg, (unsigned)hr);        \
        MessageBoxW(nullptr, _buf, L"Audio Duplicator", MB_OK | MB_ICONERROR);  \
        goto cleanup;                                                            \
    }

static void SafeReleaseMedia(IUnknown** pp) {
    if (pp && *pp) { (*pp)->Release(); *pp = nullptr; }
}
#define SAFE_RELEASE(p) SafeReleaseMedia((IUnknown**)&(p))

// ────────────────────────────────────────────────────────────────────────────
// Linear resampler — handles mismatched sample rates between devices
// ────────────────────────────────────────────────────────────────────────────
struct Resampler {
    WAVEFORMATEX srcFmt;
    WAVEFORMATEX dstFmt;
    double ratio;
    double phase;
    float  prevL, prevR;

    void init(const WAVEFORMATEX& src, const WAVEFORMATEX& dst) {
        srcFmt = src;  dstFmt = dst;
        ratio = (double)dst.nSamplesPerSec / src.nSamplesPerSec;
        phase = 0.0;
        prevL = prevR = 0.0f;
    }

    float readSample(const BYTE* buf, UINT32 frame, int ch) const {
        const WAVEFORMATEXTENSIBLE* wfex =
            (srcFmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&srcFmt)
            : nullptr;

        bool isFloat = wfex && (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        int bps = srcFmt.wBitsPerSample;
        int bpf = srcFmt.nBlockAlign;
        const BYTE* p = buf + (size_t)frame * bpf + (size_t)ch * (bps / 8);

        if (isFloat && bps == 32) { float v; memcpy(&v, p, 4); return v; }
        if (bps == 16) { INT16 v; memcpy(&v, p, 2); return v / 32768.0f; }
        if (bps == 24) {
            INT32 v = 0;
            memcpy(reinterpret_cast<BYTE*>(&v) + 1, p, 3);
            return v / 2147483648.0f;
        }
        if (bps == 32) { INT32 v; memcpy(&v, p, 4); return v / 2147483648.0f; }
        return 0.0f;
    }

    void writeSample(BYTE* buf, UINT32 frame, int ch, float val) const {
        int bpf = dstFmt.nBlockAlign;
        int bps = dstFmt.wBitsPerSample;
        BYTE* p = buf + (size_t)frame * bpf + (size_t)ch * (bps / 8);
        if (bps == 32) { memcpy(p, &val, 4); }
        else if (bps == 16) {
            val = val < -1.0f ? -1.0f : (val > 1.0f ? 1.0f : val);
            INT16 v = (INT16)(val * 32767.0f);
            memcpy(p, &v, 2);
        }
    }

    UINT32 process(const BYTE* srcBuf, UINT32 srcFrames,
        BYTE* dstBuf, UINT32 dstMaxFrames)
    {
        int srcCh = srcFmt.nChannels;
        int dstCh = dstFmt.nChannels;
        UINT32 dstFrame = 0;

        while (dstFrame < dstMaxFrames) {
            double srcPos = phase;
            UINT32 idx0 = (UINT32)srcPos;
            UINT32 idx1 = idx0 + 1;
            float  frac = (float)(srcPos - idx0);

            if (idx0 >= srcFrames) break;

            float s0L = readSample(srcBuf, idx0, 0);
            float s1L = (idx1 < srcFrames) ? readSample(srcBuf, idx1, 0) : s0L;
            float outL = s0L + frac * (s1L - s0L);

            float outR = outL;
            if (srcCh >= 2) {
                float s0R = readSample(srcBuf, idx0, 1);
                float s1R = (idx1 < srcFrames) ? readSample(srcBuf, idx1, 1) : s0R;
                outR = s0R + frac * (s1R - s0R);
            }

            writeSample(dstBuf, dstFrame, 0, outL);
            if (dstCh >= 2) writeSample(dstBuf, dstFrame, 1, outR);

            ++dstFrame;
            phase += 1.0 / ratio;
        }

        if (srcFrames > 0) {
            prevL = readSample(srcBuf, srcFrames - 1, 0);
            prevR = (srcCh >= 2) ? readSample(srcBuf, srcFrames - 1, 1) : prevL;
        }
        phase -= (UINT32)phase;
        return dstFrame;
    }
};

// ────────────────────────────────────────────────────────────────────────────
// Find a render device by friendly name substring
// ────────────────────────────────────────────────────────────────────────────
static IMMDevice* FindDeviceByName(IMMDeviceEnumerator* pEnum,
    const wchar_t* substring)
{
    IMMDeviceCollection* pColl = nullptr;
    IMMDevice* pDev = nullptr;
    IMMDevice* pFound = nullptr;

    HRESULT hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr)) return nullptr;

    UINT count = 0;
    pColl->GetCount(&count);

    for (UINT i = 0; i < count && !pFound; ++i) {
        hr = pColl->Item(i, &pDev);
        if (FAILED(hr)) continue;

        IPropertyStore* pProps = nullptr;
        hr = pDev->OpenPropertyStore(STGM_READ, &pProps);
        if (SUCCEEDED(hr)) {
            PROPVARIANT var;
            PropVariantInit(&var);
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &var);
            if (SUCCEEDED(hr) && var.vt == VT_LPWSTR) {
                std::wstring name(var.pwszVal);
                std::wstring sub(substring);
                for (auto& c : name) c = towlower(c);
                for (auto& c : sub)  c = towlower(c);
                if (name.find(sub) != std::wstring::npos) {
                    pFound = pDev;
                    pDev = nullptr;
                }
                PropVariantClear(&var);
            }
            pProps->Release();
        }
        SAFE_RELEASE(pDev);
    }
    SAFE_RELEASE(pColl);
    return pFound;
}

// ────────────────────────────────────────────────────────────────────────────
// Global stop flag
// ────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop(false);

// ────────────────────────────────────────────────────────────────────────────
// Audio loop — runs on a dedicated high-priority thread
// ────────────────────────────────────────────────────────────────────────────
struct AudioLoop {
    IAudioClient* pCapClient = nullptr;
    IAudioClient* pRenClient = nullptr;
    IAudioCaptureClient* pCapture = nullptr;
    IAudioRenderClient* pRender = nullptr;
    HANDLE               hCapEvent = nullptr;
    HANDLE               hRenEvent = nullptr;
    WAVEFORMATEX* pCapFmt = nullptr;
    WAVEFORMATEX* pRenFmt = nullptr;
    bool                 needResample = false;
    Resampler            resampler;

    static const size_t SCRATCH_BYTES = 1024 * 1024;
    std::vector<BYTE>   scratch;

    void run() {
        scratch.resize(SCRATCH_BYTES, 0);

        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        pCapClient->Start();
        pRenClient->Start();

        HANDLE waitHandles[2] = { hCapEvent, hRenEvent };

        while (!g_stop) {
            DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, 50);
            if (w == WAIT_TIMEOUT) continue;
            if (w == WAIT_FAILED)  break;

            UINT32 packetSize = 0;
            while (SUCCEEDED(pCapture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
                BYTE* pData = nullptr;
                UINT32 frames = 0;
                DWORD  flags = 0;

                HRESULT hr = pCapture->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                size_t byteCount = (size_t)frames * pCapFmt->nBlockAlign;

                if (silent)                        memset(scratch.data(), 0, byteCount);
                else if (byteCount <= SCRATCH_BYTES) memcpy(scratch.data(), pData, byteCount);

                pCapture->ReleaseBuffer(frames);

                UINT32 renBufSize = 0, renPadding = 0;
                hr = pRenClient->GetBufferSize(&renBufSize);   if (FAILED(hr)) break;
                hr = pRenClient->GetCurrentPadding(&renPadding); if (FAILED(hr)) break;

                UINT32 available = renBufSize - renPadding;
                if (available == 0) continue;

                BYTE* pRenData = nullptr;
                hr = pRender->GetBuffer(available, &pRenData);
                if (FAILED(hr)) break;

                UINT32 framesWritten = 0;
                if (needResample) {
                    framesWritten = resampler.process(scratch.data(), frames,
                        pRenData, available);
                    if (framesWritten < available) {
                        memset(pRenData + (size_t)framesWritten * pRenFmt->nBlockAlign, 0,
                            (size_t)(available - framesWritten) * pRenFmt->nBlockAlign);
                        framesWritten = available;
                    }
                }
                else {
                    UINT32 toCopy = min(frames, available);
                    memcpy(pRenData, scratch.data(), (size_t)toCopy * pRenFmt->nBlockAlign);
                    if (toCopy < available)
                        memset(pRenData + (size_t)toCopy * pRenFmt->nBlockAlign, 0,
                            (size_t)(available - toCopy) * pRenFmt->nBlockAlign);
                    framesWritten = available;
                }

                pRender->ReleaseBuffer(framesWritten, 0);
            }
        }

        pCapClient->Stop();
        pRenClient->Stop();
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
    }
};

// ────────────────────────────────────────────────────────────────────────────
// System tray icon
// ────────────────────────────────────────────────────────────────────────────
#define WM_TRAY       (WM_USER + 1)
#define ID_TRAY_STOP  1001

static NOTIFYICONDATAW g_nid = {};
static HWND            g_hWnd = nullptr;

static LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_STOP, L"Stop Audio Duplicator");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == ID_TRAY_STOP)
                PostMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        break;

    case WM_CLOSE:
    case WM_DESTROY:
        g_stop = true;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static void RunTrayMessageLoop()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AudioDuplicatorTray";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, L"AudioDuplicatorTray", L"Audio Duplicator",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Audio Duplicator — running");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// main()
// ────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // Default target: "Speakers" — override with command-line argument
    const wchar_t* targetSubstring = L"Speaker";
    std::wstring   targetBuf;

    if (argc >= 2) {
        int len = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        targetBuf.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &targetBuf[0], len);
        targetSubstring = targetBuf.c_str();
    }

    HRESULT hr = S_OK;

    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pCapDev = nullptr;
    IMMDevice* pRenDev = nullptr;
    IAudioClient* pCapClient = nullptr;
    IAudioClient* pRenClient = nullptr;
    IAudioCaptureClient* pCapture = nullptr;
    IAudioRenderClient* pRender = nullptr;
    WAVEFORMATEX* pCapFmt = nullptr;
    WAVEFORMATEX* pRenFmt = nullptr;
    HANDLE               hCapEvent = nullptr;
    HANDLE               hRenEvent = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    CHECK_HR(hr, "CoCreateInstance MMDeviceEnumerator");

    // Capture: loopback from default render endpoint
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pCapDev);
    CHECK_HR(hr, "GetDefaultAudioEndpoint");

    hr = pCapDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pCapClient);
    CHECK_HR(hr, "Activate capture IAudioClient");

    hr = pCapClient->GetMixFormat(&pCapFmt);
    CHECK_HR(hr, "GetMixFormat capture");

    hCapEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = pCapClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        100000, 0, pCapFmt, nullptr);
    CHECK_HR(hr, "Initialize capture IAudioClient");

    hr = pCapClient->SetEventHandle(hCapEvent);
    CHECK_HR(hr, "SetEventHandle capture");

    hr = pCapClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCapture);
    CHECK_HR(hr, "GetService IAudioCaptureClient");

    // Render: target device
    pRenDev = FindDeviceByName(pEnum, targetSubstring);
    if (!pRenDev) {
        wchar_t _buf[256];
        swprintf_s(_buf, L"Could not find audio device containing \"%s\".\n\nCheck the device name in Windows Sound settings.", targetSubstring);
        MessageBoxW(nullptr, _buf, L"Audio Duplicator", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    hr = pRenDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pRenClient);
    CHECK_HR(hr, "Activate render IAudioClient");

    hr = pRenClient->GetMixFormat(&pRenFmt);
    CHECK_HR(hr, "GetMixFormat render");

    hRenEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = pRenClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        100000, 0, pRenFmt, nullptr);
    CHECK_HR(hr, "Initialize render IAudioClient");

    hr = pRenClient->SetEventHandle(hRenEvent);
    CHECK_HR(hr, "SetEventHandle render");

    hr = pRenClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender);
    CHECK_HR(hr, "GetService IAudioRenderClient");

    {
        AudioLoop loop;
        loop.pCapClient = pCapClient;
        loop.pRenClient = pRenClient;
        loop.pCapture = pCapture;
        loop.pRender = pRender;
        loop.hCapEvent = hCapEvent;
        loop.hRenEvent = hRenEvent;
        loop.pCapFmt = pCapFmt;
        loop.pRenFmt = pRenFmt;
        loop.needResample = (pCapFmt->nSamplesPerSec != pRenFmt->nSamplesPerSec ||
            pCapFmt->nChannels != pRenFmt->nChannels);

        if (loop.needResample)
            loop.resampler.init(*pCapFmt, *pRenFmt);

        std::thread audioThread([&loop]() { loop.run(); });

        RunTrayMessageLoop(); // blocks until user clicks "Stop"

        g_stop = true;
        audioThread.join();
    }

cleanup:
    if (pCapFmt)  CoTaskMemFree(pCapFmt);
    if (pRenFmt)  CoTaskMemFree(pRenFmt);
    if (hCapEvent) CloseHandle(hCapEvent);
    if (hRenEvent) CloseHandle(hRenEvent);
    SAFE_RELEASE(pCapture);
    SAFE_RELEASE(pRender);
    SAFE_RELEASE(pCapClient);
    SAFE_RELEASE(pRenClient);
    SAFE_RELEASE(pCapDev);
    SAFE_RELEASE(pRenDev);
    SAFE_RELEASE(pEnum);
    CoUninitialize();
    return 0;
}