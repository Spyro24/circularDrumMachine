#!/usr/bin/env bash
# Fetch the Steinberg VST3 SDK into vendor/vst3sdk (shallow, with submodules)
# and apply the small mingw fix we need. The plugin target needs the SDK; the
# standalone does not.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$root/vendor/vst3sdk"
patch="$root/patches/vst3sdk-mingw-aligned-alloc.patch"

if [ -f "$sdk/pluginterfaces/vst/vsttypes.h" ]; then
  echo "VST3 SDK already present at $sdk"
else
  echo "cloning the VST3 SDK into $sdk (about 250 MB)"
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/steinbergmedia/vst3sdk "$sdk"
  echo "done"
fi

# alignedalloc.h reaches for C11 std::aligned_alloc, which mingw-w64's C library
# does not have (it has _aligned_malloc). The header lives in the public.sdk
# submodule, so the patch goes there. It changes nothing on the other platforms
# and applies only once.
sub="$sdk/public.sdk"
if [ -f "$patch" ] && [ -d "$sub" ]; then
  if patch -p1 -d "$sub" --dry-run -N -s < "$patch" >/dev/null 2>&1; then
    patch -p1 -d "$sub" -N -s < "$patch"
    echo "applied $(basename "$patch")"
  else
    echo "$(basename "$patch") already applied"
  fi
fi
