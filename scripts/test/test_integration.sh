#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_integration.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR_INPUT="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
TEST_REGEX="${QDGZ300_TEST_REGEX:-}"

[[ -d "${BUILD_DIR_INPUT}/tests/integration" ]] || { echo "[ERROR] Integration test directory not found: ${BUILD_DIR_INPUT}/tests/integration" >&2; exit 1; }

ctest_cmd=(ctest --test-dir "${BUILD_DIR_INPUT}/tests/integration" --output-on-failure)
if [[ -n "${TEST_REGEX}" ]]; then
  ctest_cmd+=(-R "${TEST_REGEX}")
fi
"${ctest_cmd[@]}"
