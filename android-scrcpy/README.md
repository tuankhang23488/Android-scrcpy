# android-scrcpy — C++ Win32, raw H.264, NO FFmpeg

**A standalone Windows C++ GUI project (source, not a precompiled EXE)** for listing Android devices and opening a separate live-view/control window per selected device.

## Explicit dependencies / exclusions

- Never executes, embeds, or links `ffmpeg`, `ffmpeg.exe`, `scrcpy.exe` or `mirroid.exe`.
- The Windows application itself is built as `android-scrcpy.exe` from `src/main.cpp`.
- **Requires the official Android SDK `adb.exe` + its DLL files in `platform-tools/`** next to the EXE; ADB is a distinct transport/development tool, not scrcpy. It is not reimplemented here.
- H.264 source: the Android **built-in** `screenrecord --output-format=h264 -` encoder via `adb exec-out`.
- H.264 decoder: Microsoft's built-in **Media Foundation H.264 MFT**, output NV12, converted to BGRA for Win32 GDI.
- No Android app or scrcpy-server JAR installed.

## Build on Windows

Prerequisites: Windows 10/11 x64, CMake >= 3.20, Microsoft Visual Studio 2022 (Desktop development with C++ and Windows SDK).

From **PowerShell** (with Visual Studio C++ Build Tools installed), run `./build.ps1`, or use:

```powershell
cd android-scrcpy
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build/Release/android-scrcpy.exe`. Create the following **distribution**:

```
android-scrcpy/
  android-scrcpy.exe                  <- copy from build/Release
  platform-tools/
    adb.exe
    AdbWinApi.dll
    AdbWinUsbApi.dll
    ...                                <- entire official platform-tools directory
```

Download SDK Platform-Tools from the official Android developer site:
https://developer.android.com/tools/releases/platform-tools

Run `android-scrcpy.exe`, enable **Developer Options > USB debugging** on phones, grant their RSA prompts, then click **Refresh**.

## Use

1. Device list shows serial, ADB status and optional model (list is a normal Win32 ListView and can show 100+ entries).
2. Select a device and click **Start**, or double-click. Each active device gets its own view window.
3. Click on phone screen to tap; drag to swipe. Keyboard: Esc=Back, Home=Home, Backspace=Delete, Enter=Enter, arrow keys=DPAD.
4. Select device and press **Stop**, or close its viewer window.
5. Closing the main window stops all active streams.

## Limitations — important

This is an **MVP/source project**, not a tested production binary. It has not been built or run against a physical phone in this environment. The standard Windows H.264 MFT can behave differently on different Windows builds, and compatible stream formats/device sizes may need additional handling. If it produces a black screen, check the device's `screenrecord` output, chosen resolution and Media Foundation negotiation.

- Device count in the list is not limited to 100. **100 simultaneous 720×1280 live streams is not a guaranteed capability**; PC CPU/GPU/RAM/USB bandwidth and device encoders become limiting. Each active view uses independent decoder and ADB pipe. For fleets, start only the windows you need.
- `screenrecord` normally stops after approximately **180 seconds**; the streaming loop reconnects, causing a brief gap. A custom on-device `MediaCodec` agent is needed for continuous scrcpy-like low-latency performance and reliable rotation support.
- Fixed requested capture size 720×1280. On some landscape devices / devices that reject that size, this must be adapted (e.g., 720×1280 vs 1280×720). Touch mapping assumes frame dimensions match the touchscreen coordinate system; rotated or scaled content may have mismatches. For exact touch mapping, get `wm size`/display rotation and transform coordinates.
- No audio, clipboard sync, multi-touch, rich keyboard text injection, or key repeat handling. ADB `input` commands may be slower than scrcpy's control socket.
- Current output is decoded NV12 converted on CPU to BGRA, then GDI-rendered; for many concurrent active sessions, optimize with DXVA/D3D11 and GPU surfaces.
- Some apps block screen capture; standard Android restrictions apply.
- Do not attempt to control devices without their owner's authorization.

## Next development stages

1. Add an Android-side MediaCodec + virtual-display capture service; keep C++ Win32 frontend and MF/D3D11 decoding.
2. Make H.264 parser parse SPS/PPS/VUI and slice headers robustly (currently a basic access-unit grouping implementation).
3. Add per-device resolution, rotation, input channels, ADB disconnect detection, async device discovery and logging.
4. Use D3D11 video surfaces and GPU compositor for 10s/100s of active devices; add thumbnails, grid view, concurrency throttling and pagination.

## License

Source example provided for modification; use official Android SDK Platform-Tools under its own applicable license.


## Linker fix (LNK2019)

The application discovers an installed Windows Media Foundation H.264 video decoder at runtime via `MFTEnumEx`. It no longer references `CLSID_CMSH264DecoderMFT`, so it does not depend on resolving that GUID at link time. This is still native Windows Media Foundation decoding and does not use FFmpeg.
