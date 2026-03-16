#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo bash deploy/install.sh [build_dir]

Environment:
  INSTALL_ROOT   Install root (default: /opt/qdgz300_backend)
  SERVICE_NAME   Service unit name without suffix (default: qdgz300-receiver)
  SERVICE_USER   Service user (default: qdgz300)
  SERVICE_GROUP  Service group (default: qdgz300)
EOF
}

[[ "${1:-}" == "--help" ]] && { usage; exit 0; }

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR_INPUT="${1:-${ROOT_DIR}/build_production}"
BUILD_DIR="$(cd "${BUILD_DIR_INPUT}" 2>/dev/null && pwd || true)"
INSTALL_ROOT="${INSTALL_ROOT:-/opt/qdgz300_backend}"
SERVICE_NAME="${SERVICE_NAME:-qdgz300-receiver}"
SERVICE_USER="${SERVICE_USER:-qdgz300}"
SERVICE_GROUP="${SERVICE_GROUP:-qdgz300}"
SYSTEMD_DIR="/etc/systemd/system"
BIN_DIR="${INSTALL_ROOT}/bin"
CONFIG_DIR="${INSTALL_ROOT}/config"
DATA_DIR="${INSTALL_ROOT}/data"
LOG_DIR="${INSTALL_ROOT}/logs"
SCRIPT_DIR="${INSTALL_ROOT}/scripts"
RELEASES_DIR="${INSTALL_ROOT}/releases"

log() {
  printf '[install] %s\n' "$*"
}

warn() {
  printf '[install][warn] %s\n' "$*" >&2
}

die() {
  printf '[install][error] %s\n' "$*" >&2
  exit 1
}

require_root() {
  [[ "${EUID}" -eq 0 ]] || die "Please run as root: sudo bash deploy/install.sh <build_dir>"
}

ensure_service_account() {
  if ! getent group "${SERVICE_GROUP}" >/dev/null 2>&1; then
    log "Creating service group ${SERVICE_GROUP}"
    groupadd --system "${SERVICE_GROUP}"
  fi

  if ! id -u "${SERVICE_USER}" >/dev/null 2>&1; then
    log "Creating service user ${SERVICE_USER}"
    useradd --system --gid "${SERVICE_GROUP}" \
      --home-dir "${INSTALL_ROOT}" \
      --shell /sbin/nologin \
      "${SERVICE_USER}"
  fi
}

install_service_config() {
  if [[ ! -f "${CONFIG_DIR}/receiver.yaml" ]]; then
    install -m 0640 "${ROOT_DIR}/deploy/receiver_config_example.yaml" "${CONFIG_DIR}/receiver.yaml"
    log "Installed default runtime config: ${CONFIG_DIR}/receiver.yaml"
  else
    install -m 0644 "${ROOT_DIR}/deploy/receiver_config_example.yaml" "${CONFIG_DIR}/receiver.yaml.example"
    log "Runtime config exists, refreshed example: ${CONFIG_DIR}/receiver.yaml.example"
  fi
}

backup_existing_install() {
  if [[ ! -d "${INSTALL_ROOT}" ]]; then
    return 0
  fi

  if [[ ! -f "${BIN_DIR}/receiver_app" ]]; then
    return 0
  fi

  local stamp
  stamp="$(date '+%Y%m%d_%H%M%S')"
  local backup_dir="${RELEASES_DIR}/${stamp}"
  install -d -m 0755 "${backup_dir}/bin" "${backup_dir}/config"
  cp -a "${BIN_DIR}/." "${backup_dir}/bin/" 2>/dev/null || true
  cp -a "${CONFIG_DIR}/." "${backup_dir}/config/" 2>/dev/null || true
  log "Backed up previous install to ${backup_dir}"
}

main() {
  require_root
  [[ -n "${BUILD_DIR}" && -d "${BUILD_DIR}" ]] || die "Build directory not found: ${BUILD_DIR_INPUT}"
  [[ -f "${BUILD_DIR}/receiver_app" ]] || die "Expected binary not found: ${BUILD_DIR}/receiver_app"

  ensure_service_account

  log "Creating install layout under ${INSTALL_ROOT}"
  install -d -m 0755 "${BIN_DIR}" "${CONFIG_DIR}" "${DATA_DIR}" "${LOG_DIR}" "${SCRIPT_DIR}" "${RELEASES_DIR}"
  backup_existing_install

  log "Installing application binaries"
  install -m 0755 "${BUILD_DIR}/receiver_app" "${BIN_DIR}/receiver_app"
  if [[ -f "${BUILD_DIR}/fpga_emulator" ]]; then
    install -m 0755 "${BUILD_DIR}/fpga_emulator" "${BIN_DIR}/fpga_emulator"
  fi

log "Installing helper scripts"
install -m 0755 "${ROOT_DIR}/tools/nic_tuning.sh" "${SCRIPT_DIR}/nic_tuning.sh"
install -m 0755 "${ROOT_DIR}/tools/cpu_performance.sh" "${SCRIPT_DIR}/cpu_performance.sh"
install -m 0755 "${ROOT_DIR}/tools/pcap_spool_mover.sh" "${SCRIPT_DIR}/pcap_spool_mover.sh"
install -m 0755 "${ROOT_DIR}/tools/nic_tuning.sh" "/usr/local/bin/nic-tuning.sh"
install -m 0755 "${ROOT_DIR}/tools/cpu_performance.sh" "/usr/local/bin/cpu-performance.sh"
install -m 0755 "${ROOT_DIR}/tools/pcap_spool_mover.sh" "/usr/local/bin/qdgz300-spool-mover.sh"

  log "Installing runtime config templates"
  install_service_config

  log "Installing systemd and sysctl assets"
  install -m 0644 "${ROOT_DIR}/deploy/systemd/qdgz300-receiver.service" "${SYSTEMD_DIR}/${SERVICE_NAME}.service"
  install -m 0644 "${ROOT_DIR}/deploy/systemd/qdgz300-spool-mover.service" "${SYSTEMD_DIR}/qdgz300-spool-mover.service"
  install -m 0644 "${ROOT_DIR}/deploy/systemd/qdgz300-sysctl.service" "${SYSTEMD_DIR}/qdgz300-sysctl.service"
  install -m 0644 "${ROOT_DIR}/deploy/systemd/nic-optimization.service" "${SYSTEMD_DIR}/nic-optimization.service"
  install -m 0644 "${ROOT_DIR}/deploy/systemd/cpu-performance.service" "${SYSTEMD_DIR}/cpu-performance.service"
  install -m 0644 "${ROOT_DIR}/deploy/sysctl/90-qdgz300.conf" "/etc/sysctl.d/90-qdgz300.conf"

  log "Setting ownership"
  chown -R "${SERVICE_USER}:${SERVICE_GROUP}" "${INSTALL_ROOT}"
  chmod 0750 "${DATA_DIR}" "${LOG_DIR}"
  chmod 0640 "${CONFIG_DIR}/receiver.yaml" 2>/dev/null || true

  log "Reloading systemd"
  systemctl daemon-reload

  cat <<EOF

Install completed.

Application root:
  ${INSTALL_ROOT}

Service file:
  ${SYSTEMD_DIR}/${SERVICE_NAME}.service

Next steps:
  1. Edit runtime config: ${CONFIG_DIR}/receiver.yaml
  2. Enable sysctl/tuning services:
     sudo systemctl enable qdgz300-sysctl.service nic-optimization.service cpu-performance.service qdgz300-spool-mover.service
  3. Enable receiver service:
     sudo systemctl enable ${SERVICE_NAME}.service
  4. Start services:
     sudo systemctl start qdgz300-sysctl.service nic-optimization.service cpu-performance.service
     sudo systemctl start ${SERVICE_NAME}.service qdgz300-spool-mover.service
  5. Check status:
     sudo systemctl status ${SERVICE_NAME}.service
  6. Roll back manually if needed:
     ls -1 ${RELEASES_DIR}
     cp -a ${RELEASES_DIR}/<timestamp>/bin/. ${BIN_DIR}/

EOF
}

main "$@"
