#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_integration_m02.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
INT_DIR="${BUILD_DIR}/tests/integration"

for bin in \
  m02_dispatcher_integration_tests \
  integration_m01_m02_e2e_tests
do
  "${INT_DIR}/${bin}"
done
