#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ARCH="$(uname -m)"

case "${ARCH}" in
  aarch64|arm64)
    TRIPLET="${TRIPLET:-arm64-linux}"
    ;;
  x86_64|amd64)
    TRIPLET="${TRIPLET:-x64-linux}"
    ;;
  *)
    echo "Unsupported arch: ${ARCH}" >&2
    exit 1
    ;;
esac

echo "[titanbench] build type=${BUILD_TYPE}, triplet=${TRIPLET}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DVCPKG_TARGET_TRIPLET="${TRIPLET}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

mkdir -p "${ROOT_DIR}/dist"
cp "${BUILD_DIR}/titanbench" "${ROOT_DIR}/dist/titanbench-${TRIPLET}"
strip "${ROOT_DIR}/dist/titanbench-${TRIPLET}" || true

echo "[titanbench] output: ${ROOT_DIR}/dist/titanbench-${TRIPLET}"
