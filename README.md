# Audio Duplicator

A lightweight Windows utility that duplicates your default audio playback device to any secondary audio device - with near-zero latency, no virtual cables, and no third-party drivers.

Runs silently in the background with a system tray icon. Right-click the tray icon to stop it.

---

## How It Works

There are two devices involved:

- **Device 1 - Your main device** (chosen by you in Windows): This is whatever you set as your default playback device in Windows Sound settings - your speakers, headphones, TV, Bluetooth device, etc. You control this normally through the Windows volume menu. Audio Duplicator does not touch this setting.

- **Device 2 - The duplicated device** (chosen by the app): This is the secondary device you pass as an argument (e.g. `"USB Audio"`). Audio Duplicator listens to whatever is playing on Device 1 and sends an exact copy of that audio to Device 2 simultaneously, in real time.

Under the hood, Audio Duplicator uses the **Windows Core Audio API (WASAPI)**:

- **Loopback capture** - grabs the audio stream from your default playback device right before it hits your speakers
- **Event-driven render** - immediately writes it to your chosen secondary device using a 10ms buffer
- **Built-in resampler** - automatically handles sample rate differences (e.g. 48kHz vs 44.1kHz) between devices

No audio settings are changed. No drivers are installed. The program just copies the stream while it runs.

---

## Requirements

- Windows 10 or 11
- A secondary audio output device (USB adapter, HDMI, Bluetooth, etc.)

---

## Usage

**Double-click** `audio_duplicator.exe` to start.

By default it looks for a device named **"Speakers"**. You can target a different device by passing a name substring as an argument:

```
audio_duplicator.exe "USB Audio"
audio_duplicator.exe "HDMI"
audio_duplicator.exe "Realtek"
```

The search is **case-insensitive** and matches any part of the device name shown in Windows Sound settings.

**To stop:** right-click the tray icon (bottom-right corner, near the clock) → **Stop Audio Duplicator**.

---

## Building from Source

### Option A - MSVC (Visual Studio)

1. Open a **Developer Command Prompt** (search for it in the Start menu)
2. Navigate to the project folder
3. Run:
```
cl /EHsc /O2 audio_duplicator.cpp /link ole32.lib mmdevapi.lib avrt.lib
```

Or open the project in Visual Studio 2022:
- Create a new **Empty C++ Project**
- Add `audio_duplicator.cpp` to Source Files
- Set **Configuration** to `Release` and **Platform** to `x64`
- Go to **Project → Properties → Linker → System → SubSystem** → set to `Windows`
- Go to **Linker → Input → Additional Dependencies** → add `ole32.lib;mmdevapi.lib;avrt.lib`
- Build → **Build Solution** (`Ctrl+Shift+B`)

### Option B - MinGW / g++

```
g++ -std=c++17 -O2 -o audio_duplicator.exe audio_duplicator.cpp -lole32 -lmmdevapi -lavrt -luuid
```

### Required headers (all standard Windows SDK - no downloads needed)

| Header | Purpose |
|---|---|
| `mmdeviceapi.h` | Device enumeration |
| `audioclient.h` | WASAPI audio streaming |
| `avrt.h` | Real-time thread priority |
| `shellapi.h` | System tray icon |
| `functiondiscoverykeys_devpkey.h` | Device friendly name |

### Required libraries

| Library | Purpose |
|---|---|
| `ole32.lib` | COM initialization |
| `mmdevapi.lib` | WASAPI device API |
| `avrt.lib` | Audio thread scheduling |

---

## Tips

- If the target device isn't found, check its exact name in **Settings → System → Sound → More sound settings → Playback tab**. Use any unique substring of that name as the argument.
- If you hear crackling or dropouts, your device may need a larger buffer. Change the `100000` value (10ms) in both `Initialize()` calls in the source to `200000` (20ms).
- The program works with any device Windows recognizes as an audio output: USB adapters, HDMI, Bluetooth speakers, virtual devices, etc.

---

## License

MIT - see [LICENSE](LICENSE)
