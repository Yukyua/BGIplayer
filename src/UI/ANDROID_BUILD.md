# Android host and APK

The Android launcher runs the same C++ VM, archive readers, decoders, native
commands and compositors as the desktop engine. Flutter opens an SDL2
`GameActivity` in the application's `:engine` process. This is a real engine
build, not the UI's demo renderer. The first target is **arm64-v8a, Android 9
(API 28) or newer**.

## Build

Install Flutter, Java 21, and the official Android command-line SDK. Set
`ANDROID_SDK_ROOT` to its directory, then install and accept the SDK packages:

```sh
sdkmanager "platform-tools" "platforms;android-36" "build-tools;35.0.0" \
  "ndk;28.2.13676358" "cmake;3.22.1"
sdkmanager --licenses
src/UI/tool/build_android.sh
```

The helper builds pinned SDL 2.32.10, FFmpeg 8.0, FreeType 2.14.1, and GNU
libiconv 1.18 from upstream sources with verified SHA-256 digests. Android's
system iconv cannot replace GNU iconv for the engine's CP932/GBK conversion.
Downloads and generated objects stay in `build/android-native`, not source.
The NDK CMake target compiles all ordinary aggregate C++ translation units and
links the real SDL2 audio/video/input backend. It does not compile Metal or
Cocoa implementations. The output is:

```
src/UI/build/app/outputs/flutter-apk/app-release.apk
```

The current release build uses the standard local Android **debug signing
key**, so it can be sideloaded. Before distribution, supply your own release
signing configuration and preserve that key for upgrades. No game assets,
translations, saves, SDKs, or developer certificates are included in source.

## Games and input

Use the launcher's Import button to select a game directory with Android's
Storage Access Framework. Android grants URI access, not a POSIX directory;
the importer copies the selected tree into private app storage before the
engine starts. This is necessary because the shared engine performs seekable
filesystem reads and writes UserData beside the game. Existing imported game
directories are rejected rather than overwriting saves. Import can require
substantial free space and time for large games. Keep original game files as
a separate backup. Uninstalling the app removes these private copies.

The shared Android/iOS touch dispatcher maps a single-finger tap or drag to
left mouse input and a two-finger tap to one right click. It defers a stationary
first finger until release, preventing a two-finger tap from also advancing the
underlying scene. Button edges reach separate VM scheduler passes. A third
finger, movement beyond the gesture threshold, a 450 ms chord timeout, or
focus loss cancels the gesture. Physical mice retain their normal mapping. SDL
provides Android IME text input and physical keyboard events, and handles pause/resume from the Android
Activity lifecycle. Fullscreen/resolution changes from game scripts remain
disabled by the existing mobile window policy. Android's system font directory
is supplied to normal font discovery; the settings font picker enumerates
readable `/system/fonts` files, including CJK collections when the device has
them. The application's default icon is the Flutter launcher's default icon.

## GPU boundary

Android uses **OpenGL ES 3.1 compute** for scaled alpha, character crossfade,
fast affine sampling, mask transitions, character layer composition, and the
shared copy/fade/opacity/filter/clear primitives. Integer kernels retain the
CPU reference's shifts, alpha rules, paired pixels and correction table. The
fast affine path is the existing explicitly approximate rendering policy.
Both GPU settings use the same exact kernels; CPU exact bypasses them.

A private EGL pbuffer context saves and restores SDL's current context, so
compute operations cannot replace SDL's presentation context. SSBOs store
packed 32-bit words; this avoids requiring optional 8-bit storage extensions.
CPU-backed input buffers are uploaded, compute completes, and CPU destination
bytes are published only after a successful readback. This preserves native capture,
save thumbnails, and CPU fallbacks. Bitmap backing allocations can remain
in immutable GPU buffers without a decoded CPU mirror. They share ownership
of their EGL device, survive backend destruction, and read back only when
requested by CPU/native operations. Cross-thread reads serialize context use.
Layer and transfer kernels bind compatible resident source buffers directly,
without a source readback. Foreign contexts reject safely before writing.
The host releases retained display-frame/last-primary CPU copies by default.
Device capabilities, unavailable contexts,
unaligned four-byte pixel views, or dispatch failures select the original CPU
path without partial writes. GLES 3.0 devices still run using CPU composition
and accelerated SDL presentation. Composition calls may still read inputs
back for CPU native operations and synchronize compute output; GPU residency does not promise
a completely zero-copy renderer. Image decompression and glyph rasterization
remain CPU operations.

The EGL, storage, locale, input and lifecycle adapters are host changes; they
do not add guessed engine native semantics. Existing original-address evidence
for the shared pixel formulas is recorded in `docs/SEMANTICS.md` and
`docs/SUBSYSTEM_IMPLEMENTATIONS.md`.

## Validation

The Android-specific `tests/platforms/android/gpu_smoke.cpp` compares GPU bytes
against CPU scaled/crossfade references and exact formula fixtures for all
other integer kernels, including all 257 color-filter strengths, alpha
boundaries, odd row tails, padded views and alias rejection. It also requires
an actual fast affine dispatch. This executable is for device/emulator QA, including GPU allocation ownership,
parallel readback, PixelBuffer copy-on-write/zero CPU capacity, direct resident
source binding, and foreign-context rejection. It is not packaged into the
application. See the task report for the actual
APK installation, emulator/device and game runtime checks completed.

The API35 arm64 emulator validation passed 920 exact kernel fixtures and GPU
storage lifetime/cross-thread checks. The real has_t title, first saved scene
(including character/background/text), touch advance and stop/relaunch were
exercised on the frame-cache-release build. Shared frame tracing reported
`retained_cpu_pixels=0`. The subsequent all-bitmap residency and gesture build
compiled successfully but was not rerun in the emulator. This emulator
uses ANGLE/SwiftShader; a physical Android GPU/performance/device-driver check
is still required. The APK contains the ARM64 native engine, SDL2, C++ runtime,
Flutter runtime and upstream library license notices.

The mobile gesture unit tests cover one/two-finger taps, dragging, cancellation,
rapid taps and separate button edges. Android/iOS physical multi-touch testing
is still required. The emulator was stopped after GPU correctness checks to
avoid sustained software-renderer CPU load; no physical-GPU speedup is claimed.

## Effector selector kernels

The GLES Effector blur implements selectors 0–5: horizontal, vertical, or both
axes, with zero or clamped borders. Every pass preserves the original half-byte
input truncation and fixed reciprocal arithmetic on all four channels. Each
GPU invocation scans a row/column using a sliding sum, keeping work linear in
image size. Two-axis blur keeps its byte-quantized intermediate in an SSBO;
both dispatches share one fence/readback. Single-axis calls reject overlapping
CPU spans, preserving the CPU helper's original alias behavior. Radius-zero
cross-row aliases also select the CPU fallback; identical views are a no-op.

Validation without a simulator:

```sh
python3 tests/platforms/android/validate_shaders.py
c++ -std=c++20 -O2 -Iinclude tests/platforms/android/effector_blur_reference.cpp \
  -o build/android-effector-reference
./build/android-effector-reference
```

The independent direct-box-sum reference matches all 3,600 original-executable
selector cases, including format/alpha and padded destination guards. Both
GLES shaders compile offline, and the expanded native Android smoke executable
cross-links successfully. Run that executable on an ES 3.1 device with the
original selector fixture path as its optional argument to additionally check
3,600 GPU outputs. The new Effector GPU dispatch has not been executed on an
Android device; this offline validation does not establish driver correctness
or performance. APK rebuilding remains subject to the shared runtime frame
budget and load-peak checks.

The displacement Effector kernel also implements the original format-4 signed
Q4 maps: single/dual maps, strength endpoints, Q3/Q4 interpolation fractions,
16-bit coordinate/difference wrap and transparent samples outside the source.
It uses four SSBO bindings, retaining the ES 3.1 minimum-device requirement;
small argument blocks use uniforms instead of a fifth SSBO. Native Android
smoke tests compare padded/offset/random map layouts against the verified CPU
helper and accept a second optional fixture path for all 2,016 original map
cases. Shader validation and ARM64 cross-linking passed; new map GPU dispatch
is still awaiting physical-device verification.

The normal-vector Effector kernel supports format-6 six-byte map entries,
signed coefficient pairs, point/4-bit bilinear sampling and both rectangular
and linear-buffer boundaries. Byte addressing supports unaligned map words;
linear sampling uses extended integer multiplication rather than overflowing
32-bit coordinate products. Invalid coefficient indices and unsupported
aliases are rejected before any destination bytes are published.

`python3 tests/platforms/android/emulate_shaders.py` extracts the actual integer
GLSL function bodies into a temporary C++ vector shim and executes them against
all 5,904 original-executable records (3,600 selector, 2,016 displacement and 288
normal-vector cases). It passed including padded guards, offset views and
unaligned six-byte map entries. This tests shader math and addressing without
copying its formulas into another oracle; GLES-driver execution and dispatch
synchronization still require physical-device QA. All four ES 3.1 programs
compile offline and the expanded Android smoke executable links. Its third
optional fixture argument checks the 288 original normal-vector GPU outputs.

## Exact transforms and temporal history

The exact Effector transform is separate from the fast affine sampler. It uses
the CPU-verified six-term Q16 inverse matrix, wrapping integer accumulation,
nearest half-pixel bias or four-bit bilinear interpolation, and copies all four
channels. Source/target offsets and independent strides are retained.

Temporal history uses original paired/odd-tail rules for both opaque and alpha
formats. Integer Q16 preprocessing followed by the original 4,096-entry RCPSS
table avoids FMA-dependent evaluation; exhaustive alpha/strength comparison
covers 16,842,752 weight triples. A GPU work item owns one pixel pair and writes
both scene and history, including the complete retained scene beyond a cropped
history blend view. Non-overlapping rows and independent buffers make the
combined pass equivalent to blend followed by retain. Both CPU readbacks must
succeed before either output is published. Their temporary buffers are released
on return. Unsupported aliases/layouts retain the CPU path.

Current offline results: six GLSL ES 3.1 programs compile; ARM64 native smoke
links; actual shader-body execution matches 7,441 original records (3,600 blur,
2,016 displacement, 288 vector, 1,033 transform, 252 opaque temporal, 252 alpha temporal) and 180 additional
cropped-history/alpha-format CPU comparisons. Run the exhaustive weight check with:

```sh
c++ -std=c++20 -O2 -ffp-contract=off -Iinclude \
  tests/platforms/android/temporal_weights.cpp -o build/android-temporal-weights
./build/android-temporal-weights
```

Physical Android GPU validation and performance measurement remain pending.
Final APK construction follows shared integration and runtime QA.

Two-dimensional kernels use 16×8 work groups (128 invocations); row/column
blur uses 64×1. These fit the [ES 3.1 minimum compute limits](https://registry.khronos.org/OpenGL/specs/es/3.1/es_spec_3.1.pdf)
without assuming 256-thread groups on every Android driver.

Partial displacement views keep source dimensions separate from the destination.
An additional 44 padded 3×2-destination / 7×5-source cases compare the actual
shader body against the verified CPU helper, across single/dual maps, opaque/alpha
formats, point/bilinear sampling and endpoint strengths. The unsafe single-map
strength-zero whole-source-copy geometry is rejected before GPU writes.

## Removing imported copies

Android imports use the system SAF directory picker, then copy the complete
tree into `filesDir/Games/<name>` and run this private copy. The Android import
sheet does not accept arbitrary external absolute paths. From-library removal
deletes that private copy, including `UserData`; the external SAF source is not
modified. Both confirmation dialogs explicitly describe this behavior. Other
platforms retain their existing record-only removal behavior.

`bgiplayer/storage.removeGame` runs on the storage worker and accepts only an
existing canonical direct child of the private `Games` root. It rejects root,
external, traversal, symbolic-link game roots and active game paths. Links
inside a game are unlinked without following their targets. Failed deletion
returns a channel error and retains the library entry for retry; successful
deletion is awaited before saving the updated library. Launch is blocked during
removal; import/removal requests share one worker and reject overlapping storage
operations. The active engine copy cannot be removed.

The standalone Java boundary regression covers nine rejection/deletion cases,
including an external original and nested link; Flutter controller tests cover
awaited success and retained entries after failure. `flutter analyze` and the
seven Flutter tests pass. This is host storage policy, not a VM semantic change.


## Resident v2 background decoding

CompressedBG v2 resource/loose-file loading can execute original IDCT and color
conversion on GLES3.2 with precise float arithmetic. Huffman/DC/AC and alpha
parsing stay in the bounded CPU resource workers. Four8x8 blocks share a
32-thread group and1KiB transpose workspace; a16x8 color pass writes resident
BGRX/BGRA. Both passes use four SSBO bindings, one completion wait and a
four-byte error check. Output pixels reach the VM directly without a CPU
readback/upload cycle. All-image coefficient/sample staging is temporary and
can increase peak preparation memory compared with CPU stripe-at-a-time work.

GLES3.1 compositors remain supported and keep CPU image decoding. Devices or
buffer/shader failures select the original CPU decoder. BGI_GPU_DECODE=0
selects CPU decode while retaining GPU composition; CPU/keep-pixels host modes
also retain the CPU decoder. Default dispatch admits images of65,536 pixels
or more. Exact and fast modes use the same decoder math.

Nine GLES programs compile offline, including two ES3.2 decoder programs, and
the updated ARM64 native engine cross-links. Original-executable and random
pixel comparisons pass on real Metal. Physical Android driver correctness and
performance are still pending; see docs/GPU_BACKGROUND_DECODE_20261030.md.
