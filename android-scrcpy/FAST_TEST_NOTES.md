# Fast-test version (no FFmpeg)

This is a targeted performance experiment, not a low-latency scrcpy replacement.

Changes vs linkfix:
- Capture at 540x960, 1.5Mbps instead of 720x1280, 2Mbps, lowering CPU conversion, USB traffic and paint work.
- Use GDI COLORONCOLOR rather than HALFTONE to reduce rendering overhead.
- Coalesce pending frame notifications to avoid a growing Win32 message queue.
- Query `adb shell wm size` once so click coordinates are based on the actual Android input resolution even while capturing a scaled stream.

Run: `powershell -ExecutionPolicy Bypass -File .\\build.ps1`
Copy/retain the official `platform-tools` folder adjacent to the rebuilt EXE.

**Known limitations:**
- `screenrecord` can still start slowly, introduces substantial latency, and periodically stops/restarts.
- It is NOT guaranteed to achieve 1-2 second startup or stable interactive frame rates.
- Orientation changes are not fully accounted for in input mapping.
- Scaling to 100 concurrent streams requires a different MediaCodec-based Android streaming agent, a persistent control channel, and a GPU-friendly PC renderer.
- Source edits have NOT been compiled/tested on Windows in this environment.
