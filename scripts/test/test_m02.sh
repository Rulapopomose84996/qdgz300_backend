#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_m02.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
ctest --test-dir "${BUILD_DIR}/tests/unit" --output-on-failure -L "m02"
