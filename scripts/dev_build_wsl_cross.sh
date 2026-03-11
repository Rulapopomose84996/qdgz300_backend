#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build_wsl_cross_dev}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${ROOT_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_TESTS="${BUILD_TESTS:-ON}"
RUN_TESTS="${RUN_TESTS:-OFF}"
BUILD_SIMULATOR="${BUILD_SIMULATOR:-OFF}"
ENABLE_GPU="${ENABLE_GPU:-OFF}"
ENABLE_PROTOBUF="${ENABLE_PROTOBUF:-OFF}"
BUILD_TARGET="${BUILD_TARGET:-}"
TEST_REGEX="${TEST_REGEX:-}"
CTEST_ARGS="${CTEST_ARGS:-}"
UNIT_TEST_DIR="${BUILD_DIR}/tests/unit"

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

log "Step 1/5: Validate Linux/WSL cross environment"
[[ "$(uname -s)" == "Linux" ]] || die "This script must run in Linux/WSL."
check_command cmake
check_command ninja
check_command aarch64-linux-gnu-gcc
check_command aarch64-linux-gnu-g++
[[ -f "${TOOLCHAIN_FILE}" ]] || die "Toolchain file not found: ${TOOLCHAIN_FILE}"

log "Step 2/5: Configure ARM64 cross build"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DBUILD_TESTING="${BUILD_TESTS}" \
  -DBUILD_SIMULATOR="${BUILD_SIMULATOR}" \
  -DENABLE_GPU="${ENABLE_GPU}" \
  -DENABLE_PROTOBUF="${ENABLE_PROTOBUF}"

log "Step 3/5: Build"
if [[ -n "${BUILD_TARGET}" ]]; then
  cmake --build "${BUILD_DIR}" --target "${BUILD_TARGET}"
else
  cmake --build "${BUILD_DIR}"
fi

log "Step 4/5: Run unit tests from stable subdirectory"
if [[ "${RUN_TESTS}" == "ON" && "${BUILD_TESTS}" == "ON" ]]; then
  [[ -d "${UNIT_TEST_DIR}" ]] || die "Unit test directory not found: ${UNIT_TEST_DIR}"
  ctest_cmd=(ctest --test-dir "${UNIT_TEST_DIR}" --output-on-failure)
  if [[ -n "${TEST_REGEX}" ]]; then
    ctest_cmd+=(-R "${TEST_REGEX}")
  fi
  if [[ -n "${CTEST_ARGS}" ]]; then
    # shellcheck disable=SC2206
    extra_ctest_args=(${CTEST_ARGS})
    ctest_cmd+=("${extra_ctest_args[@]}")
  fi
  "${ctest_cmd[@]}"
else
  log "Tests skipped; set RUN_TESTS=ON and BUILD_TESTS=ON to execute unit tests"
fi

log "Step 5/5: WSL cross build completed"
