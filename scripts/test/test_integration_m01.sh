#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_integration_m01.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
INT_DIR="${BUILD_DIR}/tests/integration"

for bin in \
  integration_rawblock_delivery_tests \
  integration_tests_fpga
do
  "${INT_DIR}/${bin}"
done
