#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/test/test_spool_soak.sh [options]

Options:
  --receiver <path>        receiver_app path (default: /opt/qdgz300_backend/bin/receiver_app)
  --mover <path>           pcap_spool_mover.sh path (default: /usr/local/bin/qdgz300-spool-mover.sh)
  --port <port>            UDP listen port (default: 19999)
  --phase-seconds <sec>    Per-phase send duration (default: 20)
  --pps <pps>              Send rate in packets per second (default: 50)
  --archive-max-files <n>  Archive retention max files (default: 3)
  --poll-sec <sec>         Mover poll interval seconds (default: 1)
  --workdir <path>         Existing workdir to reuse
EOF
}

RECEIVER_BIN="/opt/qdgz300_backend/bin/receiver_app"
MOVER_BIN="/usr/local/bin/qdgz300-spool-mover.sh"
LISTEN_PORT=19999
PHASE_SECONDS=20
SEND_PPS=50
ARCHIVE_MAX_FILES=3
POLL_SEC=1
WORKDIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --receiver) RECEIVER_BIN="$2"; shift 2 ;;
    --mover) MOVER_BIN="$2"; shift 2 ;;
    --port) LISTEN_PORT="$2"; shift 2 ;;
    --phase-seconds) PHASE_SECONDS="$2"; shift 2 ;;
    --pps) SEND_PPS="$2"; shift 2 ;;
    --archive-max-files) ARCHIVE_MAX_FILES="$2"; shift 2 ;;
    --poll-sec) POLL_SEC="$2"; shift 2 ;;
    --workdir) WORKDIR="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) echo "[ERROR] Unknown option: $1" >&2; exit 1 ;;
  esac
done

[[ -x "${RECEIVER_BIN}" ]] || { echo "[ERROR] receiver_app not executable: ${RECEIVER_BIN}" >&2; exit 1; }
[[ -x "${MOVER_BIN}" ]] || { echo "[ERROR] mover not executable: ${MOVER_BIN}" >&2; exit 1; }

if [[ -z "${WORKDIR}" ]]; then
  WORKDIR="$(mktemp -d /tmp/qdgz300_spool_soak.XXXXXX)"
fi

SPOOL_DIR="${WORKDIR}/spool"
ARCHIVE_DIR="${WORKDIR}/archive"
METRICS_DIR="${WORKDIR}/metrics"
CONFIG_FILE="${WORKDIR}/receiver_soak.yaml"
RECEIVER_LOG="${WORKDIR}/receiver.log"
MOVER_LOG="${WORKDIR}/mover.log"
SUMMARY_FILE="${WORKDIR}/summary.txt"
RECEIVER_PID=""
MOVER_PID=""

mkdir -p "${SPOOL_DIR}" "${ARCHIVE_DIR}" "${METRICS_DIR}"

cleanup() {
  local exit_code=$?
  if [[ -n "${RECEIVER_PID}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
    kill "${RECEIVER_PID}" 2>/dev/null || true
    wait "${RECEIVER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${MOVER_PID}" ]] && kill -0 "${MOVER_PID}" 2>/dev/null; then
    kill "${MOVER_PID}" 2>/dev/null || true
    wait "${MOVER_PID}" 2>/dev/null || true
  fi
  return "${exit_code}"
}
trap cleanup EXIT

cat > "${CONFIG_FILE}" <<EOF
network:
  listen_port: ${LISTEN_PORT}
  bind_ip: "127.0.0.1"
  recvmmsg_batch_size: 64
  socket_rcvbuf_mb: 64
  enable_ip_freebind: false
  source_filter_enabled: false
reassembly:
  timeout_ms: 100
  max_contexts: 1024
  max_total_frags: 1024
  max_reasm_bytes_per_key: 16777216
reorder:
  window_size: 512
  timeout_ms: 50
queue:
  rawcpi_q_capacity: 64
  rawcpi_q_slot_size_mb: 2
logging:
  level: "INFO"
monitoring:
  metrics_port: 18081
  metrics_bind_ip: "127.0.0.1"
consumer:
  print_summary: false
  write_to_file: false
  output_dir: "${WORKDIR}/rawblocks"
  stats_interval_ms: 1000
capture:
  enabled: true
  spool_dir: "${SPOOL_DIR}"
  archive_dir: "${ARCHIVE_DIR}"
  spool_low_watermark_pct: 10
  archive_low_watermark_pct: 10
  archive_max_files: ${ARCHIVE_MAX_FILES}
  archive_max_age_days: 1
  max_file_size_mb: 0
  max_files: 16
  filter_packet_types: ["0x03"]
  filter_source_ids: []
performance:
  prefetch_hints_enabled: true
  qos_enabled: true
  heartbeat_max_queue_depth: 1000
EOF

start_receiver() {
  "${RECEIVER_BIN}" --config "${CONFIG_FILE}" >> "${RECEIVER_LOG}" 2>&1 &
  RECEIVER_PID=$!
  sleep 1
}

start_mover() {
  QDGZ300_RECEIVER_CONFIG="${CONFIG_FILE}" \
  QDGZ300_SPOOL_POLL_INTERVAL_SEC="${POLL_SEC}" \
  QDGZ300_DEFAULT_METRICS_DIR="${METRICS_DIR}" \
  "${MOVER_BIN}" >> "${MOVER_LOG}" 2>&1 &
  MOVER_PID=$!
  sleep 1
}

restart_receiver() {
  if [[ -n "${RECEIVER_PID}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
    kill "${RECEIVER_PID}" 2>/dev/null || true
    wait "${RECEIVER_PID}" 2>/dev/null || true
  fi
  RECEIVER_PID=""
  start_receiver
}

send_phase() {
  local label="$1"
  local duration="$2"
  local pps="$3"
  python3 - "${LISTEN_PORT}" "${duration}" "${pps}" "${label}" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
duration = int(sys.argv[2])
pps = int(sys.argv[3])
label = sys.argv[4]

pkt = bytearray(32)
pkt[18] = 0x03
pkt[20] = 0x11
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
interval = 1.0 / max(pps, 1)
deadline = time.time() + duration
count = 0
while time.time() < deadline:
    sock.sendto(pkt, ("127.0.0.1", port))
    count += 1
    time.sleep(interval)
print(f"{label}_sent={count}")
PY
}

metric_value() {
  local name="$1"
  awk -v metric="${name}" '$1 == metric {print $2; exit}' "${METRICS_DIR}/qdgz300_spool_mover.prom"
}

start_receiver
start_mover

send_phase "phase1" "${PHASE_SECONDS}" "${SEND_PPS}" | tee -a "${SUMMARY_FILE}"
restart_receiver
send_phase "phase2" "${PHASE_SECONDS}" "${SEND_PPS}" | tee -a "${SUMMARY_FILE}"
sleep $((POLL_SEC + 2))

ARCHIVED_TOTAL="$(metric_value qdgz300_spool_mover_archived_total)"
FAILURES_TOTAL="$(metric_value qdgz300_spool_mover_archive_failures_total)"
RETENTION_COUNT_TOTAL="$(metric_value qdgz300_spool_mover_retention_removed_by_count_total)"
SPOOL_FILES="$(metric_value qdgz300_spool_mover_spool_files)"
ARCHIVE_FILES="$(metric_value qdgz300_spool_mover_archive_files)"

PACKETS_RECEIVED_LOG="$(grep 'Final stats: packets_received=' "${RECEIVER_LOG}" | tail -n 1 | sed -E 's/.*packets_received=([0-9]+).*/\1/' || true)"
QUEUE_DROPPED_LOG="$(grep 'Capture spool summary:' "${RECEIVER_LOG}" | tail -n 1 | sed -E 's/.*queue_dropped=([0-9]+).*/\1/' || true)"
WRITE_ERRORS_LOG="$(grep 'Capture spool summary:' "${RECEIVER_LOG}" | tail -n 1 | sed -E 's/.*write_errors=([0-9]+).*/\1/' || true)"

SEALED_LEFT="$(find "${SPOOL_DIR}" -maxdepth 1 -type f -name '*.pcap' | wc -l | tr -d ' ')"
PARTIAL_LEFT="$(find "${SPOOL_DIR}" -maxdepth 1 -type f -name '*.pcap.part' | wc -l | tr -d ' ')"

{
  echo "workdir=${WORKDIR}"
  echo "archived_total=${ARCHIVED_TOTAL:-0}"
  echo "archive_failures_total=${FAILURES_TOTAL:-0}"
  echo "retention_removed_by_count_total=${RETENTION_COUNT_TOTAL:-0}"
  echo "receiver_packets_received_log=${PACKETS_RECEIVED_LOG:-0}"
  echo "queue_dropped_log=${QUEUE_DROPPED_LOG:-0}"
  echo "write_errors_log=${WRITE_ERRORS_LOG:-0}"
  echo "spool_files_metric=${SPOOL_FILES:-0}"
  echo "archive_files_metric=${ARCHIVE_FILES:-0}"
  echo "sealed_files_left=${SEALED_LEFT}"
  echo "partial_files_left=${PARTIAL_LEFT}"
} | tee -a "${SUMMARY_FILE}"

[[ -n "${ARCHIVED_TOTAL}" && "${ARCHIVED_TOTAL}" -gt 0 ]] || { echo "[ERROR] archived_total not increasing" >&2; exit 1; }
[[ -n "${FAILURES_TOTAL}" && "${FAILURES_TOTAL}" -eq 0 ]] || { echo "[ERROR] mover reported archive failures" >&2; exit 1; }
[[ -n "${RETENTION_COUNT_TOTAL}" && "${RETENTION_COUNT_TOTAL}" -gt 0 ]] || { echo "[ERROR] archive retention by count did not trigger" >&2; exit 1; }
[[ -n "${PACKETS_RECEIVED_LOG}" && "${PACKETS_RECEIVED_LOG}" -gt 0 ]] || { echo "[ERROR] receiver packets_received did not increase" >&2; exit 1; }
[[ -n "${QUEUE_DROPPED_LOG}" && "${QUEUE_DROPPED_LOG}" -eq 0 ]] || { echo "[ERROR] capture queue_dropped not zero" >&2; exit 1; }
[[ -n "${WRITE_ERRORS_LOG}" && "${WRITE_ERRORS_LOG}" -eq 0 ]] || { echo "[ERROR] capture write_errors not zero" >&2; exit 1; }
[[ "${SEALED_LEFT}" -eq 0 ]] || { echo "[ERROR] sealed pcap files still left in spool" >&2; exit 1; }
[[ -n "${ARCHIVE_FILES}" && "${ARCHIVE_FILES}" -le "${ARCHIVE_MAX_FILES}" ]] || { echo "[ERROR] archive file count exceeds retention limit" >&2; exit 1; }

echo "[OK] spool soak test passed"
