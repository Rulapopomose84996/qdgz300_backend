#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/build/build_wsl_cross.sh

Environment:
  QDGZ300_BUILD_DIR       Build output directory (default: build_wsl_cross_dev)
  QDGZ300_TOOLCHAIN_FILE  Toolchain file path
  QDGZ300_BUILD_TYPE      CMake build type (default: Debug)
  QDGZ300_BUILD_TESTING   ON/OFF (default: ON)
  QDGZ300_RUN_TESTS       ON/OFF (default: OFF)
  QDGZ300_BUILD_SIMULATOR ON/OFF (default: OFF)
  QDGZ300_ENABLE_GPU      ON/OFF (default: OFF)
  QDGZ300_ENABLE_PROTOBUF ON/OFF (default: OFF)
  QDGZ300_BUILD_TARGET    Optional explicit build target
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_wsl_cross_dev}"
TOOLCHAIN_FILE="${QDGZ300_TOOLCHAIN_FILE:-${ROOT_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake}"
BUILD_TYPE="${QDGZ300_BUILD_TYPE:-Debug}"
BUILD_TESTING="${QDGZ300_BUILD_TESTING:-ON}"
RUN_TESTS="${QDGZ300_RUN_TESTS:-OFF}"
BUILD_SIMULATOR="${QDGZ300_BUILD_SIMULATOR:-OFF}"
ENABLE_GPU="${QDGZ300_ENABLE_GPU:-OFF}"
ENABLE_PROTOBUF="${QDGZ300_ENABLE_PROTOBUF:-OFF}"
BUILD_TARGET="${QDGZ300_BUILD_TARGET:-}"
TEST_REGEX="${QDGZ300_TEST_REGEX:-}"

log() {
  printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

die() {
  printf '\n[ERROR] %s\n' "$*" >&2
  exit 1
}

check_command() {
  command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"
}

log "Step 1/4: Validate WSL/Linux cross build environment"
[[ "$(uname -s)" == "Linux" ]] || die "This script must run in Linux/WSL."
check_command cmake
check_command ninja
check_command aarch64-linux-gnu-gcc
check_command aarch64-linux-gnu-g++
[[ -f "${TOOLCHAIN_FILE}" ]] || die "Toolchain file not found: ${TOOLCHAIN_FILE}"

log "Step 2/4: Configure cross build"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DBUILD_TESTING="${BUILD_TESTING}" \
  -DBUILD_SIMULATOR="${BUILD_SIMULATOR}" \
  -DENABLE_GPU="${ENABLE_GPU}" \
  -DENABLE_PROTOBUF="${ENABLE_PROTOBUF}"

log "Step 3/4: Build"
if [[ -n "${BUILD_TARGET}" ]]; then
  cmake --build "${BUILD_DIR}" --target "${BUILD_TARGET}"
else
  cmake --build "${BUILD_DIR}"
fi

log "Step 4/4: Run unit tests"
if [[ "${RUN_TESTS}" == "ON" && "${BUILD_TESTING}" == "ON" ]]; then
  ctest_cmd=(ctest --test-dir "${BUILD_DIR}/tests/unit" --output-on-failure)
  if [[ -n "${TEST_REGEX}" ]]; then
    ctest_cmd+=(-R "${TEST_REGEX}")
  fi
  "${ctest_cmd[@]}"
else
  log "Tests skipped; set QDGZ300_RUN_TESTS=ON and QDGZ300_BUILD_TESTING=ON to execute unit tests"
fi
