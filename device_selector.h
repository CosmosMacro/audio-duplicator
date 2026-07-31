#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

#include <string>
#include <vector>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

HRESULT EnumerateActiveRenderDevices(
    IMMDeviceEnumerator* enumerator,
    std::vector<AudioDeviceInfo>& devices,
    std::wstring& defaultDeviceId);

IMMDevice* FindRenderDeviceById(
    IMMDeviceEnumerator* enumerator,
    const std::wstring& deviceId);

const AudioDeviceInfo* FindAudioDeviceInfo(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& deviceId);

bool ShowDeviceSelectionWindow(
    HINSTANCE instance,
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& defaultDeviceId,
    std::wstring& selectedDeviceId);

void PrintDeviceList(
    const std::vector<AudioDeviceInfo>& devices,
    const std::wstring& defaultDeviceId);
