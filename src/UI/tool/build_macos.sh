#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
engine="$(cd ../.. && pwd)/build/bgiplayer"
if [ ! -x "$engine" ]; then echo "Build the engine with make in BGIplayer first." >&2; exit 1; fi
bash tool/bootstrap_platforms.sh macos
flutter analyze
flutter build macos --release
app="build/macos/Build/Products/Release/BGIplayer.app"
python3 tool/bundle_engine.py "$engine" "$app"
echo "Built: $app"
