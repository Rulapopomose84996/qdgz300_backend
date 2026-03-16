#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/enable_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"
MOVER_SERVICE_NAME="${MOVER_SERVICE_NAME:-qdgz300-spool-mover}"

systemctl enable qdgz300-sysctl.service nic-optimization.service cpu-performance.service "${SERVICE_NAME}.service" "${MOVER_SERVICE_NAME}.service"
