#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import pathlib
import re
import signal
import statistics
import subprocess
import threading
import time
import urllib.request
from datetime import datetime


REPO = pathlib.Path("/home/devuser/workspace/qdgz300_backend")
STAMP = datetime.now().strftime("%Y%m%d_%H%M%S")
OUTDIR = pathlib.Path("/home/devuser/runtime_logs") / f"perf_{STAMP}"
CONFIG = OUTDIR / "receiver_perf.yaml"

METRIC_RE = re.compile(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{[^}]*\})?\s+([-+eE0-9\.]+)$")
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="([^"]*)"')


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def check_output(cmd: list[str], **kwargs) -> str:
    return subprocess.check_output(cmd, text=True, **kwargs)


def parse_metrics(text: str) -> dict[tuple[str, tuple[tuple[str, str], ...]], float]:
    out: dict[tuple[str, tuple[tuple[str, str], ...]], float] = {}
    for line in text.splitlines():
        match = METRIC_RE.match(line.strip())
        if not match:
            continue
        name, raw_labels, value = match.groups()
        labels = tuple(sorted(LABEL_RE.findall(raw_labels or "")))
        out[(name, labels)] = float(value)
    return out


def fetch_metrics() -> dict[tuple[str, tuple[tuple[str, str], ...]], float]:
    body = urllib.request.urlopen("http://127.0.0.1:8080/metrics", timeout=2).read().decode()
    return parse_metrics(body)


def metric(metrics: dict[tuple[str, tuple[tuple[str, str], ...]], float], name: str, **labels: str) -> float:
    return metrics.get((name, tuple(sorted(labels.items()))), 0.0)


def metric_sum(metrics: dict[tuple[str, tuple[tuple[str, str], ...]], float], name: str) -> float:
    total = 0.0
    for (metric_name, _), value in metrics.items():
        if metric_name == name:
            total += value
    return total


class CpuSampler:
    def __init__(self, pid: int):
        self.pid = pid
        self.samples: list[float] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._cpu_count = os.cpu_count() or 1
        self._prev_proc: int | None = None
        self._prev_sys: int | None = None

    def _read_proc(self) -> int:
        with open(f"/proc/{self.pid}/stat", "r", encoding="utf-8") as handle:
            parts = handle.read().split()
        return int(parts[13]) + int(parts[14])

    def _read_sys(self) -> int:
        with open("/proc/stat", "r", encoding="utf-8") as handle:
            return sum(int(x) for x in handle.readline().split()[1:])

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                proc_total = self._read_proc()
                sys_total = self._read_sys()
                if self._prev_proc is not None and self._prev_sys is not None and sys_total > self._prev_sys:
                    pct = ((proc_total - self._prev_proc) / (sys_total - self._prev_sys)) * 100.0 * self._cpu_count
                    if pct >= 0.0:
                        self.samples.append(pct)
                self._prev_proc = proc_total
                self._prev_sys = sys_total
            except Exception:
                pass
            time.sleep(0.5)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)


def wait_metrics(timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            fetch_metrics()
            return
        except Exception as exc:
            last_error = exc
            time.sleep(0.3)
    raise RuntimeError(f"metrics not ready: {last_error}")


def stop_existing_receivers() -> None:
    subprocess.run(["pkill", "-f", "receiver_app --config"], check=False)


def write_config() -> None:
    OUTDIR.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(
        f"""network:
  listen_port: 9999
  bind_ips:
    - "127.0.0.1"
    - "127.0.0.2"
    - "127.0.0.3"
  bind_ip: "0.0.0.0"
  recvmmsg_batch_size: 64
  socket_rcvbuf_mb: 256
  source_id_map: [0x11, 0x12, 0x13]
  cpu_affinity_map: [16, 17, 18]
  source_filter_enabled: false
  enable_so_reuseport: false
  local_device_id: 0x01
  recv_threads: 3
reassembly:
  timeout_ms: 100
  max_contexts: 1024
  max_total_frags: 1024
  sample_count_fixed: 4096
  max_reasm_bytes_per_key: 16777216
reorder:
  window_size: 512
  timeout_ms: 50
  enable_zero_fill: true
logging:
  level: "INFO"
  log_file: "{OUTDIR / 'receiver.log'}"
monitoring:
  metrics_port: 8080
  metrics_bind_ip: "127.0.0.1"
performance:
  numa_node: 1
  reassembler_cache_align_bytes: 64
  prefetch_hints_enabled: true
  qos_enabled: true
  rma_session_timeout_ms: 1000
  heartbeat_max_queue_depth: 1000
delivery:
  method: "callback"
  reconnect_interval_ms: 100
queue:
  rawcpi_q_capacity: 64
  rawcpi_q_slot_size_mb: 2
consumer:
  print_summary: false
  write_to_file: false
  output_dir: "/tmp/receiver_rawblocks"
  stats_interval_ms: 1000
""",
        encoding="utf-8",
    )


def start_receiver() -> subprocess.Popen[str]:
    stop_existing_receivers()
    stdout = open(OUTDIR / "receiver.stdout", "w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(REPO / "build_release/receiver_app"), "--config", str(CONFIG)],
        cwd=REPO,
        stdout=stdout,
        stderr=subprocess.STDOUT,
        text=True,
    )
    wait_metrics()
    time.sleep(1.0)
    try:
        thread_view = check_output(["ps", "-T", "-p", str(proc.pid), "-o", "pid,tid,psr,pcpu,comm"])
        (OUTDIR / "receiver_threads.txt").write_text(thread_view, encoding="utf-8")
    except Exception:
        pass
    return proc


def stop_proc(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


def load_json(path: pathlib.Path) -> dict[str, float]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_three_face_case(case_name: str, total_pps: int, duration: float = 10.0, payload: int = 992) -> dict[str, object]:
    before = fetch_metrics()
    rx_pid = int(check_output(["pgrep", "-n", "receiver_app"]).strip())
    sampler = CpuSampler(rx_pid)
    sampler.start()

    procs = []
    tx_reports = []
    per_face_pps = total_pps / 3.0

    for index, ip in enumerate(["127.0.0.1", "127.0.0.2", "127.0.0.3"], start=1):
        out = OUTDIR / f"{case_name}_face{index}_tx.json"
        tx_reports.append(out)
        cmd = [
            "taskset",
            "-c",
            "24-31",
            str(REPO / "build_release/fpga_emulator"),
            "--target-ip",
            ip,
            "--target-port",
            "9999",
            "--pps",
            str(per_face_pps),
            "--duration",
            str(duration),
            "--payload-bytes",
            str(payload),
            "--threads",
            "2",
            "--output",
            str(out),
        ]
        procs.append(subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    for proc in procs:
        proc.wait()

    time.sleep(1.0)
    sampler.stop()
    after = fetch_metrics()
    tx = [load_json(path) for path in tx_reports]
    sent_packets = sum(int(item["sent_packets"]) for item in tx)
    tx_actual_pps = sum(float(item["actual_pps"]) for item in tx)
    rx_packets = metric(after, "receiver_packets_received_total") - metric(before, "receiver_packets_received_total")
    rx_bytes = metric(after, "receiver_bytes_received_total") - metric(before, "receiver_bytes_received_total")
    drops = metric_sum(after, "receiver_packets_dropped_total") - metric_sum(before, "receiver_packets_dropped_total")

    reasons: dict[str, float] = {}
    for (name, labels), value in after.items():
        if name != "receiver_packets_dropped_total" or not labels:
            continue
        reason = dict(labels).get("reason", "unknown")
        delta = value - before.get((name, labels), 0.0)
        if delta > 0:
            reasons[reason] = delta

    return {
        "case": case_name,
        "mode": "receiver_app_3face",
        "target_total_pps": total_pps,
        "duration_sec": duration,
        "payload_bytes": payload,
        "tx_actual_pps": tx_actual_pps,
        "tx_sent_packets": sent_packets,
        "rx_packets": rx_packets,
        "rx_pps": rx_packets / duration,
        "throughput_mbps": (rx_bytes * 8.0 / 1_000_000.0) / duration,
        "drops": drops,
        "loss_rate_vs_tx": max(0.0, (sent_packets - rx_packets) / sent_packets) if sent_packets else 0.0,
        "cpu_avg_pct": statistics.mean(sampler.samples) if sampler.samples else 0.0,
        "cpu_peak_pct": max(sampler.samples) if sampler.samples else 0.0,
        "proc_latency_p99_us": metric(after, "receiver_processing_latency_us_p99"),
        "proc_latency_p999_us": metric(after, "receiver_processing_latency_us_p999"),
        "heartbeat_queue_depth": metric(after, "heartbeat_queue_depth"),
        "drop_reasons": reasons,
    }


def run_raw_case(case_name: str, target_pps: int, measure: float = 8.0, payload: int = 992) -> dict[str, object]:
    bench_out = OUTDIR / f"{case_name}_raw_rx.json"
    tx_out = OUTDIR / f"{case_name}_raw_tx.json"

    bench_cmd = [
        "taskset",
        "-c",
        "20",
        str(REPO / "build_release/tools/benchmarks/bench_m01_rx_limit"),
        "--bind-ip",
        "127.0.0.1",
        "--port",
        "10099",
        "--warmup-sec",
        "0.5",
        "--measure-sec",
        str(measure),
        "--output",
        str(bench_out),
    ]
    bench_proc = subprocess.Popen(bench_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.6)
    sampler = CpuSampler(bench_proc.pid)
    sampler.start()

    tx_cmd = [
        "taskset",
        "-c",
        "24-31",
        str(REPO / "build_release/fpga_emulator"),
        "--target-ip",
        "127.0.0.1",
        "--target-port",
        "10099",
        "--pps",
        str(target_pps),
        "--duration",
        str(measure + 0.5),
        "--payload-bytes",
        str(payload),
        "--threads",
        "2",
        "--output",
        str(tx_out),
    ]
    tx_proc = subprocess.Popen(tx_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tx_proc.wait()
    bench_proc.wait()
    sampler.stop()

    rx = load_json(bench_out)
    tx = load_json(tx_out)
    expected_packets = float(tx["actual_pps"]) * measure
    rx_packets = float(rx["received_valid"])

    return {
        "case": case_name,
        "mode": "raw_udp_single_port",
        "target_pps": target_pps,
        "measure_sec": measure,
        "payload_bytes": payload,
        "tx_actual_pps": float(tx["actual_pps"]),
        "expected_packets_in_measure": expected_packets,
        "rx_packets": rx_packets,
        "rx_pps": float(rx["valid_pps"]),
        "loss_rate_vs_tx_window": max(0.0, (expected_packets - rx_packets) / expected_packets) if expected_packets else 0.0,
        "cpu_avg_pct": statistics.mean(sampler.samples) if sampler.samples else 0.0,
        "cpu_peak_pct": max(sampler.samples) if sampler.samples else 0.0,
    }


def write_summary(results: dict[str, object]) -> str:
    lines = []
    lines.append("# Server Perf Summary")
    lines.append("")
    lines.append(f"- Output dir: {OUTDIR}")
    lines.append("- Receiver topology: 127.0.0.1/127.0.0.2/127.0.0.3 -> CPU16/17/18 (loopback simulation on server)")
    lines.append("- Sender affinity: CPU24-31")
    lines.append("")
    lines.append("## Receiver App 3-Face Sweep")
    lines.append("")
    lines.append("| Total Target PPS | Actual TX PPS | RX PPS | Loss | CPU Avg % | CPU Peak % | P99 us | Throughput Mbps |")
    lines.append("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for case in results["receiver_cases"]:
        lines.append(
            f"| {case['target_total_pps']:,} | {case['tx_actual_pps']:.0f} | {case['rx_pps']:.0f} | "
            f"{case['loss_rate_vs_tx']*100:.3f}% | {case['cpu_avg_pct']:.1f} | {case['cpu_peak_pct']:.1f} | "
            f"{case['proc_latency_p99_us']:.0f} | {case['throughput_mbps']:.1f} |"
        )
    lines.append("")
    lines.append("## Raw UDP Single-Port Sweep")
    lines.append("")
    lines.append("| Target PPS | Actual TX PPS | RX PPS | Loss | CPU Avg % | CPU Peak % |")
    lines.append("| ---: | ---: | ---: | ---: | ---: | ---: |")
    for case in results["raw_cases"]:
        lines.append(
            f"| {case['target_pps']:,} | {case['tx_actual_pps']:.0f} | {case['rx_pps']:.0f} | "
            f"{case['loss_rate_vs_tx_window']*100:.3f}% | {case['cpu_avg_pct']:.1f} | {case['cpu_peak_pct']:.1f} |"
        )
    lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    write_config()
    results: dict[str, object] = {"outdir": str(OUTDIR), "receiver_cases": [], "raw_cases": [], "env": {}}

    for name, cmd in {
        "hostname": ["hostname"],
        "kernel": ["uname", "-a"],
        "lscpu": ["lscpu"],
        "ip_br_addr": ["ip", "-br", "addr"],
    }.items():
        try:
            results["env"][name] = check_output(cmd)
        except Exception as exc:
            results["env"][name] = str(exc)

    receiver_proc: subprocess.Popen[str] | None = None
    try:
        receiver_proc = start_receiver()
        for total_pps in [300000, 600000, 900000, 1200000, 1500000, 1800000]:
            case = run_three_face_case(f"app_{total_pps // 1000}k", total_pps)
            results["receiver_cases"].append(case)
            if case["loss_rate_vs_tx"] > 0.001 or case["cpu_peak_pct"] > 95.0:
                break
    finally:
        if receiver_proc is not None:
            stop_proc(receiver_proc)

    for target_pps in [300000, 600000, 900000, 1200000, 1500000, 1800000]:
        case = run_raw_case(f"raw_{target_pps // 1000}k", target_pps)
        results["raw_cases"].append(case)
        if case["loss_rate_vs_tx_window"] > 0.001 or case["cpu_peak_pct"] > 95.0:
            break

    summary = write_summary(results)
    (OUTDIR / "summary.md").write_text(summary, encoding="utf-8")
    (OUTDIR / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(summary)
    print(f"RESULT_JSON={OUTDIR / 'results.json'}")
    print(f"SUMMARY_MD={OUTDIR / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
