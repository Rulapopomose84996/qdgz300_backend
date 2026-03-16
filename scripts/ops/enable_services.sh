#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/enable_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

systemctl enable qdgz300-sysctl.service nic-optimization.service cpu-performance.service qdgz300-receiver.service qdgz300-spool-mover.service
