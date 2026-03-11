#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/ops/tail_logs.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

journalctl -u qdgz300-receiver.service -f
