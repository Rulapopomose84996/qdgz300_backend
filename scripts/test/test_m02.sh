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
UNIT_DIR="${BUILD_DIR}/tests/unit"

for bin in \
  gpu_dispatcher_tests \
  gpu_pipeline_inflight_tests \
  m02_resource_pool_tests \
  m02_signal_proc_tests
do
  "${UNIT_DIR}/${bin}"
done
