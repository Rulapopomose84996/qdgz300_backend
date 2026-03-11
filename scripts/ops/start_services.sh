#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/start_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

systemctl start qdgz300-sysctl.service nic-optimization.service cpu-performance.service qdgz300-receiver.service
