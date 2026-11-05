#!/usr/bin/env bash
# Build the Windows artifacts. Written for the CI runner (msys2, native mingw
# build) and for cross builds from Linux with mingw-w64 installed.
#
#   msys2 / Windows : ./scripts/build-windows.sh [all|standalone|vst3]
#   Linux cross     : ./scripts/build-windows.sh --cross [all|standalone|vst3]
#
# The plugin needs no SDL, so it cross-builds as soon as mingw-w64 and the VST3
# SDK are there. The standalone additionally needs mingw builds of SDL2,
# freetype, libpng and GLEW: under msys2 those come from the mingw-w64-* packages;
# for a cross build point CDM_WINDOWS_DEPS at a prefix that has them.
#
# Only our two targets are built, not the SDK's whole tree: mingw builds the
# SDK's validator as a GUI-subsystem executable whose entry point is main(),
# which does not link. moduleinfotool still builds, because the plugin's
# post-build step needs it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

cross=0
if [ "${1:-}" = "--cross" ]; then
  cross=1
  shift
fi
what="${1:-all}"

build="build/windows"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

standalone=ON
vst3=ON
case "$what" in
  standalone) vst3=OFF ;;
  vst3) standalone=OFF ;;
  all) ;;
  *) echo "usage: $0 [--cross] [all|standalone|vst3]" >&2; exit 2 ;;
esac

toolchain_args=()
if [ "$cross" = 1 ]; then
  if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "mingw-w64 is not installed: x86_64-w64-mingw32-g++ not found." >&2
    echo "Install it (apt install g++-mingw-w64-x86-64) or build on Windows." >&2
    exit 1
  fi
  toolchain_args+=("-DCMAKE_TOOLCHAIN_FILE=$root/cmake/toolchain-windows.cmake")
  if [ "$standalone" = ON ] && [ -z "${CDM_WINDOWS_DEPS:-}" ]; then
    echo "note: no CDM_WINDOWS_DEPS set, so the standalone target is skipped."
    echo "      The plugin target does not need SDL and will still build."
    standalone=OFF
  fi
fi

if [ "$vst3" = ON ]; then
  "$root/scripts/fetch-vst3sdk.sh"
fi

echo "== configuring ($what) =="
cmake -S . -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  "${toolchain_args[@]}" \
  -DCDM_BUILD_STANDALONE="$standalone" \
  -DCDM_BUILD_VST3="$vst3"

targets=()
[ "$standalone" = ON ] && targets+=(circular_drum_machine)
[ "$vst3" = ON ] && targets+=(circular_drum_machine_vst3)

echo "== building ${targets[*]} with $jobs jobs =="
cmake --build "$build" --parallel "$jobs" --target "${targets[@]}"

# The standalone links SDL2, freetype, libpng and GLEW as DLLs. Gather them,
# and their own mingw dependencies, beside the .exe so the folder can be shipped
# as it is. The plug-in needs none of this: it is linked statically.
exe="$build/circular_drum_machine.exe"
if [ "$standalone" = ON ] && [ -f "$exe" ] && command -v ldd >/dev/null 2>&1; then
  collected=0
  for dll in $(ldd "$exe" | awk '$2 == "=>" && $3 ~ /^\/mingw64\/bin\// {print $3}'); do
    cp -u "$dll" "$build/" && collected=$((collected + 1))
  done
  echo "collected $collected runtime DLLs beside the exe"
fi

echo
echo "== artifacts =="
find "$build" -maxdepth 5 \( -name '*.vst3' -o -name 'circular_drum_machine.exe' \) 2>/dev/null |
  while read -r p; do echo "$root/$p"; done
