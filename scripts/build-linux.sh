#!/usr/bin/env bash
# Build the Linux artifacts:
#   build/linux/circular_drum_machine            standalone
#   build/linux/VST3/.../circular_drum_machine.vst3   plugin bundle
#
#   ./scripts/build-linux.sh              both
#   ./scripts/build-linux.sh standalone   only the app
#   ./scripts/build-linux.sh vst3         only the plugin
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

what="${1:-all}"
build="build/linux"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

standalone=ON
vst3=ON
case "$what" in
  standalone) vst3=OFF ;;
  vst3) standalone=OFF ;;
  all) ;;
  *) echo "usage: $0 [all|standalone|vst3]" >&2; exit 2 ;;
esac

if [ "$vst3" = ON ]; then
  "$root/scripts/fetch-vst3sdk.sh"
fi

echo "== configuring ($what) =="
cmake -S . -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCDM_BUILD_STANDALONE="$standalone" \
  -DCDM_BUILD_VST3="$vst3"

echo "== building with $jobs jobs =="
cmake --build "$build" --parallel "$jobs"

echo
echo "== artifacts =="
[ -x "$build/circular_drum_machine" ] && echo "standalone: $root/$build/circular_drum_machine"
find "$build" -maxdepth 4 -name '*.vst3' -type d 2>/dev/null | while read -r p; do
  echo "vst3:       $root/$p"
done
