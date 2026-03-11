#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build_production}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_TESTS="${BUILD_TESTS:-ON}"
RUN_TESTS="${RUN_TESTS:-ON}"
BUILD_SIMULATOR="${BUILD_SIMULATOR:-OFF}"
ENABLE_GPU="${ENABLE_GPU:-ON}"
ENABLE_PROTOBUF="${ENABLE_PROTOBUF:-OFF}"
BUILD_TARGET="${BUILD_TARGET:-}"
TEST_REGEX="${TEST_REGEX:-}"
CTEST_ARGS="${CTEST_ARGS:-}"
COREX_ROOT="${COREX_ROOT:-/usr/local/corex}"
COREX_CLANGXX="${COREX_CLANGXX:-${COREX_ROOT}/bin/clang++}"
COREX_CLANG="${COREX_CLANG:-${COREX_ROOT}/bin/clang}"
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

check_file() {
  [[ -f "$1" ]] || die "Missing required file: $1"
}

log "Step 1/5: Validate production environment"
[[ "$(uname -s)" == "Linux" ]] || die "Production build must run on Linux."
check_command cmake
check_command ninja
check_file "${ROOT_DIR}/CMakeLists.txt"

if [[ "${ENABLE_GPU}" == "ON" ]]; then
  check_file "${COREX_CLANGXX}"
  check_file "${COREX_CLANG}"
  export PATH="${COREX_ROOT}/bin:${PATH}"
  export LD_LIBRARY_PATH="${COREX_ROOT}/lib64:${COREX_ROOT}/lib:${LD_LIBRARY_PATH:-}"
fi

log "Step 2/5: Configure native build"
cmake_args=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -G Ninja
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DBUILD_TESTING="${BUILD_TESTS}"
  -DBUILD_SIMULATOR="${BUILD_SIMULATOR}"
  -DENABLE_GPU="${ENABLE_GPU}"
  -DENABLE_PROTOBUF="${ENABLE_PROTOBUF}"
)

if [[ "${ENABLE_GPU}" == "ON" ]]; then
  cmake_args+=(
    -DCMAKE_C_COMPILER="${COREX_CLANG}"
    -DCMAKE_CXX_COMPILER="${COREX_CLANGXX}"
  )
fi

cmake "${cmake_args[@]}"

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

log "Step 5/5: Production build completed"
