#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/ops/check_services.sh
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

systemctl status qdgz300-sysctl.service --no-pager
systemctl status nic-optimization.service --no-pager
systemctl status cpu-performance.service --no-pager
systemctl status qdgz300-receiver.service --no-pager
systemctl status qdgz300-spool-mover.service --no-pager
