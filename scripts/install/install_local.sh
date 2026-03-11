#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash scripts/install/install_local.sh [build_dir]

This is a thin wrapper around deploy/install.sh.
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR_INPUT="${1:-${QDGZ300_BUILD_DIR:-${ROOT_DIR}/build_production}}"

exec bash "${ROOT_DIR}/deploy/install.sh" "${BUILD_DIR_INPUT}"
