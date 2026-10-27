#!/usr/bin/env bash
# Generate platform shells without replacing the existing launcher sources.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v flutter >/dev/null 2>&1; then
  echo "Flutter is not available in PATH yet." >&2
  exit 1
fi
platforms="${1:-macos}"
task_tmp="$(mktemp -d)"
trap 'rm -rf "$task_tmp"' EXIT
flutter create --project-name bgi_ui_demo --org com.bgiplayer --platforms="$platforms" "$task_tmp"
IFS=',' read -ra selected <<< "$platforms"
for platform in "${selected[@]}"; do
  if [ ! -d "$platform" ]; then cp -R "$task_tmp/$platform" "$platform"; fi
done
if [ ! -f .metadata ]; then cp "$task_tmp/.metadata" .metadata; fi
# This desktop launcher executes an external SDL host and writes game UserData.
# It is a local distribution app, not an App Store sandboxed app.
if [ -d macos/Runner ]; then
  python3 - <<'CONFIG'
from pathlib import Path
import plistlib
for path in Path('macos/Runner').glob('*.entitlements'):
    data=plistlib.loads(path.read_bytes())
    data.pop('com.apple.security.app-sandbox', None)
    path.write_bytes(plistlib.dumps(data))
CONFIG
fi
flutter pub get
