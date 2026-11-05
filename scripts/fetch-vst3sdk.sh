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
# does not have (it has _aligned_malloc). The header is part of the public.sdk
# submodule, so the patch is applied there. It applies once, and changes nothing
# on the other platforms. Dependency-free: `patch` is not installed everywhere,
# but the clone means git is.
sub="$sdk/public.sdk"
header="$sub/source/vst/utility/alignedalloc.h"
if [ -f "$patch" ] && [ -f "$header" ]; then
  if grep -q '__MINGW32__' "$header"; then
    echo "aligned_alloc patch already applied"
  elif git -C "$sub" apply --check "$patch" 2>/dev/null; then
    git -C "$sub" apply "$patch"
    echo "applied $(basename "$patch")"
  else
    echo "error: $patch does not apply to $header" >&2
    exit 1
  fi
  if ! grep -q '__MINGW32__' "$header"; then
    echo "error: $header is still unpatched" >&2
    exit 1
  fi
fi
