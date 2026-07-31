# Hardware test checklist

Use the newest `audio-duplicator-windows-x64-unsigned` artifact from the pull request's latest successful Windows build. Do not reuse an executable from an earlier workflow run.

## Setup

1. Connect both pairs of headphones to Windows.
2. Confirm that both appear as stereo playback outputs in **Settings > System > Sound**.
3. Set the first pair as the Windows default playback output.
4. Close applications that may be using either AirPods microphone.
5. Launch the newest `audio_duplicator.exe` and select the second pair.

## Expected result

- Clicking **Start** does not show a WASAPI initialization error.
- Audio plays through both pairs.
- The tray icon appears and can stop the application.
- Both devices retain normal stereo quality.

## Report if the test fails

Send a screenshot of the complete error dialog. The current build reports:

- selected output name;
- captured sample rate, channel count, and bit depth;
- the result of the native-format attempt;
- the result of the PCM 16-bit compatibility attempt.

Also note whether the selected AirPods appeared as a stereo output or a hands-free/headset output in Windows Sound settings.
