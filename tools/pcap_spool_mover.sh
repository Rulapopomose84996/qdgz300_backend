#!/usr/bin/env bash

set -euo pipefail

CONFIG_FILE="${QDGZ300_RECEIVER_CONFIG:-/opt/qdgz300_backend/config/receiver.yaml}"
DEFAULT_SPOOL_DIR="${QDGZ300_DEFAULT_SPOOL_DIR:-/opt/qdgz300_backend/data/receiver_spool}"
DEFAULT_ARCHIVE_DIR="${QDGZ300_DEFAULT_ARCHIVE_DIR:-/data/qdgz300/receiver/archive}"
DEFAULT_ARCHIVE_MAX_FILES="${QDGZ300_DEFAULT_ARCHIVE_MAX_FILES:-256}"
DEFAULT_ARCHIVE_MAX_AGE_DAYS="${QDGZ300_DEFAULT_ARCHIVE_MAX_AGE_DAYS:-7}"
DEFAULT_METRICS_DIR="${QDGZ300_DEFAULT_METRICS_DIR:-/opt/qdgz300_backend/data/metrics}"
DEFAULT_SPOOL_LOW_WATERMARK_PCT="${QDGZ300_DEFAULT_SPOOL_LOW_WATERMARK_PCT:-10}"
DEFAULT_ARCHIVE_LOW_WATERMARK_PCT="${QDGZ300_DEFAULT_ARCHIVE_LOW_WATERMARK_PCT:-10}"
POLL_INTERVAL_SEC="${QDGZ300_SPOOL_POLL_INTERVAL_SEC:-5}"
LOG_PREFIX="[qdgz300-spool-mover]"

archived_total=0
archive_failures_total=0
retention_removed_by_age_total=0
retention_removed_by_count_total=0
last_success_ts=0
last_failure_ts=0
last_spool_files=0
last_archive_files=0
last_spool_total_bytes=0
last_spool_available_bytes=0
last_archive_total_bytes=0
last_archive_available_bytes=0
spool_low_watermark_active=0
archive_low_watermark_active=0

log() {
  printf '%s %s\n' "${LOG_PREFIX}" "$*"
}

warn() {
  printf '%s WARN %s\n' "${LOG_PREFIX}" "$*" >&2
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

record_failure() {
  archive_failures_total=$((archive_failures_total + 1))
  last_failure_ts="$(date +%s)"
  warn "$*"
}

check_disk_watermark() {
  local label="$1"
  local path="$2"
  local threshold_pct="$3"
  local total_bytes_ref_name="$4"
  local avail_bytes_ref_name="$5"
  local active_ref_name="$6"

  local df_line
  df_line="$(df -P -B1 "${path}" 2>/dev/null | awk 'NR==2 {print $2 " " $4}')"
  if [[ -z "${df_line}" ]]; then
    record_failure "failed to read disk usage for ${label} path ${path}"
    return 1
  fi

  local total_bytes available_bytes
  read -r total_bytes available_bytes <<< "${df_line}"
  printf -v "${total_bytes_ref_name}" '%s' "${total_bytes}"
  printf -v "${avail_bytes_ref_name}" '%s' "${available_bytes}"

  if [[ ! "${threshold_pct}" =~ ^[0-9]+$ ]] || (( threshold_pct <= 0 )) || (( threshold_pct >= 100 )); then
    return 0
  fi

  local active_value=0
  if (( total_bytes > 0 )); then
    local threshold_bytes=$(( total_bytes * threshold_pct / 100 ))
    if (( available_bytes <= threshold_bytes )); then
      active_value=1
    fi
  fi

  local previous_value
  previous_value="${!active_ref_name}"
  printf -v "${active_ref_name}" '%s' "${active_value}"

  if (( active_value == 1 && previous_value == 0 )); then
    warn "${label} disk low watermark triggered: available_bytes=${available_bytes} total_bytes=${total_bytes} threshold_pct=${threshold_pct} path=${path}"
  elif (( active_value == 0 && previous_value == 1 )); then
    log "${label} disk low watermark recovered: available_bytes=${available_bytes} total_bytes=${total_bytes} threshold_pct=${threshold_pct} path=${path}"
  fi

  return 0
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

write_metrics() {
  local metrics_dir="$1"
  local spool_dir="$2"
  local archive_dir="$3"
  local metrics_file="${metrics_dir}/qdgz300_spool_mover.prom"
  local temp_file="${metrics_file}.tmp"

  install -d -m 0755 "${metrics_dir}"
  last_spool_files=$(find "${spool_dir}" -maxdepth 1 -type f -name '*.pcap*' | wc -l | tr -d ' ')
  last_archive_files=$(find "${archive_dir}" -maxdepth 1 -type f -name '*.pcap' | wc -l | tr -d ' ')

  cat > "${temp_file}" <<EOF
# HELP qdgz300_spool_mover_archived_total Total sealed pcap files archived from spool to archive.
# TYPE qdgz300_spool_mover_archived_total counter
qdgz300_spool_mover_archived_total ${archived_total}
# HELP qdgz300_spool_mover_archive_failures_total Total mover failures.
# TYPE qdgz300_spool_mover_archive_failures_total counter
qdgz300_spool_mover_archive_failures_total ${archive_failures_total}
# HELP qdgz300_spool_mover_retention_removed_by_age_total Total archived files removed by age retention.
# TYPE qdgz300_spool_mover_retention_removed_by_age_total counter
qdgz300_spool_mover_retention_removed_by_age_total ${retention_removed_by_age_total}
# HELP qdgz300_spool_mover_retention_removed_by_count_total Total archived files removed by count retention.
# TYPE qdgz300_spool_mover_retention_removed_by_count_total counter
qdgz300_spool_mover_retention_removed_by_count_total ${retention_removed_by_count_total}
# HELP qdgz300_spool_mover_last_success_timestamp_seconds Unix timestamp of last successful archive move.
# TYPE qdgz300_spool_mover_last_success_timestamp_seconds gauge
qdgz300_spool_mover_last_success_timestamp_seconds ${last_success_ts}
# HELP qdgz300_spool_mover_last_failure_timestamp_seconds Unix timestamp of last mover failure.
# TYPE qdgz300_spool_mover_last_failure_timestamp_seconds gauge
qdgz300_spool_mover_last_failure_timestamp_seconds ${last_failure_ts}
# HELP qdgz300_spool_mover_spool_files Current number of files in spool directory.
# TYPE qdgz300_spool_mover_spool_files gauge
qdgz300_spool_mover_spool_files ${last_spool_files}
# HELP qdgz300_spool_mover_archive_files Current number of files in archive directory.
# TYPE qdgz300_spool_mover_archive_files gauge
qdgz300_spool_mover_archive_files ${last_archive_files}
# HELP qdgz300_spool_mover_spool_total_bytes Total bytes of filesystem containing spool directory.
# TYPE qdgz300_spool_mover_spool_total_bytes gauge
qdgz300_spool_mover_spool_total_bytes ${last_spool_total_bytes}
# HELP qdgz300_spool_mover_spool_available_bytes Available bytes of filesystem containing spool directory.
# TYPE qdgz300_spool_mover_spool_available_bytes gauge
qdgz300_spool_mover_spool_available_bytes ${last_spool_available_bytes}
# HELP qdgz300_spool_mover_archive_total_bytes Total bytes of filesystem containing archive directory.
# TYPE qdgz300_spool_mover_archive_total_bytes gauge
qdgz300_spool_mover_archive_total_bytes ${last_archive_total_bytes}
# HELP qdgz300_spool_mover_archive_available_bytes Available bytes of filesystem containing archive directory.
# TYPE qdgz300_spool_mover_archive_available_bytes gauge
qdgz300_spool_mover_archive_available_bytes ${last_archive_available_bytes}
# HELP qdgz300_spool_mover_spool_low_watermark_active Whether spool disk is below configured low watermark.
# TYPE qdgz300_spool_mover_spool_low_watermark_active gauge
qdgz300_spool_mover_spool_low_watermark_active ${spool_low_watermark_active}
# HELP qdgz300_spool_mover_archive_low_watermark_active Whether archive disk is below configured low watermark.
# TYPE qdgz300_spool_mover_archive_low_watermark_active gauge
qdgz300_spool_mover_archive_low_watermark_active ${archive_low_watermark_active}
EOF

  mv -f "${temp_file}" "${metrics_file}"
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

  if ! cp --preserve=mode,timestamps "${source_file}" "${temp_target}"; then
    rm -f "${temp_target}"
    record_failure "failed to copy ${source_file} to ${temp_target}"
    return 1
  fi

  if ! mv -f "${temp_target}" "${final_target}"; then
    rm -f "${temp_target}"
    record_failure "failed to seal archive target ${final_target}"
    return 1
  fi

  if ! rm -f "${source_file}"; then
    record_failure "archived ${source_file} but failed to remove source file"
    return 1
  fi

  archived_total=$((archived_total + 1))
  last_success_ts="$(date +%s)"
  log "archived ${source_file} -> ${final_target}"
  return 0
}

cleanup_by_age() {
  local archive_dir="$1"
  local max_age_days="$2"

  [[ "${max_age_days}" =~ ^[0-9]+$ ]] || return 0
  (( max_age_days > 0 )) || return 0

  while IFS= read -r expired_file; do
    [[ -n "${expired_file}" ]] || continue
    if ! rm -f "${expired_file}"; then
      record_failure "failed to remove expired archive file ${expired_file}"
      continue
    fi
    retention_removed_by_age_total=$((retention_removed_by_age_total + 1))
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
    if ! rm -f "${archive_files[index]}"; then
      record_failure "failed to remove archive file by count ${archive_files[index]}"
      continue
    fi
    retention_removed_by_count_total=$((retention_removed_by_count_total + 1))
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
  local metrics_dir="$5"
  local spool_low_watermark_pct="$6"
  local archive_low_watermark_pct="$7"

  if ! install -d -m 0755 "${spool_dir}" "${archive_dir}" "${metrics_dir}"; then
    record_failure "failed to prepare mover directories"
    return 1
  fi

  while true; do
    shopt -s nullglob
    local files=("${spool_dir}"/*.pcap)
    shopt -u nullglob

    for source_file in "${files[@]}"; do
      move_one_file "${source_file}" "${archive_dir}"
    done

    apply_archive_retention "${archive_dir}" "${archive_max_files}" "${archive_max_age_days}"
    check_disk_watermark "spool" "${spool_dir}" "${spool_low_watermark_pct}" "last_spool_total_bytes" "last_spool_available_bytes" "spool_low_watermark_active" || true
    check_disk_watermark "archive" "${archive_dir}" "${archive_low_watermark_pct}" "last_archive_total_bytes" "last_archive_available_bytes" "archive_low_watermark_active" || true
    write_metrics "${metrics_dir}" "${spool_dir}" "${archive_dir}"

    sleep "${POLL_INTERVAL_SEC}"
  done
}

main() {
  local spool_dir archive_dir archive_max_files archive_max_age_days metrics_dir
  local spool_low_watermark_pct archive_low_watermark_pct
  spool_dir="$(trim "$(read_config_value "spool_dir")")"
  archive_dir="$(trim "$(read_config_value "archive_dir")")"
  spool_low_watermark_pct="$(trim "$(read_config_value "spool_low_watermark_pct")")"
  archive_low_watermark_pct="$(trim "$(read_config_value "archive_low_watermark_pct")")"
  archive_max_files="$(trim "$(read_config_value "archive_max_files")")"
  archive_max_age_days="$(trim "$(read_config_value "archive_max_age_days")")"
  metrics_dir="$(trim "$(read_config_value "metrics_dir")")"

  [[ -n "${spool_dir}" ]] || spool_dir="${DEFAULT_SPOOL_DIR}"
  [[ -n "${archive_dir}" ]] || archive_dir="${DEFAULT_ARCHIVE_DIR}"
  [[ -n "${spool_low_watermark_pct}" ]] || spool_low_watermark_pct="${DEFAULT_SPOOL_LOW_WATERMARK_PCT}"
  [[ -n "${archive_low_watermark_pct}" ]] || archive_low_watermark_pct="${DEFAULT_ARCHIVE_LOW_WATERMARK_PCT}"
  [[ -n "${archive_max_files}" ]] || archive_max_files="${DEFAULT_ARCHIVE_MAX_FILES}"
  [[ -n "${archive_max_age_days}" ]] || archive_max_age_days="${DEFAULT_ARCHIVE_MAX_AGE_DAYS}"
  [[ -n "${metrics_dir}" ]] || metrics_dir="${DEFAULT_METRICS_DIR}"

  log "start with config=${CONFIG_FILE} spool_dir=${spool_dir} archive_dir=${archive_dir} spool_low_watermark_pct=${spool_low_watermark_pct} archive_low_watermark_pct=${archive_low_watermark_pct} archive_max_files=${archive_max_files} archive_max_age_days=${archive_max_age_days} metrics_dir=${metrics_dir} interval=${POLL_INTERVAL_SEC}s"

  cd /
  main_loop "${spool_dir}" "${archive_dir}" "${archive_max_files}" "${archive_max_age_days}" "${metrics_dir}" "${spool_low_watermark_pct}" "${archive_low_watermark_pct}"
}

main "$@"
