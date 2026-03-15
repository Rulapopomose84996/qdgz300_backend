#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/prepare_wsl_cross_deps.sh

Environment:
  QDGZ300_OFFLINE_DEPS_DIR  Shared offline archive directory
  QDGZ300_DEPS_ROOT         Shared dependency cache root for WSL cross build
  QDGZ300_TOOLCHAIN_FILE    Toolchain file path
  QDGZ300_BUILD_TYPE        Dependency build type (default: Debug)
  QDGZ300_SKIP_GTEST        ON/OFF (default: OFF)
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_OFFLINE_DEPS_DIR="/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/archives"
DEFAULT_DEPS_ROOT="/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/build/wsl-aarch64"
OFFLINE_DEPS_DIR="${QDGZ300_OFFLINE_DEPS_DIR:-${DEFAULT_OFFLINE_DEPS_DIR}}"
DEPS_ROOT="${QDGZ300_DEPS_ROOT:-${DEFAULT_DEPS_ROOT}}"
DEPS_PREFIX="${DEPS_ROOT}/prefix"
DEPS_SRC_ROOT="${DEPS_ROOT}/src"
DEPS_BUILD_ROOT="${DEPS_ROOT}/build"
TOOLCHAIN_FILE="${QDGZ300_TOOLCHAIN_FILE:-${ROOT_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake}"
BUILD_TYPE="${QDGZ300_BUILD_TYPE:-Debug}"
SKIP_GTEST="${QDGZ300_SKIP_GTEST:-OFF}"

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

ensure_dir() {
  mkdir -p "$1"
}

extract_if_needed() {
  local archive_name="$1"
  local extracted_dir_name="$2"
  local archive_path="${OFFLINE_DEPS_DIR}/${archive_name}"
  local extracted_dir="${DEPS_SRC_ROOT}/${extracted_dir_name}"

  check_file "${archive_path}"
  if [[ -d "${extracted_dir}" ]]; then
    log "Reuse extracted source: ${extracted_dir_name}"
    return
  fi

  log "Extract ${archive_name}"
  tar -xf "${archive_path}" -C "${DEPS_SRC_ROOT}"
}

copy_spdlog_headers() {
  local spdlog_include_dir="${DEPS_SRC_ROOT}/spdlog-1.13.0/include"
  [[ -f "${spdlog_include_dir}/spdlog/spdlog.h" ]] || die "spdlog headers not found under ${spdlog_include_dir}"

  ensure_dir "${DEPS_PREFIX}/include"
  rm -rf "${DEPS_PREFIX}/include/spdlog"
  cp -r "${spdlog_include_dir}/spdlog" "${DEPS_PREFIX}/include/"
}

normalize_library() {
  local destination_name="$1"
  shift
  local destination_dir="${DEPS_PREFIX}/lib"
  local source_path=""
  local destination_path=""

  ensure_dir "${destination_dir}"

  for candidate in "$@"; do
    if [[ -f "${candidate}" ]]; then
      source_path="${candidate}"
      break
    fi
  done

  [[ -n "${source_path}" ]] || die "Failed to locate library for ${destination_name}"
  destination_path="${destination_dir}/${destination_name}"

  if [[ "$(realpath "${source_path}")" == "$(realpath -m "${destination_path}")" ]]; then
    return
  fi

  cp -f "${source_path}" "${destination_path}"
}

configure_and_install_yaml_cpp() {
  local source_dir="${DEPS_SRC_ROOT}/yaml-cpp-0.8.0"
  local build_dir="${DEPS_BUILD_ROOT}/yaml-cpp"

  ensure_dir "${build_dir}"
  log "Configure yaml-cpp"
  cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}" \
    -DYAML_BUILD_SHARED_LIBS=OFF \
    -DYAML_CPP_BUILD_TESTS=OFF \
    -DYAML_CPP_BUILD_TOOLS=OFF

  log "Build yaml-cpp"
  cmake --build "${build_dir}"

  log "Install yaml-cpp"
  cmake --install "${build_dir}"

  normalize_library "libyaml-cpp.a" \
    "${DEPS_PREFIX}/lib/libyaml-cpp.a" \
    "${DEPS_PREFIX}/lib/libyaml-cppd.a" \
    "${DEPS_PREFIX}/lib64/libyaml-cpp.a" \
    "${DEPS_PREFIX}/lib64/libyaml-cppd.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libyaml-cpp.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libyaml-cppd.a"
}

configure_and_install_gtest() {
  local source_dir="${DEPS_SRC_ROOT}/googletest-1.14.0"
  local build_dir="${DEPS_BUILD_ROOT}/googletest"

  ensure_dir "${build_dir}"
  log "Configure googletest"
  cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}" \
    -DBUILD_GMOCK=ON \
    -DINSTALL_GTEST=ON

  log "Build googletest"
  cmake --build "${build_dir}"

  log "Install googletest"
  cmake --install "${build_dir}"

  normalize_library "libgtest.a" \
    "${DEPS_PREFIX}/lib/libgtest.a" \
    "${DEPS_PREFIX}/lib/libgtestd.a" \
    "${DEPS_PREFIX}/lib64/libgtest.a" \
    "${DEPS_PREFIX}/lib64/libgtestd.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgtest.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgtestd.a"
  normalize_library "libgtest_main.a" \
    "${DEPS_PREFIX}/lib/libgtest_main.a" \
    "${DEPS_PREFIX}/lib/libgtest_maind.a" \
    "${DEPS_PREFIX}/lib64/libgtest_main.a" \
    "${DEPS_PREFIX}/lib64/libgtest_maind.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgtest_main.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgtest_maind.a"
  normalize_library "libgmock.a" \
    "${DEPS_PREFIX}/lib/libgmock.a" \
    "${DEPS_PREFIX}/lib/libgmockd.a" \
    "${DEPS_PREFIX}/lib64/libgmock.a" \
    "${DEPS_PREFIX}/lib64/libgmockd.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgmock.a" \
    "${DEPS_PREFIX}/lib/aarch64-linux-gnu/libgmockd.a"
}

log "Step 1/5: Validate WSL cross dependency environment"
[[ "$(uname -s)" == "Linux" ]] || die "This script must run in Linux/WSL."
check_command cmake
check_command ninja
check_command tar
check_command aarch64-linux-gnu-gcc
check_command aarch64-linux-gnu-g++
check_file "${TOOLCHAIN_FILE}"
[[ -d "${OFFLINE_DEPS_DIR}" ]] || die "Shared offline dependency directory not found: ${OFFLINE_DEPS_DIR}"

log "Step 2/5: Prepare shared cache directories"
ensure_dir "${DEPS_SRC_ROOT}"
ensure_dir "${DEPS_BUILD_ROOT}"
ensure_dir "${DEPS_PREFIX}"

log "Step 3/5: Extract required archives"
extract_if_needed "spdlog-1.13.0.tar.gz" "spdlog-1.13.0"
extract_if_needed "yaml-cpp-0.8.0.tar.gz" "yaml-cpp-0.8.0"
if [[ "${SKIP_GTEST}" != "ON" ]]; then
  extract_if_needed "googletest-1.14.0.tar.gz" "googletest-1.14.0"
fi

log "Step 4/5: Build shared dependency prefix"
copy_spdlog_headers
configure_and_install_yaml_cpp
if [[ "${SKIP_GTEST}" != "ON" ]]; then
  configure_and_install_gtest
fi

log "Step 5/5: Shared WSL cross dependency cache is ready"
printf 'archives    : %s\n' "${OFFLINE_DEPS_DIR}"
printf 'deps root   : %s\n' "${DEPS_ROOT}"
printf 'deps prefix : %s\n' "${DEPS_PREFIX}"
