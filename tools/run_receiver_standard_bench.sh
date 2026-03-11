#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build_wsl_native_perf"
RESULTS_DIR="${BUILD_DIR}/results/standard_bench_$(date +%Y%m%d_%H%M%S)"
BIND_IP="127.0.0.1"
TARGET_IP="127.0.0.1"
PORT=9999
SENDER_THREADS=2
COMMON_HEADER_BYTES=32
WARMUP_SEC=0.2
MEASURE_SEC=60
BURST_TOTAL_SEC=20
CPU_STOP_THRESHOLD=95.0
LOSS_STOP_THRESHOLD=0.001

PACKET_RATE_STEPS=(100000 200000 400000 600000 800000 1000000 1200000 1400000 1600000)
THROUGHPUT_STEPS_MBPS=(500 1000 2000 5000 8000 10000)
SENS_PACKET_SIZES=(1500 1024 512 256 128)
SENS_TARGET_MBPS=5000

RX_BIN="${BUILD_DIR}/tools/benchmarks/bench_m01_rx_limit"
TX_BIN="${BUILD_DIR}/fpga_emulator"

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --build-dir DIR          Build directory (default: ${BUILD_DIR})
  --results-dir DIR        Output directory (default: auto timestamp directory)
  --port N                 UDP port (default: ${PORT})
  --sender-threads N       Sender worker threads (default: ${SENDER_THREADS})
  --measure-sec N          Per-step measure duration in seconds (default: ${MEASURE_SEC})
  --burst-total-sec N      Burst test total seconds (default: ${BURST_TOTAL_SEC})
  --warmup-sec N           Receiver warmup seconds (default: ${WARMUP_SEC})
  --loss-stop-threshold X  Stop threshold of loss rate (default: ${LOSS_STOP_THRESHOLD}, e.g. 0.001 = 0.1%)
  --quick                  Quick mode (10s per step, burst total 10s)
  -h, --help               Show help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --results-dir)
            RESULTS_DIR="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --sender-threads)
            SENDER_THREADS="$2"
            shift 2
            ;;
        --measure-sec)
            MEASURE_SEC="$2"
            shift 2
            ;;
        --burst-total-sec)
            BURST_TOTAL_SEC="$2"
            shift 2
            ;;
        --warmup-sec)
            WARMUP_SEC="$2"
            shift 2
            ;;
        --loss-stop-threshold)
            LOSS_STOP_THRESHOLD="$2"
            shift 2
            ;;
        --quick)
            MEASURE_SEC=10
            BURST_TOTAL_SEC=10
            shift 1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

RX_BIN="${BUILD_DIR}/tools/benchmarks/bench_m01_rx_limit"
TX_BIN="${BUILD_DIR}/fpga_emulator"

mkdir -p "${RESULTS_DIR}"

if [[ ! -x "${RX_BIN}" || ! -x "${TX_BIN}" ]]; then
    echo "[INFO] Bench binaries not found, building..."
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DBUILD_TESTING=ON -DENABLE_GPU=OFF -DENABLE_PROTOBUF=OFF
    cmake --build "${BUILD_DIR}" --target bench_m01_rx_limit fpga_emulator -j"$(nproc)"
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required." >&2
    exit 1
fi

HAVE_PIDSTAT=0
if command -v pidstat >/dev/null 2>&1; then
    HAVE_PIDSTAT=1
fi

run_case() {
    local case_id="$1"
    local test_group="$2"
    local target_pps="$3"
    local duration_sec="$4"
    local packet_size_total="$5"
    local notes="${6:-}"

    local payload_bytes=$((packet_size_total - COMMON_HEADER_BYTES))
    if (( payload_bytes < 64 )); then
        payload_bytes=64
    fi

    local rx_json="${RESULTS_DIR}/${case_id}_rx.json"
    local tx_json="${RESULTS_DIR}/${case_id}_tx.json"
    local rx_log="${RESULTS_DIR}/${case_id}_rx.log"
    local tx_log="${RESULTS_DIR}/${case_id}_tx.log"
    local cpu_log="${RESULTS_DIR}/${case_id}_cpu.log"
    local case_json="${RESULTS_DIR}/${case_id}_case.json"

    (
        "${RX_BIN}" \
            --bind-ip "${BIND_IP}" \
            --port "${PORT}" \
            --warmup-sec "${WARMUP_SEC}" \
            --measure-sec "${duration_sec}" \
            --output "${rx_json}" >"${rx_log}" 2>&1
    ) &
    local rx_pid=$!

    sleep "${WARMUP_SEC}"

    local pidstat_pid=""
    if [[ "${HAVE_PIDSTAT}" -eq 1 ]]; then
        pidstat -u -p "${rx_pid}" 1 >"${cpu_log}" 2>&1 &
        pidstat_pid=$!
    fi

    "${TX_BIN}" \
        --target-ip "${TARGET_IP}" \
        --target-port "${PORT}" \
        --pps "${target_pps}" \
        --duration "${duration_sec}" \
        --payload-bytes "${payload_bytes}" \
        --threads "${SENDER_THREADS}" \
        --output "${tx_json}" >"${tx_log}" 2>&1 || true

    wait "${rx_pid}" || true

    if [[ -n "${pidstat_pid}" ]]; then
        kill "${pidstat_pid}" >/dev/null 2>&1 || true
        wait "${pidstat_pid}" >/dev/null 2>&1 || true
    fi

    python3 - <<PY
import json
from pathlib import Path

rx_path = Path(r"${rx_json}")
tx_path = Path(r"${tx_json}")
cpu_path = Path(r"${cpu_log}")

def read_json(path: Path):
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}

rx = read_json(rx_path)
tx = read_json(tx_path)

tx_actual_pps = float(tx.get("actual_pps", 0.0) or 0.0)
rx_valid_pps = float(rx.get("valid_pps", 0.0) or 0.0)
loss_rate = 1.0 - (rx_valid_pps / tx_actual_pps) if tx_actual_pps > 0 else 1.0
if loss_rate < 0:
    loss_rate = 0.0

cpu_avg = None
if cpu_path.exists():
    pid = str(int(rx.get("pid", 0) or 0))
    for line in cpu_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        s = line.strip()
        if not s.startswith("Average:"):
            continue
        parts = s.split()
        if len(parts) < 8:
            continue
        for i in range(len(parts) - 1):
            if parts[i].isdigit():
                try:
                    cpu_avg = float(parts[i + 4])
                except Exception:
                    pass
                break

packet_size_total = int(${packet_size_total})
throughput_mbps = rx_valid_pps * packet_size_total * 8.0 / 1e6

obj = {
    "case_id": r"${case_id}",
    "test_group": r"${test_group}",
    "target_pps": float(${target_pps}),
    "duration_sec": float(${duration_sec}),
    "packet_size_total_bytes": packet_size_total,
    "payload_bytes": int(${payload_bytes}),
    "tx_actual_pps": tx_actual_pps,
    "rx_valid_pps": rx_valid_pps,
    "loss_rate": loss_rate,
    "throughput_mbps": throughput_mbps,
    "cpu_avg_percent": cpu_avg,
    "notes": r"${notes}",
    "raw": {"tx": tx, "rx": rx}
}

Path(r"${case_json}").write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"[CASE] {obj['case_id']} target_pps={obj['target_pps']:.2f} tx={obj['tx_actual_pps']:.2f} rx={obj['rx_valid_pps']:.2f} loss={obj['loss_rate']:.6f} cpu={obj['cpu_avg_percent']}")
PY
}

echo "[INFO] Results dir: ${RESULTS_DIR}"
echo "[INFO] Running Test#1 最大稳定包率..."

case_files=()

max_stable_pps=0
for rate in "${PACKET_RATE_STEPS[@]}"; do
    case_id="t1_pps_${rate}"
    run_case "${case_id}" "最大稳定包率" "${rate}" "${MEASURE_SEC}" 1024 "包率阶梯"
    case_files+=("${RESULTS_DIR}/${case_id}_case.json")

    eval_data="$(python3 - <<PY
import json
d=json.load(open(r"${RESULTS_DIR}/${case_id}_case.json", encoding="utf-8"))
cpu=d.get("cpu_avg_percent")
cpu = -1 if cpu is None else float(cpu)
print(f"{d['loss_rate']} {cpu} {d['rx_valid_pps']}")
PY
)"
    loss="$(echo "${eval_data}" | awk '{print $1}')"
    cpu="$(echo "${eval_data}" | awk '{print $2}')"
    rx_pps="$(echo "${eval_data}" | awk '{print $3}')"

    if python3 - <<PY
loss=float("${loss}")
cpu=float("${cpu}")
ok_loss = loss <= float("${LOSS_STOP_THRESHOLD}")
ok_cpu = (cpu < 0) or (cpu <= float("${CPU_STOP_THRESHOLD}"))
raise SystemExit(0 if (ok_loss and ok_cpu) else 1)
PY
    then
        max_stable_pps="${rx_pps}"
    else
        echo "[INFO] Test#1 stop condition reached at target ${rate} pps"
        break
    fi
done

echo "[INFO] Running Test#2 最大稳定吞吐量..."
max_stable_throughput=0
for bw in "${THROUGHPUT_STEPS_MBPS[@]}"; do
    pps="$(python3 - <<PY
bw=float("${bw}")
pkt=float(1024)
print(bw*1e6/(pkt*8.0))
PY
)"
    case_id="t2_bw_${bw}Mbps"
    run_case "${case_id}" "最大稳定吞吐量" "${pps}" "${MEASURE_SEC}" 1024 "吞吐阶梯 ${bw}Mbps"
    case_files+=("${RESULTS_DIR}/${case_id}_case.json")

    eval_data="$(python3 - <<PY
import json
d=json.load(open(r"${RESULTS_DIR}/${case_id}_case.json", encoding="utf-8"))
cpu=d.get("cpu_avg_percent")
cpu = -1 if cpu is None else float(cpu)
print(f"{d['loss_rate']} {cpu} {d['throughput_mbps']}")
PY
)"
    loss="$(echo "${eval_data}" | awk '{print $1}')"
    cpu="$(echo "${eval_data}" | awk '{print $2}')"
    th="$(echo "${eval_data}" | awk '{print $3}')"

    if python3 - <<PY
loss=float("${loss}")
cpu=float("${cpu}")
ok_loss = loss <= float("${LOSS_STOP_THRESHOLD}")
ok_cpu = (cpu < 0) or (cpu <= float("${CPU_STOP_THRESHOLD}"))
raise SystemExit(0 if (ok_loss and ok_cpu) else 1)
PY
    then
        max_stable_throughput="${th}"
    else
        echo "[INFO] Test#2 stop condition reached at ${bw} Mbps target"
        break
    fi
done

echo "[INFO] Running Test#3 包长敏感性..."
for packet_size in "${SENS_PACKET_SIZES[@]}"; do
    pps="$(python3 - <<PY
mbps=float("${SENS_TARGET_MBPS}")
pkt=float("${packet_size}")
print(mbps*1e6/(pkt*8.0))
PY
)"
    case_id="t3_pkt_${packet_size}"
    run_case "${case_id}" "包长敏感性" "${pps}" "${MEASURE_SEC}" "${packet_size}" "目标吞吐 ${SENS_TARGET_MBPS}Mbps"
    case_files+=("${RESULTS_DIR}/${case_id}_case.json")
done

echo "[INFO] Running Test#4 突发流量..."
BURST_LOW=300000
BURST_HIGH=1200000
BURST_PHASE1=5
BURST_PHASE2=1
BURST_PHASE3=$((BURST_TOTAL_SEC - BURST_PHASE1 - BURST_PHASE2))
if (( BURST_PHASE3 < 1 )); then
    BURST_PHASE3=1
fi
BURST_EFFECTIVE=$((BURST_PHASE1 + BURST_PHASE2 + BURST_PHASE3))
BURST_WEIGHTED_PPS="$(python3 - <<PY
low=float("${BURST_LOW}")
high=float("${BURST_HIGH}")
t1=float("${BURST_PHASE1}")
t2=float("${BURST_PHASE2}")
t3=float("${BURST_PHASE3}")
print((low*t1 + high*t2 + low*t3)/(t1+t2+t3))
PY
)"
case_id="t4_burst"
run_case "${case_id}" "突发流量" "${BURST_WEIGHTED_PPS}" "${BURST_EFFECTIVE}" 1024 "模型: 0-5s ${BURST_LOW}pps, 5-6s ${BURST_HIGH}pps, 其余${BURST_LOW}pps (等效均值)"
case_files+=("${RESULTS_DIR}/${case_id}_case.json")

echo "[INFO] Generating final reports..."
python3 - <<PY
import json
from pathlib import Path
from datetime import datetime

results_dir = Path(r"${RESULTS_DIR}")
case_files = sorted(results_dir.glob("*_case.json"))
cases = []
for p in case_files:
    if p.exists():
        cases.append(json.loads(p.read_text(encoding="utf-8")))

def select(group):
    return [c for c in cases if c.get("test_group") == group]

t1 = select("最大稳定包率")
t2 = select("最大稳定吞吐量")
t3 = select("包长敏感性")
t4 = select("突发流量")

def stable_filter(arr):
    out = []
    for c in arr:
        loss_ok = float(c.get("loss_rate", 1)) <= float(${LOSS_STOP_THRESHOLD})
        cpu = c.get("cpu_avg_percent")
        cpu_ok = True if cpu is None else float(cpu) <= float(${CPU_STOP_THRESHOLD})
        if loss_ok and cpu_ok:
            out.append(c)
    return out

t1_stable = stable_filter(t1)
t2_stable = stable_filter(t2)
max_stable_pps = max((c["rx_valid_pps"] for c in t1_stable), default=0.0)
max_stable_mbps = max((c["throughput_mbps"] for c in t2_stable), default=0.0)

summary = {
    "report_name": "雷达数据接收模块标准化压测报告",
    "generated_at": datetime.now().isoformat(timespec="seconds"),
    "environment": {
        "mode": "localhost loopback",
        "receiver_bin": r"${RX_BIN}",
        "sender_bin": r"${TX_BIN}",
        "port": int(${PORT}),
        "sender_threads": int(${SENDER_THREADS}),
        "measure_sec": float(${MEASURE_SEC}),
        "burst_total_sec": int(${BURST_TOTAL_SEC}),
        "cpu_stop_threshold_percent": float(${CPU_STOP_THRESHOLD}),
        "loss_stop_threshold": float(${LOSS_STOP_THRESHOLD})
    },
    "kpi": {
        "max_stable_pps": max_stable_pps,
        "max_stable_throughput_mbps": max_stable_mbps,
        "max_stable_throughput_gbps": max_stable_mbps / 1000.0
    },
    "tests": {
        "最大稳定包率": t1,
        "最大稳定吞吐量": t2,
        "包长敏感性": t3,
        "突发流量": t4
    }
}

json_path = results_dir / "receiver_standard_bench_report.json"
json_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

def fmt(v, nd=2):
    return f"{v:.{nd}f}"

lines = []
lines.append("# 雷达数据接收模块标准化压测总报告")
lines.append("")
lines.append(f"- 生成时间：{summary['generated_at']}")
lines.append("- 测试环境：本机回环（localhost）")
lines.append(f"- Receiver：{summary['environment']['receiver_bin']}")
lines.append(f"- Sender：{summary['environment']['sender_bin']}")
lines.append("")
lines.append("## 结论摘要")
lines.append("")
lines.append(f"- 最大稳定包率：**{fmt(summary['kpi']['max_stable_pps'], 0)} 包/秒**")
lines.append(f"- 最大稳定吞吐量：**{fmt(summary['kpi']['max_stable_throughput_gbps'], 3)} Gbps**")
lines.append("")
lines.append("## 测试一：最大稳定包率（1024B，阶梯升压）")
lines.append("")
lines.append("| 目标包率(pps) | 实际发送(pps) | 接收有效(pps) | 丢包率 | CPU均值(%) |")
lines.append("|---:|---:|---:|---:|---:|")
for c in t1:
    cpu = "-" if c.get("cpu_avg_percent") is None else fmt(c["cpu_avg_percent"], 2)
    lines.append(f"| {fmt(c['target_pps'],0)} | {fmt(c['tx_actual_pps'],2)} | {fmt(c['rx_valid_pps'],2)} | {fmt(c['loss_rate']*100,4)}% | {cpu} |")

lines.append("")
lines.append("## 测试二：最大吞吐量（1024B，带宽阶梯）")
lines.append("")
lines.append("| 目标场景 | 实际发送(pps) | 接收有效(pps) | 实测吞吐(Mbps) | 丢包率 | CPU均值(%) |")
lines.append("|---|---:|---:|---:|---:|---:|")
for c in t2:
    cpu = "-" if c.get("cpu_avg_percent") is None else fmt(c["cpu_avg_percent"], 2)
    lines.append(f"| {c['notes']} | {fmt(c['tx_actual_pps'],2)} | {fmt(c['rx_valid_pps'],2)} | {fmt(c['throughput_mbps'],2)} | {fmt(c['loss_rate']*100,4)}% | {cpu} |")

lines.append("")
lines.append("## 测试三：包长敏感性（目标 5Gbps）")
lines.append("")
lines.append("| 总包长(Bytes) | 实际发送(pps) | 接收有效(pps) | 实测吞吐(Mbps) | 丢包率 | CPU均值(%) |")
lines.append("|---:|---:|---:|---:|---:|---:|")
for c in t3:
    cpu = "-" if c.get("cpu_avg_percent") is None else fmt(c["cpu_avg_percent"], 2)
    lines.append(f"| {c['packet_size_total_bytes']} | {fmt(c['tx_actual_pps'],2)} | {fmt(c['rx_valid_pps'],2)} | {fmt(c['throughput_mbps'],2)} | {fmt(c['loss_rate']*100,4)}% | {cpu} |")

lines.append("")
lines.append("## 测试四：突发流量")
lines.append("")
if t4:
    c = t4[0]
    cpu = "-" if c.get("cpu_avg_percent") is None else fmt(c["cpu_avg_percent"], 2)
    lines.append(f"- 模型：{c['notes']}")
    lines.append(f"- 实际发送包率：{fmt(c['tx_actual_pps'],2)} pps")
    lines.append(f"- 接收有效包率：{fmt(c['rx_valid_pps'],2)} pps")
    lines.append(f"- 丢包率：{fmt(c['loss_rate']*100,4)}%")
    lines.append(f"- CPU均值：{cpu}%")

lines.append("")
lines.append("## 说明")
lines.append("")
lines.append("- 本报告代表软件处理上限，不代表真实网卡/交换机/链路能力。")
lines.append("- 突发测试当前由等效均值包率近似模拟。若需严格分段发送，可再扩展 sender 支持时间分段速率。")

md_path = results_dir / "receiver_standard_bench_report.md"
md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

print(f"JSON report: {json_path}")
print(f"Markdown report: {md_path}")
PY

echo "[DONE] Standard bench completed."
