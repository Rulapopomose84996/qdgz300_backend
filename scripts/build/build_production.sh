#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/build/build_production.sh

Environment:
  QDGZ300_BUILD_DIR       Build output directory (default: build_production)
  QDGZ300_OFFLINE_DEPS_DIR Shared offline archive directory
  QDGZ300_DEPS_ROOT       Shared dependency cache root
  QDGZ300_BUILD_TYPE      CMake build type (default: Release)
  QDGZ300_BUILD_TESTING   ON/OFF (default: ON)
  QDGZ300_RUN_TESTS       ON/OFF (default: ON)
  QDGZ300_BUILD_SIMULATOR ON/OFF (default: OFF)
  QDGZ300_ENABLE_GPU      ON/OFF (default: ON)
  QDGZ300_ENABLE_PROTOBUF ON/OFF (default: OFF)
  QDGZ300_BUILD_TARGET    Optional explicit build target
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}"
OFFLINE_DEPS_DIR="${QDGZ300_OFFLINE_DEPS_DIR:-/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/archives}"
DEPS_ROOT="${QDGZ300_DEPS_ROOT:-/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64}"
DEPS_PREFIX="${QDGZ300_DEPS_PREFIX:-${DEPS_ROOT}/prefix}"
BUILD_TYPE="${QDGZ300_BUILD_TYPE:-Release}"
BUILD_TESTING="${QDGZ300_BUILD_TESTING:-ON}"
RUN_TESTS="${QDGZ300_RUN_TESTS:-ON}"
BUILD_SIMULATOR="${QDGZ300_BUILD_SIMULATOR:-OFF}"
ENABLE_GPU="${QDGZ300_ENABLE_GPU:-ON}"
ENABLE_PROTOBUF="${QDGZ300_ENABLE_PROTOBUF:-OFF}"
BUILD_TARGET="${QDGZ300_BUILD_TARGET:-}"
TEST_REGEX="${QDGZ300_TEST_REGEX:-}"
COREX_ROOT="${QDGZ300_COREX_ROOT:-/usr/local/corex}"
COREX_CLANGXX="${QDGZ300_COREX_CLANGXX:-${COREX_ROOT}/bin/clang++}"
COREX_CLANG="${QDGZ300_COREX_CLANG:-${COREX_ROOT}/bin/clang}"

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

reset_build_dir_if_cache_mismatched() {
  local cache_file="${BUILD_DIR}/CMakeCache.txt"
  local cached_source_dir=""

  [[ -f "${cache_file}" ]] || return

  cached_source_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache_file}" | head -n 1)"
  [[ -n "${cached_source_dir}" ]] || return

  if [[ "${cached_source_dir}" != "${ROOT_DIR}" ]]; then
    log "Detected stale CMake cache from ${cached_source_dir}; removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
  fi
}

log "Step 1/5: Validate production build environment"
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

log "Step 2/5: Prepare shared dependency cache"
prepare_env=(
  "QDGZ300_OFFLINE_DEPS_DIR=${OFFLINE_DEPS_DIR}"
  "QDGZ300_DEPS_ROOT=${DEPS_ROOT}"
  "QDGZ300_BUILD_TYPE=${BUILD_TYPE}"
)

if [[ "${ENABLE_GPU}" == "ON" ]]; then
  prepare_env+=(
    "QDGZ300_C_COMPILER=${COREX_CLANG}"
    "QDGZ300_CXX_COMPILER=${COREX_CLANGXX}"
  )
fi

env "${prepare_env[@]}" bash "${ROOT_DIR}/scripts/prepare_native_deps.sh"

log "Step 3/5: Configure native build"
reset_build_dir_if_cache_mismatched
cmake_args=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -G Ninja
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DBUILD_TESTING="${BUILD_TESTING}"
  -DBUILD_SIMULATOR="${BUILD_SIMULATOR}"
  -DENABLE_GPU="${ENABLE_GPU}"
  -DENABLE_PROTOBUF="${ENABLE_PROTOBUF}"
  -DQDGZ300_OFFLINE_DEPS_DIR="${OFFLINE_DEPS_DIR}"
  -DQDGZ300_DEPS_ROOT="${DEPS_ROOT}"
  -DQDGZ300_DEPS_PREFIX="${DEPS_PREFIX}"
)

if [[ "${ENABLE_GPU}" == "ON" ]]; then
  cmake_args+=(
    -DCMAKE_C_COMPILER="${COREX_CLANG}"
    -DCMAKE_CXX_COMPILER="${COREX_CLANGXX}"
  )
fi

cmake "${cmake_args[@]}"

log "Step 4/5: Build"
if [[ -n "${BUILD_TARGET}" ]]; then
  cmake --build "${BUILD_DIR}" --target "${BUILD_TARGET}"
else
  cmake --build "${BUILD_DIR}"
fi

log "Step 5/5: Run unit tests"
if [[ "${RUN_TESTS}" == "ON" && "${BUILD_TESTING}" == "ON" ]]; then
  QDGZ300_TEST_REGEX="${TEST_REGEX}" bash "${ROOT_DIR}/scripts/test/test_unit.sh" "${BUILD_DIR}"
else
  log "Tests skipped; set QDGZ300_RUN_TESTS=ON and QDGZ300_BUILD_TESTING=ON to execute unit tests"
fi
