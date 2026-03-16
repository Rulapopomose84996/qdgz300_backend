#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/ops/check_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"
MOVER_SERVICE_NAME="${MOVER_SERVICE_NAME:-qdgz300-spool-mover}"

systemctl status qdgz300-sysctl.service --no-pager
systemctl status nic-optimization.service --no-pager
systemctl status cpu-performance.service --no-pager
systemctl status "${SERVICE_NAME}.service" --no-pager
systemctl status "${MOVER_SERVICE_NAME}.service" --no-pager
