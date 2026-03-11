#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_common.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
UNIT_DIR="${BUILD_DIR}/tests/unit"

for bin in \
  common_config_snapshot_tests \
  common_error_codes_tests \
  common_event_types_tests \
  common_memory_pool_tests \
  common_metrics_tests \
  common_spsc_queue_tests \
  common_system_state_tests \
  common_types_tests
do
  "${UNIT_DIR}/${bin}"
done
