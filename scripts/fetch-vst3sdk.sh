#!/usr/bin/env bash
# Fetch the Steinberg VST3 SDK into vendor/vst3sdk (shallow, with submodules)
# and apply the small mingw fix we need. The plugin target needs the SDK; the
# standalone does not.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$root/vendor/vst3sdk"

if [ -f "$sdk/pluginterfaces/vst/vsttypes.h" ]; then
  echo "VST3 SDK already present at $sdk"
else
  echo "cloning the VST3 SDK into $sdk (about 250 MB)"
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/steinbergmedia/vst3sdk "$sdk"
  echo "done"
fi

# alignedalloc.h reaches for C11 std::aligned_alloc, which mingw-w64's C library
# does not have (it has _aligned_malloc). Teach that header about mingw: it is a
# no-op on the other platforms.
#
# Plain substitutions rather than a context patch, because the file lives in the
# public.sdk submodule and a Windows checkout may have rewritten it to CRLF,
# which makes patch(1) and git apply reject the hunks. The three patterns are
# unanchored for the same reason. The patterns are ordered so that no line can
# be rewritten twice.
header="$sdk/public.sdk/source/vst/utility/alignedalloc.h"
if [ -f "$header" ]; then
  if grep -q '__MINGW32__' "$header"; then
    echo "aligned_alloc: already patched"
  else
    sed -i \
      -e 's/#if defined(_MSC_VER)/#if defined(_MSC_VER) || defined(__MINGW32__)/' \
      -e 's/#ifdef _MSC_VER/#if defined(_MSC_VER) || defined(__MINGW32__)/' \
      -e 's/#elif defined(_MSC_VER)/#elif defined(_MSC_VER) || defined(__MINGW32__)/' \
      "$header"
    patched=$(grep -c '__MINGW32__' "$header" || true)
    if [ "$patched" -ne 3 ]; then
      echo "error: aligned_alloc patch touched $patched lines, expected 3" >&2
      exit 1
    fi
    echo "aligned_alloc: patched $patched lines"
  fi
fi
