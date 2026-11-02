#!/bin/bash -e
# Validate res/hdr.wgsl with the same Tint the game's Dawn uses (built by
#   cmake -S build-dawn/src -B build-dawn/build-tint -DTINT_BUILD_CMD_TOOLS=ON ... --target tint).
# A WGSL error otherwise only shows up as a fatal crash at game start.
TINT="${TINT:-/media/ESSD/Dusklight/build-dawn/build-tint/tint}"
cd "$(dirname "$0")/.."
for ep in vs_main fs_main; do
  "$TINT" res/hdr.wgsl --format spirv --ep "$ep" -o /dev/null
done
echo "res/hdr.wgsl: OK"
