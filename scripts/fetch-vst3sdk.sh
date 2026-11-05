#!/usr/bin/env bash
# Fetch the Steinberg VST3 SDK into vendor/vst3sdk (shallow, with submodules).
# The plugin target needs it; the standalone does not.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$root/vendor/vst3sdk"

if [ -f "$sdk/pluginterfaces/vst/vsttypes.h" ]; then
  echo "VST3 SDK already present at $sdk"
  exit 0
fi

echo "cloning the VST3 SDK into $sdk (about 250 MB)"
git clone --depth 1 --recurse-submodules --shallow-submodules \
  https://github.com/steinbergmedia/vst3sdk "$sdk"
echo "done"
