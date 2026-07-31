// audio_duplicator_polling.cpp
// Polling-based WASAPI implementation intended for Bluetooth playback devices.

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

constexpr REFERENCE_TIME kPollingBufferDuration = 1000000; // 100 ms
constexpr DWORD kPollingSleepMilliseconds = 5;

struct ScopedCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ~ScopedCom() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct CommandLineOptions {
    bool listDevices = false;
    bool showHelp = false;
    std::wstring targetDeviceId;
    std::wstring legacyNameSubstring;
};

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length) <= 0) {
        return {};
    }

    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
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
        errorMessage = L"Use either --device-id or a name substring, not both.";
        return false;
    }

    return true;
}

void ShowHelp() {
    MessageBoxW(
        nullptr,
        L"Audio Duplicator\n\n"
        L"Set the first headphones as the Windows default output, then select the second output.\n\n"
        L"Options:\n"
        L"  --list-devices          List active playback devices and IDs\n"
        L"  --device-id <ID>        Use an exact Windows device ID\n"
        L"  <name substring>        Use one uniquely matching device\n"
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
    const std::wstring loweredSubstring = Lowercase(substring);
    const AudioDeviceInfo* match = nullptr;

    for (const auto& device : devices) {
        if (Lowercase(device.name).find(loweredSubstring) == std::wstring::npos) {
            continue;
        }

        if (match) {
            ambiguous = true;
            return nullptr;
        }
        match = &device;
    }

    return match;
}

std::wstring GetDeviceId(IMMDevice* device) {
    if (!device) {
        return {};
    }

    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) {
        return {};
    }

    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }

    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

float ReadSample(
    const BYTE* buffer,
    UINT32 frame,
    UINT32 channel,
    const WAVEFORMATEX* format) {
    if (!buffer || !format || channel >= format->nChannels) {
        return 0.0f;
    }

    const UINT32 bytesPerSample = format->wBitsPerSample / 8;
    if (bytesPerSample == 0) {
        return 0.0f;
    }

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
        if ((value & 0x00800000) != 0) {
            value |= static_cast<INT32>(0xFF000000);
        }
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

struct CaptureSetup {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* format = nullptr;
    HRESULT defaultDurationAttempt = E_FAIL;
    HRESULT fixedDurationAttempt = E_FAIL;

    ~CaptureSetup() {
        if (format) {
            CoTaskMemFree(format);
        }
    }
};

HRESULT ActivateAudioClient(IMMDevice* device, ComPtr<IAudioClient>& client) {
    client.Reset();
    if (!device) {
        return E_POINTER;
    }

    return device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
}

HRESULT InitializeCaptureClient(IMMDevice* captureDevice, CaptureSetup& setup) {
    HRESULT hr = ActivateAudioClient(captureDevice, setup.client);
    if (FAILED(hr)) {
        return hr;
    }

    hr = setup.client->GetMixFormat(&setup.format);
    if (FAILED(hr)) {
        return hr;
    }

    setup.defaultDurationAttempt = setup.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        0,
        0,
        setup.format,
        nullptr);

    if (FAILED(setup.defaultDurationAttempt)) {
        setup.client.Reset();
        hr = ActivateAudioClient(captureDevice, setup.client);
        if (FAILED(hr)) {
            return hr;
        }

        setup.fixedDurationAttempt = setup.client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            kPollingBufferDuration,
            0,
            setup.format,
            nullptr);
        if (FAILED(setup.fixedDurationAttempt)) {
            return setup.fixedDurationAttempt;
        }
    }

    return setup.client->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(setup.capture.GetAddressOf()));
}

void ShowCaptureInitializationError(
    const CaptureSetup& setup,
    HRESULT finalResult) {
    wchar_t message[768] = {};
    swprintf_s(
        message,
        L"Windows could not initialize loopback capture.\n\n"
        L"Primary format: %lu Hz, %u channel(s), %u-bit\n\n"
        L"Polling attempt with Windows default buffer: 0x%08X\n"
        L"Polling attempt with 100 ms buffer: 0x%08X\n"
        L"Final result: 0x%08X\n\n"
        L"Confirm that the primary AirPods are still connected and selected as the Windows default stereo output.",
        setup.format ? setup.format->nSamplesPerSec : 0,
        setup.format ? setup.format->nChannels : 0,
        setup.format ? setup.format->wBitsPerSample : 0,
        static_cast<unsigned>(setup.defaultDurationAttempt),
        static_cast<unsigned>(setup.fixedDurationAttempt),
        static_cast<unsigned>(finalResult));

    MessageBoxW(nullptr, message, L"Audio Duplicator", MB_OK | MB_ICONERROR);
}

enum class RenderMode {
    DirectCopy,
    Pcm16
};

struct RenderSetup {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX streamFormat = {};
    RenderMode mode = RenderMode::DirectCopy;
    HRESULT sourceFormatAttempt = E_FAIL;
    HRESULT pcmCaptureRateAttempt = E_FAIL;
    HRESULT pcm48kAttempt = E_FAIL;
};

WAVEFORMATEX MakePcm16Format(DWORD sampleRate) {
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    return format;
}

HRESULT TryInitializeRender(
    IMMDevice* renderDevice,
    const WAVEFORMATEX* format,
    ComPtr<IAudioClient>& client,
    ComPtr<IAudioRenderClient>& render) {
    HRESULT hr = ActivateAudioClient(renderDevice, client);
    if (FAILED(hr)) {
        return hr;
    }

    constexpr DWORD conversionFlags =
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        conversionFlags,
        kPollingBufferDuration,
        0,
        format,
        nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    return client->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(render.GetAddressOf()));
}

HRESULT InitializeRenderClient(
    IMMDevice* renderDevice,
    const WAVEFORMATEX* captureFormat,
    RenderSetup& setup) {
    setup.sourceFormatAttempt = TryInitializeRender(
        renderDevice,
        captureFormat,
        setup.client,
        setup.render);
    if (SUCCEEDED(setup.sourceFormatAttempt)) {
        setup.streamFormat = *captureFormat;
        setup.mode = RenderMode::DirectCopy;
        return S_OK;
    }

    setup.client.Reset();
    setup.render.Reset();
    WAVEFORMATEX pcmCaptureRate = MakePcm16Format(captureFormat->nSamplesPerSec);
    setup.pcmCaptureRateAttempt = TryInitializeRender(
        renderDevice,
        &pcmCaptureRate,
        setup.client,
        setup.render);
    if (SUCCEEDED(setup.pcmCaptureRateAttempt)) {
        setup.streamFormat = pcmCaptureRate;
        setup.mode = RenderMode::Pcm16;
        return S_OK;
    }

    setup.client.Reset();
    setup.render.Reset();
    WAVEFORMATEX pcm48k = MakePcm16Format(48000);
    setup.pcm48kAttempt = TryInitializeRender(
        renderDevice,
        &pcm48k,
        setup.client,
        setup.render);
    if (SUCCEEDED(setup.pcm48kAttempt)) {
        setup.streamFormat = pcm48k;
        setup.mode = RenderMode::Pcm16;
        return S_OK;
    }

    return setup.pcm48kAttempt;
}

void ShowRenderInitializationError(
    const std::wstring& deviceName,
    const WAVEFORMATEX* captureFormat,
    const RenderSetup& setup,
    HRESULT finalResult) {
    wchar_t message[896] = {};
    swprintf_s(
        message,
        L"Windows could not initialize the selected secondary output.\n\n"
        L"Device: %s\n"
        L"Primary stream: %lu Hz, %u channel(s), %u-bit\n\n"
        L"Source-format attempt: 0x%08X\n"
        L"PCM 16-bit at primary rate: 0x%08X\n"
        L"PCM 16-bit at 48 kHz: 0x%08X\n"
        L"Final result: 0x%08X\n\n"
        L"Confirm that the secondary AirPods are connected as a stereo output and that no application is using their microphone.",
        deviceName.c_str(),
        captureFormat ? captureFormat->nSamplesPerSec : 0,
        captureFormat ? captureFormat->nChannels : 0,
        captureFormat ? captureFormat->wBitsPerSample : 0,
        static_cast<unsigned>(setup.sourceFormatAttempt),
        static_cast<unsigned>(setup.pcmCaptureRateAttempt),
        static_cast<unsigned>(setup.pcm48kAttempt),
        static_cast<unsigned>(finalResult));

    MessageBoxW(nullptr, message, L"Audio Duplicator", MB_OK | MB_ICONERROR);
}

UINT32 CalculateConvertedFrameCount(
    UINT32 sourceFrames,
    DWORD sourceRate,
    DWORD destinationRate) {
    if (sourceFrames == 0 || sourceRate == 0 || destinationRate == 0) {
        return 0;
    }

    const double converted =
        static_cast<double>(sourceFrames) * destinationRate / sourceRate;
    return static_cast<UINT32>((std::max)(1.0, std::floor(converted + 0.5)));
}

UINT32 ConvertToStereoPcm16(
    const BYTE* source,
    UINT32 sourceFrames,
    const WAVEFORMATEX* sourceFormat,
    DWORD destinationRate,
    UINT32 maximumOutputFrames,
    std::vector<INT16>& destination) {
    if (!source || !sourceFormat || maximumOutputFrames == 0) {
        return 0;
    }

    UINT32 outputFrames = CalculateConvertedFrameCount(
        sourceFrames,
        sourceFormat->nSamplesPerSec,
        destinationRate);
    outputFrames = (std::min)(outputFrames, maximumOutputFrames);
    destination.resize(static_cast<size_t>(outputFrames) * 2);

    const double sourceStep =
        static_cast<double>(sourceFormat->nSamplesPerSec) / destinationRate;

    for (UINT32 outputFrame = 0; outputFrame < outputFrames; ++outputFrame) {
        const double sourcePosition = outputFrame * sourceStep;
        UINT32 firstFrame = static_cast<UINT32>(sourcePosition);
        if (firstFrame >= sourceFrames) {
            firstFrame = sourceFrames - 1;
        }
        const UINT32 secondFrame = (std::min)(firstFrame + 1, sourceFrames - 1);
        const float fraction = static_cast<float>(sourcePosition - firstFrame);

        const float firstLeft = ReadSample(source, firstFrame, 0, sourceFormat);
        const float secondLeft = ReadSample(source, secondFrame, 0, sourceFormat);
        const float left = firstLeft + fraction * (secondLeft - firstLeft);

        float right = left;
        if (sourceFormat->nChannels >= 2) {
            const float firstRight = ReadSample(source, firstFrame, 1, sourceFormat);
            const float secondRight = ReadSample(source, secondFrame, 1, sourceFormat);
            right = firstRight + fraction * (secondRight - firstRight);
        }

        destination[static_cast<size_t>(outputFrame) * 2] = FloatToPcm16(left);
        destination[static_cast<size_t>(outputFrame) * 2 + 1] = FloatToPcm16(right);
    }

    return outputFrames;
}

static std::atomic<bool> g_stop(false);

struct AudioLoop {
    IAudioClient* captureClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    const WAVEFORMATEX* captureFormat = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX renderFormat = {};
    RenderMode renderMode = RenderMode::DirectCopy;

    std::vector<BYTE> directCopyBuffer;
    std::vector<INT16> pcm16Buffer;

    void Run() {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        const HRESULT renderStart = renderClient->Start();
        const HRESULT captureStart = captureClient->Start();
        if (FAILED(renderStart) || FAILED(captureStart)) {
            g_stop = true;
            if (task) {
                AvRevertMmThreadCharacteristics(task);
            }
            return;
        }

        while (!g_stop) {
            bool receivedPacket = false;
            UINT32 packetSize = 0;

            while (SUCCEEDED(capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
                receivedPacket = true;

                BYTE* input = nullptr;
                UINT32 capturedFrames = 0;
                DWORD captureFlags = 0;
                HRESULT hr = capture->GetBuffer(
                    &input,
                    &capturedFrames,
                    &captureFlags,
                    nullptr,
                    nullptr);
                if (FAILED(hr)) {
                    break;
                }

                UINT32 renderBufferSize = 0;
                UINT32 renderPadding = 0;
                hr = renderClient->GetBufferSize(&renderBufferSize);
                if (SUCCEEDED(hr)) {
                    hr = renderClient->GetCurrentPadding(&renderPadding);
                }

                if (FAILED(hr)) {
                    capture->ReleaseBuffer(capturedFrames);
                    break;
                }

                const UINT32 availableFrames = renderBufferSize - renderPadding;
                if (availableFrames == 0) {
                    capture->ReleaseBuffer(capturedFrames);
                    continue;
                }

                const bool silent =
                    (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                UINT32 framesToWrite = 0;

                if (renderMode == RenderMode::DirectCopy) {
                    framesToWrite = (std::min)(capturedFrames, availableFrames);
                    if (!silent && framesToWrite > 0) {
                        const size_t bytes =
                            static_cast<size_t>(framesToWrite) * captureFormat->nBlockAlign;
                        directCopyBuffer.resize(bytes);
                        std::memcpy(directCopyBuffer.data(), input, bytes);
                    }
                } else {
                    if (silent) {
                        framesToWrite = CalculateConvertedFrameCount(
                            capturedFrames,
                            captureFormat->nSamplesPerSec,
                            renderFormat.nSamplesPerSec);
                        framesToWrite = (std::min)(framesToWrite, availableFrames);
                    } else {
                        framesToWrite = ConvertToStereoPcm16(
                            input,
                            capturedFrames,
                            captureFormat,
                            renderFormat.nSamplesPerSec,
                            availableFrames,
                            pcm16Buffer);
                    }
                }

                capture->ReleaseBuffer(capturedFrames);
                if (framesToWrite == 0) {
                    continue;
                }

                BYTE* output = nullptr;
                hr = render->GetBuffer(framesToWrite, &output);
                if (FAILED(hr)) {
                    break;
                }

                if (!silent) {
                    if (renderMode == RenderMode::DirectCopy) {
                        std::memcpy(
                            output,
                            directCopyBuffer.data(),
                            static_cast<size_t>(framesToWrite) * renderFormat.nBlockAlign);
                    } else {
                        std::memcpy(
                            output,
                            pcm16Buffer.data(),
                            static_cast<size_t>(framesToWrite) * renderFormat.nBlockAlign);
                    }
                }

                render->ReleaseBuffer(
                    framesToWrite,
                    silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
            }

            if (!receivedPacket) {
                Sleep(kPollingSleepMilliseconds);
            }
        }

        captureClient->Stop();
        renderClient->Stop();
        if (task) {
            AvRevertMmThreadCharacteristics(task);
        }
    }
};

constexpr UINT kTrayMessage = WM_USER + 1;
constexpr UINT kTrayStopCommand = 1001;
NOTIFYICONDATAW g_trayIcon = {};

LRESULT CALLBACK TrayWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == kTrayMessage &&
        (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kTrayStopCommand, L"Stop Audio Duplicator");

        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        const int command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x,
            cursor.y,
            0,
            window,
            nullptr);
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

    return DefWindowProcW(window, message, wParam, lParam);
}

void RunTrayMessageLoop(const std::wstring& secondaryName) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"AudioDuplicatorPollingTray";
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"Audio Duplicator",
        0,
        0,
        0,
        0,
        0,
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
        MessageBoxW(
            nullptr,
            argumentError.c_str(),
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 2;
    }

    if (options.showHelp) {
        ShowHelp();
        return 0;
    }

    ScopedCom com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) {
        MessageBoxW(
            nullptr,
            L"Failed to initialize COM.",
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
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
        MessageBoxW(
            nullptr,
            L"Unable to access Windows audio devices.",
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    std::vector<AudioDeviceInfo> devices;
    std::wstring defaultDeviceId;
    hr = EnumerateActiveRenderDevices(enumerator.Get(), devices, defaultDeviceId);
    if (FAILED(hr)) {
        MessageBoxW(
            nullptr,
            L"Unable to enumerate active playback devices.",
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    if (options.listDevices) {
        PrintDeviceList(devices, defaultDeviceId);
        return 0;
    }

    std::wstring selectedDeviceId;
    if (!options.targetDeviceId.empty()) {
        selectedDeviceId = options.targetDeviceId;
    } else if (!options.legacyNameSubstring.empty()) {
        bool ambiguous = false;
        const AudioDeviceInfo* match = FindUniqueNameMatch(
            devices,
            options.legacyNameSubstring,
            ambiguous);
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
            GetModuleHandleW(nullptr),
            devices,
            defaultDeviceId,
            selectedDeviceId)) {
        return 0;
    }

    ComPtr<IMMDevice> captureDevice;
    hr = enumerator->GetDefaultAudioEndpoint(
        eRender,
        eConsole,
        captureDevice.GetAddressOf());
    if (FAILED(hr)) {
        MessageBoxW(
            nullptr,
            L"No Windows default playback device is available.",
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    if (selectedDeviceId.empty() || selectedDeviceId == GetDeviceId(captureDevice.Get())) {
        MessageBoxW(
            nullptr,
            L"Choose a secondary output different from the Windows default output.",
            L"Audio Duplicator",
            MB_OK | MB_ICONWARNING);
        return 1;
    }

    ComPtr<IMMDevice> renderDevice;
    renderDevice.Attach(FindRenderDeviceById(enumerator.Get(), selectedDeviceId));
    if (!renderDevice) {
        MessageBoxW(
            nullptr,
            L"The selected secondary output is unavailable.",
            L"Audio Duplicator",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring selectedDeviceName = L"secondary output";
    if (const AudioDeviceInfo* info = FindAudioDeviceInfo(devices, selectedDeviceId)) {
        selectedDeviceName = info->name;
    }

    CaptureSetup captureSetup;
    hr = InitializeCaptureClient(captureDevice.Get(), captureSetup);
    if (FAILED(hr)) {
        ShowCaptureInitializationError(captureSetup, hr);
        return 1;
    }

    RenderSetup renderSetup;
    hr = InitializeRenderClient(
        renderDevice.Get(),
        captureSetup.format,
        renderSetup);
    if (FAILED(hr)) {
        ShowRenderInitializationError(
            selectedDeviceName,
            captureSetup.format,
            renderSetup,
            hr);
        return 1;
    }

    AudioLoop loop;
    loop.captureClient = captureSetup.client.Get();
    loop.capture = captureSetup.capture.Get();
    loop.captureFormat = captureSetup.format;
    loop.renderClient = renderSetup.client.Get();
    loop.render = renderSetup.render.Get();
    loop.renderFormat = renderSetup.streamFormat;
    loop.renderMode = renderSetup.mode;

    g_stop = false;
    std::thread audioThread([&loop]() { loop.Run(); });
    RunTrayMessageLoop(selectedDeviceName);
    g_stop = true;
    audioThread.join();
    return 0;
}
