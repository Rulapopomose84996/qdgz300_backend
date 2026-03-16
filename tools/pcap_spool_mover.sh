#!/usr/bin/env bash

set -euo pipefail

CONFIG_FILE="${QDGZ300_RECEIVER_CONFIG:-/opt/qdgz300_backend/config/receiver.yaml}"
DEFAULT_SPOOL_DIR="${QDGZ300_DEFAULT_SPOOL_DIR:-/opt/qdgz300_backend/data/receiver_spool}"
DEFAULT_ARCHIVE_DIR="${QDGZ300_DEFAULT_ARCHIVE_DIR:-/data/qdgz300/receiver/archive}"
DEFAULT_ARCHIVE_MAX_FILES="${QDGZ300_DEFAULT_ARCHIVE_MAX_FILES:-256}"
DEFAULT_ARCHIVE_MAX_AGE_DAYS="${QDGZ300_DEFAULT_ARCHIVE_MAX_AGE_DAYS:-7}"
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

cleanup_by_age() {
  local archive_dir="$1"
  local max_age_days="$2"

  [[ "${max_age_days}" =~ ^[0-9]+$ ]] || return 0
  (( max_age_days > 0 )) || return 0

  while IFS= read -r expired_file; do
    [[ -n "${expired_file}" ]] || continue
    rm -f "${expired_file}"
    log "retention removed by age: ${expired_file}"
  done < <(find "${archive_dir}" -maxdepth 1 -type f -name '*.pcap' -mtime "+${max_age_days}" | sort)
}

cleanup_by_count() {
  local archive_dir="$1"
  local max_files="$2"

  [[ "${max_files}" =~ ^[0-9]+$ ]] || return 0
  (( max_files > 0 )) || return 0

  mapfile -t archive_files < <(find "${archive_dir}" -maxdepth 1 -type f -name '*.pcap' -printf '%T@ %p\n' | sort -n | awk '{ $1=""; sub(/^ /, ""); print }')
  local total_files="${#archive_files[@]}"
  if (( total_files <= max_files )); then
    return 0
  fi

  local remove_count=$((total_files - max_files))
  local index
  for ((index = 0; index < remove_count; ++index)); do
    rm -f "${archive_files[index]}"
    log "retention removed by count: ${archive_files[index]}"
  done
}

apply_archive_retention() {
  local archive_dir="$1"
  local max_files="$2"
  local max_age_days="$3"

  cleanup_by_age "${archive_dir}" "${max_age_days}"
  cleanup_by_count "${archive_dir}" "${max_files}"
}

main_loop() {
  local spool_dir="$1"
  local archive_dir="$2"
  local archive_max_files="$3"
  local archive_max_age_days="$4"

  install -d -m 0755 "${spool_dir}" "${archive_dir}"

  while true; do
    shopt -s nullglob
    local files=("${spool_dir}"/*.pcap)
    shopt -u nullglob

    for source_file in "${files[@]}"; do
      move_one_file "${source_file}" "${archive_dir}"
    done

    apply_archive_retention "${archive_dir}" "${archive_max_files}" "${archive_max_age_days}"

    sleep "${POLL_INTERVAL_SEC}"
  done
}

main() {
  local spool_dir archive_dir archive_max_files archive_max_age_days
  spool_dir="$(trim "$(read_config_value "spool_dir")")"
  archive_dir="$(trim "$(read_config_value "archive_dir")")"
  archive_max_files="$(trim "$(read_config_value "archive_max_files")")"
  archive_max_age_days="$(trim "$(read_config_value "archive_max_age_days")")"

  [[ -n "${spool_dir}" ]] || spool_dir="${DEFAULT_SPOOL_DIR}"
  [[ -n "${archive_dir}" ]] || archive_dir="${DEFAULT_ARCHIVE_DIR}"
  [[ -n "${archive_max_files}" ]] || archive_max_files="${DEFAULT_ARCHIVE_MAX_FILES}"
  [[ -n "${archive_max_age_days}" ]] || archive_max_age_days="${DEFAULT_ARCHIVE_MAX_AGE_DAYS}"

  log "start with config=${CONFIG_FILE} spool_dir=${spool_dir} archive_dir=${archive_dir} archive_max_files=${archive_max_files} archive_max_age_days=${archive_max_age_days} interval=${POLL_INTERVAL_SEC}s"

  cd /
  main_loop "${spool_dir}" "${archive_dir}" "${archive_max_files}" "${archive_max_age_days}"
}

main "$@"
