// audio_duplicator.cpp
// Duplicates the Windows default playback device to a selected secondary output.
// Uses WASAPI loopback capture + event-driven render and installs no drivers.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "device_selector.h"

#define CHECK_HR(hrValue, message)                                                \
    if (FAILED(hrValue)) {                                                        \
        wchar_t errorBuffer[256];                                                 \
        swprintf_s(errorBuffer, L"Error: %hs\n(hr=0x%08X)",                    \
            message, static_cast<unsigned>(hrValue));                             \
        MessageBoxW(nullptr, errorBuffer, L"Audio Duplicator",                  \
            MB_OK | MB_ICONERROR);                                                \
        goto cleanup;                                                             \
    }

static void SafeReleaseMedia(IUnknown** object) {
    if (object && *object) {
        (*object)->Release();
        *object = nullptr;
    }
}

#define SAFE_RELEASE(pointer) SafeReleaseMedia(reinterpret_cast<IUnknown**>(&(pointer)))

namespace {

struct CommandLineOptions {
    bool listDevices = false;
    bool showHelp = false;
    std::wstring targetDeviceId;
    std::wstring legacyNameSubstring;
};

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) return {};

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

bool ParseCommandLine(
    int argc,
    char* argv[],
    CommandLineOptions& options,
    std::wstring& errorMessage) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = Utf8ToWide(argv[index]);

        if (argument == L"--list-devices") {
            options.listDevices = true;
            continue;
        }
        if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            options.showHelp = true;
            continue;
        }
        if (argument == L"--device-id") {
            if (index + 1 >= argc) {
                errorMessage = L"--device-id requires a Windows audio device ID.";
                return false;
            }
            options.targetDeviceId = Utf8ToWide(argv[++index]);
            continue;
        }
        if (argument.rfind(L"--", 0) == 0) {
            errorMessage = L"Unknown option: " + argument;
            return false;
        }
        if (!options.legacyNameSubstring.empty()) {
            errorMessage = L"Only one device-name substring can be supplied.";
            return false;
        }
        options.legacyNameSubstring = argument;
    }

    if (!options.targetDeviceId.empty() && !options.legacyNameSubstring.empty()) {
        errorMessage = L"Use either --device-id or a legacy name substring, not both.";
        return false;
    }
    return true;
}

void ShowHelp() {
    MessageBoxW(
        nullptr,
        L"Audio Duplicator\n\n"
        L"Double-click the application to select a secondary output.\n"
        L"The primary output is the current Windows default playback device.\n\n"
        L"Command-line options:\n"
        L"  --list-devices          List active playback devices and IDs\n"
        L"  --device-id <ID>        Start with an exact Windows device ID\n"
        L"  <name substring>        Legacy device-name matching\n"
        L"  --help                  Show this help",
        L"Audio Duplicator",
        MB_OK | MB_ICONINFORMATION);
}

std::wstring Lowercase(std::wstring text) {
    for (wchar_t& character : text) character = static_cast<wchar_t>(towlower(character));
    return text;
}

const AudioDeviceInfo* FindDeviceByUniqueNameSubstring(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& substring,
    bool& ambiguous) {
    ambiguous = false;
    if (substring.empty()) return nullptr;

    const std::wstring loweredSubstring = Lowercase(substring);
    const AudioDeviceInfo* result = nullptr;

    for (const auto& device : devices) {
        if (Lowercase(device.name).find(loweredSubstring) == std::wstring::npos) continue;
        if (result) {
            ambiguous = true;
            return nullptr;
        }
        result = &device;
    }
    return result;
}

std::wstring GetImmDeviceId(IMMDevice* device) {
    if (!device) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

// Linear resampler retained from the original project. A later change will
// replace this with a format-aware converter and clock-drift compensation.
struct Resampler {
    WAVEFORMATEX sourceFormat = {};
    WAVEFORMATEX destinationFormat = {};
    double ratio = 1.0;
    double phase = 0.0;

    void init(const WAVEFORMATEX& source, const WAVEFORMATEX& destination) {
        sourceFormat = source;
        destinationFormat = destination;
        ratio = static_cast<double>(destination.nSamplesPerSec) /
            static_cast<double>(source.nSamplesPerSec);
        phase = 0.0;
    }

    float readSample(const BYTE* buffer, UINT32 frame, int channel) const {
        const WAVEFORMATEXTENSIBLE* extensible =
            (sourceFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&sourceFormat)
            : nullptr;

        const bool isFloat = extensible &&
            extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const int bitsPerSample = sourceFormat.wBitsPerSample;
        const BYTE* sample = buffer +
            static_cast<size_t>(frame) * sourceFormat.nBlockAlign +
            static_cast<size_t>(channel) * (bitsPerSample / 8);

        if (isFloat && bitsPerSample == 32) {
            float value = 0.0f;
            std::memcpy(&value, sample, sizeof(value));
            return value;
        }
        if (bitsPerSample == 16) {
            INT16 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return value / 32768.0f;
        }
        if (bitsPerSample == 24) {
            INT32 value = 0;
            std::memcpy(reinterpret_cast<BYTE*>(&value) + 1, sample, 3);
            return value / 2147483648.0f;
        }
        if (bitsPerSample == 32) {
            INT32 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return value / 2147483648.0f;
        }
        return 0.0f;
    }

    void writeSample(BYTE* buffer, UINT32 frame, int channel, float value) const {
        const int bitsPerSample = destinationFormat.wBitsPerSample;
        BYTE* sample = buffer +
            static_cast<size_t>(frame) * destinationFormat.nBlockAlign +
            static_cast<size_t>(channel) * (bitsPerSample / 8);

        if (bitsPerSample == 32) {
            std::memcpy(sample, &value, sizeof(value));
        } else if (bitsPerSample == 16) {
            value = (std::max)(-1.0f, (std::min)(1.0f, value));
            const INT16 integerValue = static_cast<INT16>(value * 32767.0f);
            std::memcpy(sample, &integerValue, sizeof(integerValue));
        }
    }

    UINT32 process(
        const BYTE* sourceBuffer,
        UINT32 sourceFrames,
        BYTE* destinationBuffer,
        UINT32 destinationMaxFrames) {
        const int sourceChannels = sourceFormat.nChannels;
        const int destinationChannels = destinationFormat.nChannels;
        UINT32 destinationFrame = 0;

        while (destinationFrame < destinationMaxFrames) {
            const double sourcePosition = phase;
            const UINT32 firstIndex = static_cast<UINT32>(sourcePosition);
            const UINT32 secondIndex = firstIndex + 1;
            const float fraction = static_cast<float>(sourcePosition - firstIndex);
            if (firstIndex >= sourceFrames) break;

            const float firstLeft = readSample(sourceBuffer, firstIndex, 0);
            const float secondLeft = secondIndex < sourceFrames
                ? readSample(sourceBuffer, secondIndex, 0)
                : firstLeft;
            const float outputLeft = firstLeft + fraction * (secondLeft - firstLeft);

            float outputRight = outputLeft;
            if (sourceChannels >= 2) {
                const float firstRight = readSample(sourceBuffer, firstIndex, 1);
                const float secondRight = secondIndex < sourceFrames
                    ? readSample(sourceBuffer, secondIndex, 1)
                    : firstRight;
                outputRight = firstRight + fraction * (secondRight - firstRight);
            }

            writeSample(destinationBuffer, destinationFrame, 0, outputLeft);
            if (destinationChannels >= 2) {
                writeSample(destinationBuffer, destinationFrame, 1, outputRight);
            }

            ++destinationFrame;
            phase += 1.0 / ratio;
        }

        phase -= static_cast<UINT32>(phase);
        return destinationFrame;
    }
};

static std::atomic<bool> g_stop(false);

struct AudioLoop {
    IAudioClient* captureClient = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE captureEvent = nullptr;
    HANDLE renderEvent = nullptr;
    WAVEFORMATEX* captureFormat = nullptr;
    WAVEFORMATEX* renderFormat = nullptr;
    bool needsResampling = false;
    Resampler resampler;
    std::vector<BYTE> scratch;

    void run() {
        scratch.resize(1024 * 1024, 0);

        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        if (FAILED(captureClient->Start()) || FAILED(renderClient->Start())) {
            g_stop = true;
            if (task) AvRevertMmThreadCharacteristics(task);
            return;
        }

        HANDLE waitHandles[2] = { captureEvent, renderEvent };
        while (!g_stop) {
            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 50);
            if (waitResult == WAIT_TIMEOUT) continue;
            if (waitResult == WAIT_FAILED) break;

            UINT32 packetSize = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
                BYTE* inputData = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                HRESULT hr = capture->GetBuffer(&inputData, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                const size_t byteCount = static_cast<size_t>(frames) * captureFormat->nBlockAlign;
                if (byteCount > scratch.size()) scratch.resize(byteCount);

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                    std::memset(scratch.data(), 0, byteCount);
                } else {
                    std::memcpy(scratch.data(), inputData, byteCount);
                }
                capture->ReleaseBuffer(frames);

                UINT32 renderBufferSize = 0;
                UINT32 renderPadding = 0;
                hr = renderClient->GetBufferSize(&renderBufferSize);
                if (FAILED(hr)) break;
                hr = renderClient->GetCurrentPadding(&renderPadding);
                if (FAILED(hr)) break;

                const UINT32 available = renderBufferSize - renderPadding;
                if (available == 0) continue;

                BYTE* outputData = nullptr;
                hr = render->GetBuffer(available, &outputData);
                if (FAILED(hr)) break;

                UINT32 framesWritten = 0;
                if (needsResampling) {
                    framesWritten = resampler.process(
                        scratch.data(), frames, outputData, available);
                    if (framesWritten < available) {
                        std::memset(
                            outputData + static_cast<size_t>(framesWritten) * renderFormat->nBlockAlign,
                            0,
                            static_cast<size_t>(available - framesWritten) * renderFormat->nBlockAlign);
                        framesWritten = available;
                    }
                } else {
                    const UINT32 framesToCopy = (std::min)(frames, available);
                    std::memcpy(
                        outputData,
                        scratch.data(),
                        static_cast<size_t>(framesToCopy) * renderFormat->nBlockAlign);
                    if (framesToCopy < available) {
                        std::memset(
                            outputData + static_cast<size_t>(framesToCopy) * renderFormat->nBlockAlign,
                            0,
                            static_cast<size_t>(available - framesToCopy) * renderFormat->nBlockAlign);
                    }
                    framesWritten = available;
                }

                render->ReleaseBuffer(framesWritten, 0);
            }
        }

        captureClient->Stop();
        renderClient->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
    }
};

constexpr UINT kTrayMessage = WM_USER + 1;
constexpr UINT kTrayStopCommand = 1001;
static NOTIFYICONDATAW g_trayIcon = {};
static HWND g_trayWindow = nullptr;

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kTrayStopCommand, L"Stop Audio Duplicator");
            POINT cursor = {};
            GetCursorPos(&cursor);
            SetForegroundWindow(window);
            const int command = TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_NONOTIFY,
                cursor.x, cursor.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (command == static_cast<int>(kTrayStopCommand)) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
        }
        return 0;

    case WM_CLOSE:
    case WM_DESTROY:
        g_stop = true;
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void RunTrayMessageLoop(const std::wstring& secondaryName) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"AudioDuplicatorTray";
    RegisterClassW(&windowClass);

    g_trayWindow = CreateWindowExW(
        0, L"AudioDuplicatorTray", L"Audio Duplicator",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_trayWindow;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    const std::wstring tooltip = L"Audio Duplicator -> " + secondaryName;
    wcsncpy_s(g_trayIcon.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    CommandLineOptions options;
    std::wstring argumentError;
    if (!ParseCommandLine(argc, argv, options, argumentError)) {
        MessageBoxW(nullptr, argumentError.c_str(), L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 2;
    }
    if (options.showHelp) {
        ShowHelp();
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialize COM.",
            L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* captureDevice = nullptr;
    IMMDevice* renderDevice = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* captureFormat = nullptr;
    WAVEFORMATEX* renderFormat = nullptr;
    HANDLE captureEvent = nullptr;
    HANDLE renderEvent = nullptr;
    std::vector<AudioDeviceInfo> devices;
    std::wstring enumeratedDefaultId;
    std::wstring selectedDeviceId;
    std::wstring selectedDeviceName;
    std::wstring actualDefaultId;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    CHECK_HR(hr, "CoCreateInstance MMDeviceEnumerator");

    hr = EnumerateActiveRenderDevices(enumerator, devices, enumeratedDefaultId);
    CHECK_HR(hr, "Enumerate active playback devices");

    if (options.listDevices) {
        PrintDeviceList(devices, enumeratedDefaultId);
        goto cleanup;
    }

    if (!options.targetDeviceId.empty()) {
        selectedDeviceId = options.targetDeviceId;
    } else if (!options.legacyNameSubstring.empty()) {
        bool ambiguous = false;
        const AudioDeviceInfo* match = FindDeviceByUniqueNameSubstring(
            devices, options.legacyNameSubstring, ambiguous);
        if (ambiguous) {
            MessageBoxW(nullptr,
                L"More than one active playback device matches that name.\n\n"
                L"Run with --list-devices and use --device-id for an exact selection.",
                L"Audio Duplicator", MB_OK | MB_ICONWARNING);
            goto cleanup;
        }
        if (!match) {
            MessageBoxW(nullptr,
                L"No active playback device matches that name.",
                L"Audio Duplicator", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        selectedDeviceId = match->id;
    } else {
        if (!ShowDeviceSelectionWindow(
                GetModuleHandleW(nullptr), devices,
                enumeratedDefaultId, selectedDeviceId)) {
            goto cleanup;
        }
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &captureDevice);
    CHECK_HR(hr, "Get default playback device");

    actualDefaultId = GetImmDeviceId(captureDevice);
    if (selectedDeviceId.empty()) {
        MessageBoxW(nullptr, L"No secondary playback device was selected.",
            L"Audio Duplicator", MB_OK | MB_ICONWARNING);
        goto cleanup;
    }
    if (selectedDeviceId == actualDefaultId) {
        MessageBoxW(nullptr,
            L"The primary and secondary outputs are the same device.\n\n"
            L"Choose a different secondary output.",
            L"Audio Duplicator", MB_OK | MB_ICONWARNING);
        goto cleanup;
    }

    renderDevice = FindRenderDeviceById(enumerator, selectedDeviceId);
    if (!renderDevice) {
        MessageBoxW(nullptr,
            L"The selected secondary output is unavailable. Reconnect it and try again.",
            L"Audio Duplicator", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (const AudioDeviceInfo* selectedInfo = FindAudioDeviceInfo(devices, selectedDeviceId)) {
        selectedDeviceName = selectedInfo->name;
    } else {
        selectedDeviceName = L"secondary output";
    }

    hr = captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&captureClient));
    CHECK_HR(hr, "Activate capture IAudioClient");

    hr = captureClient->GetMixFormat(&captureFormat);
    CHECK_HR(hr, "Get capture mix format");

    captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        CHECK_HR(hr, "Create capture event");
    }

    hr = captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, captureFormat, nullptr);
    CHECK_HR(hr, "Initialize capture IAudioClient");

    hr = captureClient->SetEventHandle(captureEvent);
    CHECK_HR(hr, "Set capture event handle");

    hr = captureClient->GetService(
        __uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    CHECK_HR(hr, "Get IAudioCaptureClient");

    hr = renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&renderClient));
    CHECK_HR(hr, "Activate render IAudioClient");

    hr = renderClient->GetMixFormat(&renderFormat);
    CHECK_HR(hr, "Get render mix format");

    renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!renderEvent) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        CHECK_HR(hr, "Create render event");
    }

    hr = renderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, renderFormat, nullptr);
    CHECK_HR(hr, "Initialize render IAudioClient");

    hr = renderClient->SetEventHandle(renderEvent);
    CHECK_HR(hr, "Set render event handle");

    hr = renderClient->GetService(
        __uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    CHECK_HR(hr, "Get IAudioRenderClient");

    {
        AudioLoop loop;
        loop.captureClient = captureClient;
        loop.renderClient = renderClient;
        loop.capture = capture;
        loop.render = render;
        loop.captureEvent = captureEvent;
        loop.renderEvent = renderEvent;
        loop.captureFormat = captureFormat;
        loop.renderFormat = renderFormat;
        loop.needsResampling =
            captureFormat->nSamplesPerSec != renderFormat->nSamplesPerSec ||
            captureFormat->nChannels != renderFormat->nChannels;

        if (loop.needsResampling) loop.resampler.init(*captureFormat, *renderFormat);

        g_stop = false;
        std::thread audioThread([&loop]() { loop.run(); });
        RunTrayMessageLoop(selectedDeviceName);
        g_stop = true;
        audioThread.join();
    }

cleanup:
    if (captureFormat) CoTaskMemFree(captureFormat);
    if (renderFormat) CoTaskMemFree(renderFormat);
    if (captureEvent) CloseHandle(captureEvent);
    if (renderEvent) CloseHandle(renderEvent);
    SAFE_RELEASE(capture);
    SAFE_RELEASE(render);
    SAFE_RELEASE(captureClient);
    SAFE_RELEASE(renderClient);
    SAFE_RELEASE(captureDevice);
    SAFE_RELEASE(renderDevice);
    SAFE_RELEASE(enumerator);
    if (shouldUninitializeCom) CoUninitialize();
    return 0;
}
