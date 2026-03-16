#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/stop_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"
MOVER_SERVICE_NAME="${MOVER_SERVICE_NAME:-qdgz300-spool-mover}"

systemctl stop "${MOVER_SERVICE_NAME}.service" "${SERVICE_NAME}.service"
