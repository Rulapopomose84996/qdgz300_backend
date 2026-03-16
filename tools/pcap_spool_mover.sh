#!/usr/bin/env bash

set -euo pipefail

CONFIG_FILE="${QDGZ300_RECEIVER_CONFIG:-/opt/qdgz300_backend/config/receiver.yaml}"
DEFAULT_SPOOL_DIR="${QDGZ300_DEFAULT_SPOOL_DIR:-/opt/qdgz300_backend/data/receiver_spool}"
DEFAULT_ARCHIVE_DIR="${QDGZ300_DEFAULT_ARCHIVE_DIR:-/data/qdgz300/receiver/archive}"
POLL_INTERVAL_SEC="${QDGZ300_SPOOL_POLL_INTERVAL_SEC:-5}"
LOG_PREFIX="[qdgz300-spool-mover]"

log() {
  printf '%s %s\n' "${LOG_PREFIX}" "$*"
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

read_config_value() {
  local key="$1"
  python3 - "$CONFIG_FILE" "$key" <<'PY'
import sys
from pathlib import Path

config_path = Path(sys.argv[1])
key = sys.argv[2]
if not config_path.exists():
    sys.exit(0)

in_capture = False
for raw_line in config_path.read_text(encoding="utf-8").splitlines():
    stripped = raw_line.strip()
    if not stripped or stripped.startswith("#"):
        continue
    if not raw_line.startswith(" ") and stripped.endswith(":"):
        in_capture = stripped[:-1] == "capture"
        continue
    if not in_capture:
        continue
    if raw_line.startswith("  ") and ":" in stripped:
        parsed_key, parsed_value = stripped.split(":", 1)
        if parsed_key == key:
            print(parsed_value.strip().strip('"').strip("'"))
            break
PY
}

move_one_file() {
  local source_file="$1"
  local archive_dir="$2"
  local file_name
  file_name="$(basename "$source_file")"
  local temp_target="${archive_dir}/.${file_name}.moving"
  local final_target="${archive_dir}/${file_name}"

  [[ -f "${source_file}" ]] || return 0

  if [[ -e "${final_target}" || -e "${temp_target}" ]]; then
    local stamp
    stamp="$(date '+%Y%m%d_%H%M%S')"
    temp_target="${archive_dir}/.${stamp}_${file_name}.moving"
    final_target="${archive_dir}/${stamp}_${file_name}"
  fi

  cp --preserve=mode,timestamps "${source_file}" "${temp_target}"
  mv -f "${temp_target}" "${final_target}"
  rm -f "${source_file}"
  log "archived ${source_file} -> ${final_target}"
}

main_loop() {
  local spool_dir="$1"
  local archive_dir="$2"

  install -d -m 0755 "${spool_dir}" "${archive_dir}"

  while true; do
    shopt -s nullglob
    local files=("${spool_dir}"/*.pcap)
    shopt -u nullglob

    for source_file in "${files[@]}"; do
      move_one_file "${source_file}" "${archive_dir}"
    done

    sleep "${POLL_INTERVAL_SEC}"
  done
}

main() {
  local spool_dir archive_dir
  spool_dir="$(trim "$(read_config_value "spool_dir")")"
  archive_dir="$(trim "$(read_config_value "archive_dir")")"

  [[ -n "${spool_dir}" ]] || spool_dir="${DEFAULT_SPOOL_DIR}"
  [[ -n "${archive_dir}" ]] || archive_dir="${DEFAULT_ARCHIVE_DIR}"

  log "start with config=${CONFIG_FILE} spool_dir=${spool_dir} archive_dir=${archive_dir} interval=${POLL_INTERVAL_SEC}s"

  cd /
  main_loop "${spool_dir}" "${archive_dir}"
}

main "$@"
