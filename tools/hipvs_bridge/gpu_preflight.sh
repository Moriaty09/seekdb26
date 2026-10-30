#!/usr/bin/env bash
# Read-only environment inspection. Writes evidence files, installs nothing.
set -Eeuo pipefail
EVIDENCE=${1:?Usage: bash tools/hipvs_bridge/gpu_preflight.sh /path/to/evidence}
mkdir -p "$EVIDENCE"
for command in gcc cmake rustc rocminfo; do
  command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 2; }
done
test -x /opt/rocm/bin/hipcc
test -f /opt/hipvs/include/cuvs/core/c_api.h
test -f /opt/hipvs/lib/libcuvs_c.so
{
  date -u '+%Y-%m-%dT%H:%M:%SZ'
  uname -a
  gcc --version
  cmake --version
  rustc --version
  /opt/rocm/bin/hipcc --version
  for file in /opt/rocm/.info/version /opt/rocm/.info/version-dev; do
    if [[ -f "$file" ]]; then cat "$file"; fi
  done
} > "$EVIDENCE/toolchain.txt"
rocminfo > "$EVIDENCE/rocminfo.txt"
if command -v amd-smi >/dev/null; then
  amd-smi static > "$EVIDENCE/gpu.txt"
elif command -v rocm-smi >/dev/null; then
  rocm-smi --showproductname --showmeminfo vram > "$EVIDENCE/gpu.txt"
else
  echo 'GPU management CLI not found; record model and VRAM manually.' > "$EVIDENCE/gpu.txt"
fi
sha256sum /opt/hipvs/lib/libcuvs_c.so > "$EVIDENCE/hipvs-library.sha256"
if git -C /opt/hipvs rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C /opt/hipvs rev-parse HEAD > "$EVIDENCE/hipvs-source-commit.txt"
else
  echo 'Installed library has no Git metadata; confirm fixed hipVS commit with image provider.' > "$EVIDENCE/hipvs-source-commit.txt"
fi
echo "Environment recorded in $EVIDENCE; compare versions/device against official requirements."
