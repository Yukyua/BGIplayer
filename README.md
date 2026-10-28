# BGIplayer

Fresh C++20/SDL2 implementation of the Buriko General Interpreter (BGI).

This project is intentionally independent from the existing `openbgi` source
tree. Native behavior is derived from Ghidra decompilation of the target
executable and tracked in `docs/SEMANTICS.md`.

The active frontier, uncommitted-work status, and restart instructions are
recorded in `docs/CURRENT_STATE.md`.

Physical file access and Windows-to-host path mapping are documented in
[`docs/HOST_FILE_SYSTEM.md`](docs/HOST_FILE_SYSTEM.md). Save/load compression
and VM wait semantics remain separate from that host interface.

Content checks use `BGI_CHECK_LEVEL=warn` (default) or `strict`. Structural
errors always reject the operation. See [`docs/CHECK_LEVELS.md`](docs/CHECK_LEVELS.md)
for the classification and affected formats.

The reusable [`bgi-runtime-debug` skill](skills/bgi-runtime-debug/SKILL.md)
provides a step-by-step runtime investigation workflow, targeted trace guidance,
and read-only log/SUD inspection tools for agents continuing this port.

## Current milestone

The bootstrap currently provides:

- PackFile and BURIKO ARC20 index parsing;
- case-insensitive game archive discovery;
- script header extraction;
- the BP operand/frame stack primitives and a growing opcode core;
- an SDL2 window/event loop and a null backend for tests;
- FFmpeg archive audio demux/decode into 48 kHz stereo S16 PCM and SDL2
  BGM/effect mixing;
- all `760/760` in-scope runtime native slots registered with Ghidra-derived
  ABI/status/state semantics;
- BG loading including uncompressed and `CompressedBG___`
  predictor/RLE/Huffman codecs;
- a deterministic self-test binary.

The runtime target is the 760 in-scope native slots counted by
`scripts/native_progress.pl`, not a compatibility fallback. Developer
diagnostics, installer, network, optical-drive and removable-media slots are
outside the runtime scope and do not block completion.

Native-slot registration is complete, but engine completion still requires
consuming all recorded renderer commands, scheduler/wait parity, host audio and
movie backends, and remaining text/input integration. `docs/CURRENT_STATE.md`
tracks that backend frontier.

## Build

```sh
make
make test
./build/bgiplayer --game "../放課後シンデレラ" --backend null --list-archives
./build/bgiplayer --game "../放課後シンデレラ" --backend sdl2
./build/bgiplayer --game "../放課後シンデレラ" \
  --dump-bitmap Archive/data02800.arc:title_ani_logo \
  --dump-bitmap-output tmp_shot/title_ani_logo.bmp
SDL_VIDEODRIVER=dummy ./build/bgiplayer \
  --game "../放課後シンデレラ" --backend sdl2 --seconds 0.1
```

Build dependencies: zlib, SDL2 plus FFmpeg development libraries exposed through
`pkg-config` (`libavformat`, `libavcodec`, `libavutil`, `libswresample`,
`libswscale`).

On macOS, large verified sprite scaling and crossfade operations automatically
use Metal compute, with exact CPU fallback. No extra GPU SDK or shader compiler
installation is needed. Use `BGI_GPU=0` to force CPU, `BGI_GPU=1` to request GPU
at every size, and `BGI_TRACE_GPU=1` to see actual dispatch counts. Other
platforms currently use the portable CPU kernels. See
[`docs/GPU_PIXEL_KERNELS.md`](docs/GPU_PIXEL_KERNELS.md) for supported paths,
pixel comparisons, and measured costs.

For a reproducible input sequence, repeat `--click` in order. For example,
`--click 320,610 --click 472,269 --click-delay-ms 15000 --click-interval-ms 15000`
presses Start at15seconds and the first prologue card at30seconds. Both
headless and SDL hosts release each press at that same point after150ms.
The click count is at least the number of points; a larger `--click-count`
repeats the final point. A single point retains the existing repeated-click
behavior. These host inputs do not alter script state or selection results.
Use `--record-clicks logs/clicks.csv` to save mouse motion and left/right
down/up events, `--replay-clicks logs/clicks.csv` to run them automatically,
and `--trace-clicks` for readable terminal output. See
[`docs/CLICK_RECORDING.md`](docs/CLICK_RECORDING.md) for timing and replay limits.
Use `--right-click X,Y` in the same sequence to inject the right button
instead of the left button. Coordinates are engine logical pixels. Both
buttons use the normal input records and are released after150ms.

### Android

Android 9+ ARM64 is supported by the Flutter launcher and the native SDL2
engine. Install Android SDK/NDK 28.2.13676358 and CMake 3.22.1, set
`ANDROID_SDK_ROOT`, and run `src/UI/tool/build_android.sh`. The APK is generated
at `src/UI/build/app/outputs/flutter-apk/app-release.apk`. Import a game folder
through the Android directory picker; the application copies it into private
storage and keeps UserData there. GLES 3.1 compute accelerates supported
composition operations, with a CPU fallback on other devices. The current
release APK uses the local Android debug signing key for sideloading.

### GPU frame memory

GPU presentation releases the completed frame's CPU pixel caches by default.
VM bitmap resources and window planes also move to GPU storage, with native
pixel operations, screenshots and debug snapshots reading bytes back on demand.
Use `BGI_KEEP_CPU_PIXELS=1` to retain CPU pixel copies for debugging, or `BGI_GPU=0`
for CPU rendering. Intermediate compute operations still use synchronous
transfers; this is not a fully resident GPU resource graph.

Enable `BGI_TRACE_GPU_FRAMES=1` to record full frame, render-phase and callback
costs. Use `python3 tools/analyze_frame_budget.py run.log --output frame-cost`
to export CSV/JSON, separating present waits from work against a 16.67 ms budget.
