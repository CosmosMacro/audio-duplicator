// audio_duplicator_compat.cpp
// Bluetooth-compatible Audio Duplicator implementation for Windows 10/11.
// Captures the Windows default output through WASAPI loopback and renders to
// a selected secondary endpoint without installing a driver.

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

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "device_selector.h"

using Microsoft::WRL::ComPtr;

namespace {

struct ScopedCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ScopedCom() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

struct ScopedHandle {
    HANDLE value = nullptr;
    ~ScopedHandle() {
        if (value) CloseHandle(value);
    }
};

struct ScopedWaveFormat {
    WAVEFORMATEX* value = nullptr;
    ~ScopedWaveFormat() {
        if (value) CoTaskMemFree(value);
    }
};

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
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            options.showHelp = true;
        } else if (argument == L"--device-id") {
            if (index + 1 >= argc) {
                errorMessage = L"--device-id requires a Windows audio device ID.";
                return false;
            }
            options.targetDeviceId = Utf8ToWide(argv[++index]);
        } else if (argument.rfind(L"--", 0) == 0) {
            errorMessage = L"Unknown option: " + argument;
            return false;
        } else if (!options.legacyNameSubstring.empty()) {
            errorMessage = L"Only one device-name substring can be supplied.";
            return false;
        } else {
            options.legacyNameSubstring = argument;
        }
    }

    if (!options.targetDeviceId.empty() && !options.legacyNameSubstring.empty()) {
        errorMessage = L"Use either --device-id or a device-name substring, not both.";
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
        L"Options:\n"
        L"  --list-devices          List active playback devices and IDs\n"
        L"  --device-id <ID>        Start with an exact Windows device ID\n"
        L"  <name substring>        Select one uniquely matching device\n"
        L"  --help                  Show this help",
        L"Audio Duplicator",
        MB_OK | MB_ICONINFORMATION);
}

std::wstring Lowercase(std::wstring text) {
    for (wchar_t& character : text) {
        character = static_cast<wchar_t>(towlower(character));
    }
    return text;
}

const AudioDeviceInfo* FindUniqueNameMatch(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& substring,
    bool& ambiguous) {
    ambiguous = false;
    const std::wstring lowered = Lowercase(substring);
    const AudioDeviceInfo* match = nullptr;

    for (const auto& device : devices) {
        if (Lowercase(device.name).find(lowered) == std::wstring::npos) continue;
        if (match) {
            ambiguous = true;
            return nullptr;
        }
        match = &device;
    }
    return match;
}

std::wstring GetDeviceId(IMMDevice* device) {
    if (!device) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

float ReadSample(const BYTE* buffer, UINT32 frame, UINT32 channel, const WAVEFORMATEX* format) {
    if (!buffer || !format || channel >= format->nChannels) return 0.0f;

    const UINT32 bytesPerSample = format->wBitsPerSample / 8;
    const BYTE* sample = buffer +
        static_cast<size_t>(frame) * format->nBlockAlign +
        static_cast<size_t>(channel) * bytesPerSample;

    if (IsFloatFormat(format) && format->wBitsPerSample == 32) {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return value;
    }

    if (format->wBitsPerSample == 16) {
        INT16 value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }

    if (format->wBitsPerSample == 24) {
        INT32 value = static_cast<INT32>(sample[0]) |
            (static_cast<INT32>(sample[1]) << 8) |
            (static_cast<INT32>(sample[2]) << 16);
        if ((value & 0x00800000) != 0) value |= static_cast<INT32>(0xFF000000);
        return static_cast<float>(value) / 8388608.0f;
    }

    if (format->wBitsPerSample == 32) {
        INT32 value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(value) / 2147483648.0f;
    }

    return 0.0f;
}

INT16 FloatToPcm16(float value) {
    value = (std::max)(-1.0f, (std::min)(1.0f, value));
    return static_cast<INT16>(std::lrint(value * 32767.0f));
}

struct RenderSetup {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX streamFormat = {};
    bool convertToPcm16 = false;
    HRESULT nativeAttempt = E_FAIL;
    HRESULT pcmAttempt = E_FAIL;
};

HRESULT ActivateAudioClient(IMMDevice* device, ComPtr<IAudioClient>& client) {
    client.Reset();
    if (!device) return E_POINTER;
    return device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
}

HRESULT InitializeRenderClient(
    IMMDevice* renderDevice,
    const WAVEFORMATEX* captureFormat,
    RenderSetup& setup) {
    constexpr DWORD conversionFlags =
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = ActivateAudioClient(renderDevice, setup.client);
    if (FAILED(hr)) return hr;

    setup.nativeAttempt = setup.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        conversionFlags,
        0,
        0,
        captureFormat,
        nullptr);

    if (SUCCEEDED(setup.nativeAttempt)) {
        setup.streamFormat = *captureFormat;
        setup.convertToPcm16 = false;
        return setup.client->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(setup.render.GetAddressOf()));
    }

    // Microsoft documents that a failed Initialize call can leave the same
    // IAudioClient instance unusable, so activate a fresh instance for retry.
    setup.client.Reset();
    setup.render.Reset();

    WAVEFORMATEX pcm16 = {};
    pcm16.wFormatTag = WAVE_FORMAT_PCM;
    pcm16.nChannels = 2;
    pcm16.nSamplesPerSec = captureFormat->nSamplesPerSec;
    pcm16.wBitsPerSample = 16;
    pcm16.nBlockAlign = static_cast<WORD>(pcm16.nChannels * pcm16.wBitsPerSample / 8);
    pcm16.nAvgBytesPerSec = pcm16.nSamplesPerSec * pcm16.nBlockAlign;
    pcm16.cbSize = 0;

    hr = ActivateAudioClient(renderDevice, setup.client);
    if (FAILED(hr)) return hr;

    setup.pcmAttempt = setup.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        conversionFlags,
        0,
        0,
        &pcm16,
        nullptr);
    if (FAILED(setup.pcmAttempt)) return setup.pcmAttempt;

    setup.streamFormat = pcm16;
    setup.convertToPcm16 = true;
    return setup.client->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(setup.render.GetAddressOf()));
}

void ShowRenderInitializationError(
    const std::wstring& deviceName,
    const WAVEFORMATEX* captureFormat,
    const RenderSetup& setup,
    HRESULT finalResult) {
    wchar_t message[768] = {};
    swprintf_s(
        message,
        L"Windows could not initialize the selected Bluetooth output.\n\n"
        L"Device: %s\n"
        L"Captured stream: %lu Hz, %u channel(s), %u-bit\n\n"
        L"Native-format attempt: 0x%08X\n"
        L"PCM 16-bit compatibility attempt: 0x%08X\n"
        L"Final result: 0x%08X\n\n"
        L"Confirm that the device is connected as a stereo playback output and that "
        L"its microphone is not currently in use.",
        deviceName.c_str(),
        captureFormat ? captureFormat->nSamplesPerSec : 0,
        captureFormat ? captureFormat->nChannels : 0,
        captureFormat ? captureFormat->wBitsPerSample : 0,
        static_cast<unsigned>(setup.nativeAttempt),
        static_cast<unsigned>(setup.pcmAttempt),
        static_cast<unsigned>(finalResult));

    MessageBoxW(nullptr, message, L"Audio Duplicator", MB_OK | MB_ICONERROR);
}

static std::atomic<bool> g_stop(false);

struct AudioLoop {
    IAudioClient* captureClient = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE captureEvent = nullptr;
    const WAVEFORMATEX* captureFormat = nullptr;
    WAVEFORMATEX renderFormat = {};
    bool convertToPcm16 = false;

    std::vector<BYTE> byteScratch;
    std::vector<INT16> pcmScratch;

    void Run() {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        if (FAILED(renderClient->Start()) || FAILED(captureClient->Start())) {
            g_stop = true;
            if (task) AvRevertMmThreadCharacteristics(task);
            return;
        }

        while (!g_stop) {
            const DWORD waitResult = WaitForSingleObject(captureEvent, 50);
            if (waitResult == WAIT_TIMEOUT) continue;
            if (waitResult != WAIT_OBJECT_0) break;

            UINT32 packetSize = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
                BYTE* input = nullptr;
                UINT32 capturedFrames = 0;
                DWORD captureFlags = 0;
                HRESULT hr = capture->GetBuffer(
                    &input, &capturedFrames, &captureFlags, nullptr, nullptr);
                if (FAILED(hr)) break;

                UINT32 renderBufferSize = 0;
                UINT32 renderPadding = 0;
                hr = renderClient->GetBufferSize(&renderBufferSize);
                if (SUCCEEDED(hr)) hr = renderClient->GetCurrentPadding(&renderPadding);
                if (FAILED(hr)) {
                    capture->ReleaseBuffer(capturedFrames);
                    break;
                }

                const UINT32 availableFrames = renderBufferSize - renderPadding;
                const UINT32 framesToWrite = (std::min)(capturedFrames, availableFrames);
                const bool silent = (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                if (framesToWrite > 0 && !silent) {
                    if (convertToPcm16) {
                        pcmScratch.resize(static_cast<size_t>(framesToWrite) * 2);
                        for (UINT32 frame = 0; frame < framesToWrite; ++frame) {
                            const float left = ReadSample(input, frame, 0, captureFormat);
                            const float right = captureFormat->nChannels >= 2
                                ? ReadSample(input, frame, 1, captureFormat)
                                : left;
                            pcmScratch[static_cast<size_t>(frame) * 2] = FloatToPcm16(left);
                            pcmScratch[static_cast<size_t>(frame) * 2 + 1] = FloatToPcm16(right);
                        }
                    } else {
                        const size_t bytes =
                            static_cast<size_t>(framesToWrite) * captureFormat->nBlockAlign;
                        byteScratch.resize(bytes);
                        std::memcpy(byteScratch.data(), input, bytes);
                    }
                }

                capture->ReleaseBuffer(capturedFrames);
                if (framesToWrite == 0) continue;

                BYTE* output = nullptr;
                hr = render->GetBuffer(framesToWrite, &output);
                if (FAILED(hr)) break;

                if (!silent) {
                    if (convertToPcm16) {
                        std::memcpy(
                            output,
                            pcmScratch.data(),
                            static_cast<size_t>(framesToWrite) * renderFormat.nBlockAlign);
                    } else {
                        std::memcpy(
                            output,
                            byteScratch.data(),
                            static_cast<size_t>(framesToWrite) * renderFormat.nBlockAlign);
                    }
                }

                render->ReleaseBuffer(
                    framesToWrite,
                    silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
            }
        }

        captureClient->Stop();
        renderClient->Stop();
        if (task) AvRevertMmThreadCharacteristics(task);
    }
};

constexpr UINT kTrayMessage = WM_USER + 1;
constexpr UINT kTrayStopCommand = 1001;
NOTIFYICONDATAW g_trayIcon = {};

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM, LPARAM lParam) {
    if (message == kTrayMessage &&
        (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)) {
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
        return 0;
    }

    if (message == WM_CLOSE || message == WM_DESTROY) {
        g_stop = true;
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, 0, lParam);
}

void RunTrayMessageLoop(const std::wstring& secondaryName) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"AudioDuplicatorTrayCompat";
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"Audio Duplicator",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        windowClass.hInstance,
        nullptr);

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = window;
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
        MessageBoxW(nullptr, argumentError.c_str(), L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 2;
    }
    if (options.showHelp) {
        ShowHelp();
        return 0;
    }

    ScopedCom com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to access Windows audio devices.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::vector<AudioDeviceInfo> devices;
    std::wstring enumeratedDefaultId;
    hr = EnumerateActiveRenderDevices(enumerator.Get(), devices, enumeratedDefaultId);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to enumerate active playback devices.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (options.listDevices) {
        PrintDeviceList(devices, enumeratedDefaultId);
        return 0;
    }

    std::wstring selectedDeviceId;
    if (!options.targetDeviceId.empty()) {
        selectedDeviceId = options.targetDeviceId;
    } else if (!options.legacyNameSubstring.empty()) {
        bool ambiguous = false;
        const AudioDeviceInfo* match = FindUniqueNameMatch(
            devices, options.legacyNameSubstring, ambiguous);
        if (ambiguous || !match) {
            MessageBoxW(
                nullptr,
                ambiguous
                    ? L"Several devices match that name. Use the graphical selector or --device-id."
                    : L"No active playback device matches that name.",
                L"Audio Duplicator",
                MB_OK | MB_ICONWARNING);
            return 1;
        }
        selectedDeviceId = match->id;
    } else if (!ShowDeviceSelectionWindow(
            GetModuleHandleW(nullptr), devices, enumeratedDefaultId, selectedDeviceId)) {
        return 0;
    }

    ComPtr<IMMDevice> captureDevice;
    hr = enumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, captureDevice.GetAddressOf());
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"No Windows default playback device is available.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (selectedDeviceId.empty() || selectedDeviceId == GetDeviceId(captureDevice.Get())) {
        MessageBoxW(nullptr, L"Choose a secondary output different from the Windows default output.", L"Audio Duplicator", MB_OK | MB_ICONWARNING);
        return 1;
    }

    ComPtr<IMMDevice> renderDevice;
    renderDevice.Attach(FindRenderDeviceById(enumerator.Get(), selectedDeviceId));
    if (!renderDevice) {
        MessageBoxW(nullptr, L"The selected secondary output is unavailable.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring selectedDeviceName = L"secondary output";
    if (const AudioDeviceInfo* info = FindAudioDeviceInfo(devices, selectedDeviceId)) {
        selectedDeviceName = info->name;
    }

    ComPtr<IAudioClient> captureClient;
    hr = captureDevice->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(captureClient.GetAddressOf()));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to open the Windows default output for loopback capture.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    ScopedWaveFormat captureFormat;
    hr = captureClient->GetMixFormat(&captureFormat.value);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to read the primary output format.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    ScopedHandle captureEvent;
    captureEvent.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent.value) {
        MessageBoxW(nullptr, L"Unable to create the audio capture event.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    hr = captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0,
        0,
        captureFormat.value,
        nullptr);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to initialize loopback capture.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    hr = captureClient->SetEventHandle(captureEvent.value);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to configure loopback capture notifications.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = captureClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(capture.GetAddressOf()));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Unable to access captured audio frames.", L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return 1;
    }

    RenderSetup renderSetup;
    hr = InitializeRenderClient(renderDevice.Get(), captureFormat.value, renderSetup);
    if (FAILED(hr)) {
        ShowRenderInitializationError(
            selectedDeviceName, captureFormat.value, renderSetup, hr);
        return 1;
    }

    AudioLoop loop;
    loop.captureClient = captureClient.Get();
    loop.renderClient = renderSetup.client.Get();
    loop.capture = capture.Get();
    loop.render = renderSetup.render.Get();
    loop.captureEvent = captureEvent.value;
    loop.captureFormat = captureFormat.value;
    loop.renderFormat = renderSetup.streamFormat;
    loop.convertToPcm16 = renderSetup.convertToPcm16;

    g_stop = false;
    std::thread audioThread([&loop]() { loop.Run(); });
    RunTrayMessageLoop(selectedDeviceName);
    g_stop = true;
    audioThread.join();
    return 0;
}
