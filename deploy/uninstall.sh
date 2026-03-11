#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash deploy/uninstall.sh

Environment:
  INSTALL_ROOT   Install root to remove (default: /opt/qdgz300_backend)
  SERVICE_NAME   Service unit name without suffix (default: qdgz300-receiver)
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

INSTALL_ROOT="${INSTALL_ROOT:-/opt/qdgz300_backend}"
SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"
SYSTEMD_DIR="/etc/systemd/system"

log() {
  printf '[uninstall] %s\n' "$*"
}

die() {
  printf '[uninstall][error] %s\n' "$*" >&2
  exit 1
}

[[ "${EUID}" -eq 0 ]] || die "Please run as root: sudo bash deploy/uninstall.sh"

systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true
systemctl disable "${SERVICE_NAME}.service" 2>/dev/null || true
rm -f "${SYSTEMD_DIR}/${SERVICE_NAME}.service"
systemctl daemon-reload

if [[ -d "${INSTALL_ROOT}" ]]; then
  rm -rf "${INSTALL_ROOT}"
  log "Removed install root: ${INSTALL_ROOT}"
else
  log "Install root not found, skip: ${INSTALL_ROOT}"
fi
