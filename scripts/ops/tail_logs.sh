#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/tail_logs.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"

journalctl -u "${SERVICE_NAME}.service" -f
