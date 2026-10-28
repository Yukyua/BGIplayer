#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-"$ROOT/build/android-sdk"}
export ANDROID_SDK_ROOT ANDROID_HOME="$ANDROID_SDK_ROOT"
export GRADLE_USER_HOME=${GRADLE_USER_HOME:-"$ROOT/build/android-gradle"}
FLUTTER_ROOT=${FLUTTER_ROOT:-$(python3 -c 'import pathlib,shutil;print(pathlib.Path(shutil.which("flutter")).resolve().parents[1])')}
export CMAKE_BUILD_PARALLEL_LEVEL=2
python3 "$ROOT/src/UI/tool/build_android_native.py" --sdk "$ANDROID_SDK_ROOT"
flutter precache --android
# Flutter ships the standard wrapper JAR; keep generated binaries out of source.
cp "$FLUTTER_ROOT/bin/cache/artifacts/gradle_wrapper/gradle/wrapper/gradle-wrapper.jar" "$ROOT/src/UI/android/gradle/wrapper/gradle-wrapper.jar"
printf 'sdk.dir=%s\nflutter.sdk=%s\n' "$ANDROID_SDK_ROOT" "$FLUTTER_ROOT" > "$ROOT/src/UI/android/local.properties"
cd "$ROOT/src/UI"
flutter pub get
flutter build apk --release --target-platform android-arm64
