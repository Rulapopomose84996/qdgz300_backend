#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/stop_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

systemctl stop qdgz300-spool-mover.service qdgz300-receiver.service
