#!/usr/bin/env bash
# Official AMD V5 compile commands; no additional CMake/build options.
set -Eeuo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BRIDGE_DIR=${1:?Usage: bash tools/hipvs_bridge/build_contest.sh /absolute/bridge/output}
[[ "$BRIDGE_DIR" == /* ]] || { echo 'Bridge output must be an absolute Linux path' >&2; exit 2; }
mkdir -p "$BRIDGE_DIR"
cd "$ROOT"
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh "$BRIDGE_DIR/libseekdb_cuvs_bridge.so"
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$BRIDGE_DIR/libseekdb_cuvs_bridge.so"
cmake --build build_release --target seekdb
test -x build_release/src/observer/seekdb
test -s "$BRIDGE_DIR/libseekdb_cuvs_bridge.so"
