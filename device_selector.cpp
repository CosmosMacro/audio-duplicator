#include "device_selector.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <cstdio>
#include <cwchar>
#include <utility>

namespace {

constexpr wchar_t kWindowClassName[] = L"AudioDuplicatorDeviceSelector";
constexpr int kWindowWidth = 560;
constexpr int kWindowHeight = 250;
constexpr int kControlMargin = 20;
constexpr int kLabelHeight = 20;
constexpr int kControlHeight = 28;
constexpr int kStartButtonId = 1001;
constexpr int kCancelButtonId = 1002;
constexpr int kSecondaryComboId = 1003;

struct SelectionContext {
    const std::vector<AudioDeviceInfo>* devices = nullptr;
    std::wstring defaultDeviceId;
    std::wstring selectedDeviceId;
    bool accepted = false;
    HWND secondaryCombo = nullptr;
};

std::wstring GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return {};

    IPropertyStore* properties = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(hr)) return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;

    hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }

    PropVariantClear(&value);
    properties->Release();
    return result;
}

std::wstring GetDeviceId(IMMDevice* device) {
    if (!device) return {};

    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};

    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

void ApplyDefaultFont(HWND control) {
    if (!control) return;
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width) {
    HWND control = CreateWindowExW(
        0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE,
        x, y, width, kLabelHeight,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyDefaultFont(control);
    return control;
}

HWND CreateReadOnlyText(HWND parent, const std::wstring& text, int x, int y, int width) {
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        x, y, width, kControlHeight,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyDefaultFont(control);
    return control;
}

HWND CreateButton(HWND parent, const wchar_t* text, int id, int x, int y, int width, bool defaultButton) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    if (defaultButton) style |= BS_DEFPUSHBUTTON;

    HWND control = CreateWindowExW(
        0, L"BUTTON", text, style,
        x, y, width, kControlHeight,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    ApplyDefaultFont(control);
    return control;
}

void CenterWindow(HWND window) {
    RECT windowRect = {};
    if (!GetWindowRect(window, &windowRect)) return;

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK SelectionWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SelectionContext* context = reinterpret_cast<SelectionContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<SelectionContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }

    switch (message) {
    case WM_CREATE: {
        if (!context || !context->devices) return -1;

        const AudioDeviceInfo* primary = FindAudioDeviceInfo(
            *context->devices, context->defaultDeviceId);
        const std::wstring primaryName = primary ? primary->name : L"Unknown default output";

        CreateLabel(window, L"Primary output (Windows default)",
            kControlMargin, 18, kWindowWidth - 2 * kControlMargin);
        CreateReadOnlyText(window, primaryName,
            kControlMargin, 42, kWindowWidth - 2 * kControlMargin - 16);

        CreateLabel(window, L"Secondary output to receive the duplicate",
            kControlMargin, 86, kWindowWidth - 2 * kControlMargin);

        context->secondaryCombo = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            kControlMargin, 110, kWindowWidth - 2 * kControlMargin - 16, 220,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSecondaryComboId)),
            GetModuleHandleW(nullptr), nullptr);
        ApplyDefaultFont(context->secondaryCombo);

        int firstItem = -1;
        for (size_t index = 0; index < context->devices->size(); ++index) {
            const auto& device = context->devices->at(index);
            if (device.id == context->defaultDeviceId) continue;

            const LRESULT item = SendMessageW(
                context->secondaryCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(device.name.c_str()));
            if (item == CB_ERR || item == CB_ERRSPACE) continue;

            SendMessageW(
                context->secondaryCombo, CB_SETITEMDATA,
                static_cast<WPARAM>(item), static_cast<LPARAM>(index));
            if (firstItem < 0) firstItem = static_cast<int>(item);
        }

        if (firstItem >= 0) {
            SendMessageW(context->secondaryCombo, CB_SETCURSEL, firstItem, 0);
        }

        const int buttonWidth = 110;
        const int buttonY = 164;
        HWND startButton = CreateButton(
            window, L"Start", kStartButtonId,
            kWindowWidth - kControlMargin - 16 - buttonWidth * 2 - 12,
            buttonY, buttonWidth, true);
        CreateButton(
            window, L"Cancel", kCancelButtonId,
            kWindowWidth - kControlMargin - 16 - buttonWidth,
            buttonY, buttonWidth, false);

        if (firstItem < 0) {
            EnableWindow(startButton, FALSE);
            MessageBoxW(
                window,
                L"Only one active playback device is available. Connect the second pair of headphones and reopen Audio Duplicator.",
                L"Audio Duplicator",
                MB_OK | MB_ICONINFORMATION);
        }

        SetFocus(context->secondaryCombo);
        return 0;
    }

    case WM_COMMAND: {
        if (!context) break;
        const int command = LOWORD(wParam);

        if (command == kStartButtonId) {
            const LRESULT selected = SendMessageW(
                context->secondaryCombo, CB_GETCURSEL, 0, 0);
            if (selected == CB_ERR) {
                MessageBoxW(window, L"Select a secondary output.",
                    L"Audio Duplicator", MB_OK | MB_ICONWARNING);
                return 0;
            }

            const LRESULT deviceIndex = SendMessageW(
                context->secondaryCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(selected), 0);
            if (deviceIndex == CB_ERR ||
                deviceIndex < 0 ||
                static_cast<unsigned long long>(deviceIndex) >= context->devices->size()) {
                MessageBoxW(window, L"The selected output is no longer valid.",
                    L"Audio Duplicator", MB_OK | MB_ICONERROR);
                return 0;
            }

            const auto& device = context->devices->at(static_cast<size_t>(deviceIndex));
            if (device.id == context->defaultDeviceId) {
                MessageBoxW(window,
                    L"The primary and secondary outputs must be different devices.",
                    L"Audio Duplicator", MB_OK | MB_ICONWARNING);
                return 0;
            }

            context->selectedDeviceId = device.id;
            context->accepted = true;
            DestroyWindow(window);
            return 0;
        }

        if (command == kCancelButtonId) {
            DestroyWindow(window);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureSelectionWindowClass(HINSTANCE instance) {
    WNDCLASSW existing = {};
    if (GetClassInfoW(instance, kWindowClassName, &existing)) return true;

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = SelectionWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    return RegisterClassW(&windowClass) != 0;
}

} // namespace

HRESULT EnumerateActiveRenderDevices(
    IMMDeviceEnumerator* enumerator,
    std::vector<AudioDeviceInfo>& devices,
    std::wstring& defaultDeviceId) {
    devices.clear();
    defaultDeviceId.clear();
    if (!enumerator) return E_POINTER;

    IMMDevice* defaultDevice = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (SUCCEEDED(hr)) {
        defaultDeviceId = GetDeviceId(defaultDevice);
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return hr;

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        collection->Release();
        return hr;
    }

    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device)) || !device) continue;

        AudioDeviceInfo info;
        info.id = GetDeviceId(device);
        info.name = GetDeviceFriendlyName(device);
        info.isDefault = !defaultDeviceId.empty() && info.id == defaultDeviceId;

        if (!info.id.empty()) {
            if (info.name.empty()) info.name = L"Unnamed playback device";
            devices.push_back(std::move(info));
        }
        device->Release();
    }

    collection->Release();
    return S_OK;
}

IMMDevice* FindRenderDeviceById(
    IMMDeviceEnumerator* enumerator,
    const std::wstring& deviceId) {
    if (!enumerator || deviceId.empty()) return nullptr;

    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDevice(deviceId.c_str(), &device))) return nullptr;
    return device;
}

const AudioDeviceInfo* FindAudioDeviceInfo(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& deviceId) {
    for (const auto& device : devices) {
        if (device.id == deviceId) return &device;
    }
    return nullptr;
}

bool ShowDeviceSelectionWindow(
    HINSTANCE instance,
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& defaultDeviceId,
    std::wstring& selectedDeviceId) {
    selectedDeviceId.clear();
    if (!EnsureSelectionWindowClass(instance)) {
        MessageBoxW(nullptr, L"Could not create the device selection window.",
            L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return false;
    }

    SelectionContext context;
    context.devices = &devices;
    context.defaultDeviceId = defaultDeviceId;

    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kWindowClassName,
        L"Audio Duplicator - Select outputs",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight,
        nullptr, nullptr, instance, &context);

    if (!window) {
        MessageBoxW(nullptr, L"Could not open the device selection window.",
            L"Audio Duplicator", MB_OK | MB_ICONERROR);
        return false;
    }

    CenterWindow(window);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (context.accepted) selectedDeviceId = context.selectedDeviceId;
    return context.accepted;
}

void PrintDeviceList(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& defaultDeviceId) {
    const bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    bool allocated = false;
    if (!attached) allocated = AllocConsole() != FALSE;

    FILE* output = nullptr;
    _wfreopen_s(&output, L"CONOUT$", L"w", stdout);

    std::wprintf(L"Audio Duplicator playback devices\n\n");
    for (size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        const bool isDefault = device.id == defaultDeviceId;
        std::wprintf(L"[%zu]%ls %ls\n", index,
            isDefault ? L" [Windows default]" : L"", device.name.c_str());
        std::wprintf(L"     ID: %ls\n\n", device.id.c_str());
    }
    std::fflush(stdout);

    if (allocated) {
        std::wprintf(L"Press Enter to close...\n");
        std::fflush(stdout);
        (void)std::getwchar();
        FreeConsole();
    }
}
