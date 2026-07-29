# Hardware test checklist

Use this checklist before merging changes that affect device selection or WASAPI streaming.

1. Pair and connect two playback devices in Windows.
2. Set the first device as the Windows default output.
3. Launch `audio_duplicator.exe` and select the second device.
4. Start playback and confirm that both outputs receive stereo audio.
5. Confirm that selecting the default output as the secondary output is rejected.
6. Stop the application from the notification-area icon.

## Bluetooth checks

- Prefer the stereo/headphones endpoint rather than a hands-free/headset endpoint.
- Confirm that no application is actively using an AirPods microphone during playback.
- Note any delay, crackling, dropout, or progressive loss of synchronization.

## Regression: shared event-driven buffer

Both `IAudioClient::Initialize` calls must pass `0` for `hnsBufferDuration` and `hnsPeriodicity` when using `AUDCLNT_SHAREMODE_SHARED` with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`. A fixed duration can be rejected by Bluetooth audio drivers with `AUDCLNT_E_UNSUPPORTED_FORMAT` (`0x88890008`).
