#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_m01.sh [build_dir]
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"
UNIT_DIR="${BUILD_DIR}/tests/unit"

for bin in \
  config_manager_tests \
  metrics_tests \
  packet_pool_tests \
  pcap_writer_tests \
  rawblock_adapter_tests \
  reassembler_tests \
  reorderer_tests \
  spsc_queue_tests \
  stub_consumer_tests \
  udp_receiver_tests
do
  "${UNIT_DIR}/${bin}"
done
