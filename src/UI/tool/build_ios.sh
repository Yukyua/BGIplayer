#!/usr/bin/env bash
# Build the Flutter library and real embedded iOS engine, then package the unsigned device IPA.
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-simulator}"
if [ ! -d ios ]; then bash tool/bootstrap_platforms.sh ios; fi
flutter pub get
python3 tool/build_ios_native.py "$mode"
case "$mode" in
  simulator) flutter build ios --simulator --debug ;;
  device) flutter build ios --release --no-codesign
    python3 tool/package_ios_ipa.py ;;
  *) echo "Usage: bash tool/build_ios.sh [simulator|device]" >&2; exit 2 ;;
esac
