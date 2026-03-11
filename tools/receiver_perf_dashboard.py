#!/usr/bin/env python3
"""
Local real-time dashboard for receiver performance.

Data source:
- Receiver Prometheus endpoint, default: http://127.0.0.1:8080/metrics

Dashboard:
- http://127.0.0.1:8090
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import threading
import time
import urllib.request
from collections import deque
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Deque, Dict, List, Optional, Tuple


METRIC_LINE_RE = re.compile(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{[^}]*\})?\s+([-+eE0-9\.]+)$")
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="([^"]*)"')


def parse_labels(raw: Optional[str]) -> Dict[str, str]:
    if not raw:
        return {}
    out: Dict[str, str] = {}
    for k, v in LABEL_RE.findall(raw):
        out[k] = v
    return out


def parse_prometheus_text(text: str) -> Dict[Tuple[str, Tuple[Tuple[str, str], ...]], float]:
    parsed: Dict[Tuple[str, Tuple[Tuple[str, str], ...]], float] = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        m = METRIC_LINE_RE.match(s)
        if not m:
            continue
        name, raw_labels, value_text = m.groups()
        try:
            value = float(value_text)
        except ValueError:
            continue
        labels = tuple(sorted(parse_labels(raw_labels).items()))
        parsed[(name, labels)] = value
    return parsed


def metric_value(
    metrics: Dict[Tuple[str, Tuple[Tuple[str, str], ...]], float],
    name: str,
    labels: Optional[Dict[str, str]] = None,
) -> float:
    label_tuple = tuple(sorted((labels or {}).items()))
    return metrics.get((name, label_tuple), 0.0)


def metric_sum_by_name(metrics: Dict[Tuple[str, Tuple[Tuple[str, str], ...]], float], name: str) -> float:
    total = 0.0
    for (metric_name, _), value in metrics.items():
        if metric_name == name:
            total += value
    return total


def find_pid_by_name(proc_name: str) -> Optional[int]:
    try:
        out = subprocess.check_output(["pgrep", "-n", proc_name], text=True, stderr=subprocess.DEVNULL).strip()
        if not out:
            return None
        return int(out)
    except Exception:
        return None


class ProcessCpuSampler:
    def __init__(self, pid: int):
        self.pid = pid
        self.prev_proc_total: Optional[int] = None
        self.prev_sys_total: Optional[int] = None
        self.cpu_count = os.cpu_count() or 1

    def _read_proc_ticks(self) -> Optional[int]:
        try:
            with open(f"/proc/{self.pid}/stat", "r", encoding="utf-8") as f:
                parts = f.read().split()
            utime = int(parts[13])
            stime = int(parts[14])
            return utime + stime
        except Exception:
            return None

    def _read_sys_ticks(self) -> Optional[int]:
        try:
            with open("/proc/stat", "r", encoding="utf-8") as f:
                first = f.readline()
            parts = first.split()
            if not parts or parts[0] != "cpu":
                return None
            return sum(int(x) for x in parts[1:])
        except Exception:
            return None

    def sample_percent(self) -> Optional[float]:
        proc_total = self._read_proc_ticks()
        sys_total = self._read_sys_ticks()
        if proc_total is None or sys_total is None:
            return None
        if self.prev_proc_total is None or self.prev_sys_total is None:
            self.prev_proc_total = proc_total
            self.prev_sys_total = sys_total
            return 0.0
        d_proc = proc_total - self.prev_proc_total
        d_sys = sys_total - self.prev_sys_total
        self.prev_proc_total = proc_total
        self.prev_sys_total = sys_total
        if d_proc < 0 or d_sys <= 0:
            return 0.0
        return (d_proc / d_sys) * 100.0 * self.cpu_count


@dataclass
class SamplePoint:
    ts: float
    elapsed_sec: float
    throughput_mbps: float
    packet_rate_pps: float
    drop_count_sec: float
    drop_total: float
    cpu_percent: Optional[float]

    def to_json(self) -> Dict[str, float]:
        return {
            "ts": self.ts,
            "elapsed_sec": self.elapsed_sec,
            "throughput_mbps": self.throughput_mbps,
            "packet_rate_pps": self.packet_rate_pps,
            "drop_count_sec": self.drop_count_sec,
            "drop_total": self.drop_total,
            "cpu_percent": self.cpu_percent,
        }


class DashboardState:
    def __init__(self, max_points: int):
        self.max_points = max_points
        self.start_ts = time.time()
        self.lock = threading.Lock()
        self.points: Deque[SamplePoint] = deque(maxlen=max_points)
        self.latest_raw: Dict[str, float] = {}
        self.last_error: Optional[str] = None

    def append(self, point: SamplePoint, raw: Dict[str, float]) -> None:
        with self.lock:
            self.points.append(point)
            self.latest_raw = raw
            self.last_error = None

    def set_error(self, err: str) -> None:
        with self.lock:
            self.last_error = err

    def snapshot(self) -> Dict[str, object]:
        with self.lock:
            return {
                "start_ts": self.start_ts,
                "points": [p.to_json() for p in self.points],
                "latest_raw": self.latest_raw,
                "last_error": self.last_error,
            }


class Poller(threading.Thread):
    def __init__(
        self,
        state: DashboardState,
        metrics_url: str,
        interval_sec: float,
        packet_size_bytes: int,
        cpu_sampler: Optional[ProcessCpuSampler],
    ):
        super().__init__(daemon=True)
        self.state = state
        self.metrics_url = metrics_url
        self.interval_sec = interval_sec
        self.packet_size_bytes = packet_size_bytes
        self.cpu_sampler = cpu_sampler
        self.stop_event = threading.Event()
        self.prev_ts: Optional[float] = None
        self.prev_packets: Optional[float] = None
        self.prev_bytes: Optional[float] = None
        self.prev_drops: Optional[float] = None

    def run(self) -> None:
        while not self.stop_event.is_set():
            t0 = time.time()
            try:
                req = urllib.request.Request(self.metrics_url, method="GET")
                with urllib.request.urlopen(req, timeout=1.5) as resp:
                    body = resp.read().decode("utf-8", errors="ignore")
                parsed = parse_prometheus_text(body)
                packets_total = metric_value(parsed, "receiver_packets_received_total")
                bytes_total = metric_value(parsed, "receiver_bytes_received_total")
                drops_total = metric_sum_by_name(parsed, "receiver_packets_dropped_total")
                now_ts = time.time()
                dt = (now_ts - self.prev_ts) if self.prev_ts is not None else 0.0

                pps = 0.0
                throughput_mbps = 0.0
                drop_sec = 0.0
                if dt > 0 and self.prev_packets is not None and self.prev_bytes is not None and self.prev_drops is not None:
                    d_packets = packets_total - self.prev_packets
                    d_bytes = bytes_total - self.prev_bytes
                    d_drops = drops_total - self.prev_drops
                    if d_packets < 0:
                        d_packets = 0.0
                    if d_bytes < 0:
                        d_bytes = 0.0
                    if d_drops < 0:
                        d_drops = 0.0
                    pps = d_packets / dt
                    # Prefer byte counter from receiver metrics. If unavailable (always zero),
                    # fall back to packet-rate estimation based on configured packet size.
                    if d_bytes > 0:
                        throughput_mbps = (d_bytes * 8.0 / 1_000_000.0) / dt
                    else:
                        throughput_mbps = (pps * self.packet_size_bytes * 8.0) / 1_000_000.0
                    drop_sec = d_drops / dt

                cpu_percent = self.cpu_sampler.sample_percent() if self.cpu_sampler else None
                elapsed = now_ts - self.state.start_ts
                point = SamplePoint(
                    ts=now_ts,
                    elapsed_sec=elapsed,
                    throughput_mbps=throughput_mbps,
                    packet_rate_pps=pps,
                    drop_count_sec=drop_sec,
                    drop_total=drops_total,
                    cpu_percent=cpu_percent,
                )
                self.state.append(
                    point,
                    {
                        "packets_total": packets_total,
                        "bytes_total": bytes_total,
                        "drops_total": drops_total,
                        "packet_size_bytes": self.packet_size_bytes,
                    },
                )
                self.prev_ts = now_ts
                self.prev_packets = packets_total
                self.prev_bytes = bytes_total
                self.prev_drops = drops_total
            except Exception as ex:
                self.state.set_error(str(ex))

            elapsed_loop = time.time() - t0
            sleep_sec = max(0.05, self.interval_sec - elapsed_loop)
            self.stop_event.wait(sleep_sec)

    def stop(self) -> None:
        self.stop_event.set()


def dashboard_html() -> str:
    return """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Radar Receiver 性能监控面板</title>
  <style>
    :root {
      --bg-a: #0f1b1d;
      --bg-b: #1e2f26;
      --card: rgba(255, 255, 255, 0.9);
      --ink: #143022;
      --muted: #4e6a5a;
      --line-a: #0f766e;
      --line-b: #0ea5e9;
      --line-c: #ea580c;
      --line-d: #dc2626;
      --grid: #d9e7dd;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Noto Sans SC", "Microsoft YaHei", "PingFang SC", sans-serif;
      color: var(--ink);
      background:
        radial-gradient(1200px 700px at -10% -10%, #2f6f57 0%, transparent 60%),
        radial-gradient(1000px 600px at 110% 0%, #235a6d 0%, transparent 58%),
        linear-gradient(140deg, var(--bg-a), var(--bg-b));
      min-height: 100vh;
      padding: 20px;
    }
    .wrap { max-width: 1200px; margin: 0 auto; }
    .head {
      background: var(--card);
      border-radius: 18px;
      padding: 16px 18px;
      margin-bottom: 14px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.18);
    }
    h1 { margin: 0 0 8px; font-size: 24px; letter-spacing: 0.02em; }
    .meta { color: var(--muted); font-size: 14px; display: flex; gap: 14px; flex-wrap: wrap; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
    .card {
      background: var(--card);
      border-radius: 16px;
      padding: 12px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.16);
    }
    .title {
      font-weight: 700; font-size: 16px; margin: 0 0 8px;
      display: flex; align-items: center; justify-content: space-between;
    }
    .badge {
      font-size: 12px; padding: 3px 8px; border-radius: 999px;
      background: #edf7f0; color: #246247;
    }
    canvas { width: 100%; height: 250px; display: block; border-radius: 10px; background: #fbfefc; }
    .foot {
      margin-top: 14px; color: #e4f5e9; font-size: 13px;
      display: flex; justify-content: space-between; gap: 10px; flex-wrap: wrap;
    }
    .err { color: #ffded8; font-weight: 700; }
    @media (max-width: 900px) {
      .grid { grid-template-columns: 1fr; }
      canvas { height: 220px; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="head">
      <h1>Radar Receiver 性能监控面板</h1>
      <div class="meta">
        <span id="runTime">运行时间: 0s</span>
        <span id="latestText">最新采样: -</span>
        <span id="statusText">状态: 启动中</span>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <p class="title">吞吐量曲线 <span class="badge">Mbps</span></p>
        <canvas id="throughput"></canvas>
      </div>
      <div class="card">
        <p class="title">包率曲线 <span class="badge">pps</span></p>
        <canvas id="pps"></canvas>
      </div>
      <div class="card">
        <p class="title">CPU 使用率曲线 <span class="badge">%</span></p>
        <canvas id="cpu"></canvas>
      </div>
      <div class="card">
        <p class="title">丢包曲线 <span class="badge">count/s</span></p>
        <canvas id="drop"></canvas>
      </div>
    </div>

    <div class="foot">
      <span>每秒自动刷新，显示最近历史窗口。</span>
      <span id="errText" class="err"></span>
    </div>
  </div>

  <script>
    const colors = {
      throughput: "#0f766e",
      pps: "#0ea5e9",
      cpu: "#ea580c",
      drop: "#dc2626",
      axis: "#577160",
      grid: "#d9e7dd"
    };

    function drawChart(canvas, points, valueKey, color, formatValue, opts = {}) {
      const ctx = canvas.getContext("2d");
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
      }
      ctx.clearRect(0, 0, w, h);
      const pad = { l: 54, r: 12, t: 12, b: 28 };
      const plotW = w - pad.l - pad.r;
      const plotH = h - pad.t - pad.b;
      if (plotW <= 0 || plotH <= 0) return;

      const values = points.map(p => Number(p[valueKey] || 0));
      const maxVRaw = values.length ? Math.max(...values) : 1;
      let maxV = maxVRaw <= 0 ? 1 : maxVRaw * 1.15;
      if (opts.robustScale && values.length >= 8) {
        const sorted = [...values].sort((a, b) => a - b);
        const p95 = sorted[Math.floor((sorted.length - 1) * 0.95)];
        const latest = values[values.length - 1] || 0;
        const robustMax = Math.max(1, p95 * 1.2, latest * 1.2);
        // Prevent one-off spikes from dominating axis scale.
        if (maxV > robustMax * 1.8) {
          maxV = robustMax;
        }
      }
      const minV = 0;

      ctx.strokeStyle = colors.grid;
      ctx.lineWidth = 1;
      for (let i = 0; i <= 4; i++) {
        const y = pad.t + (plotH * i / 4);
        ctx.beginPath();
        ctx.moveTo(pad.l, y);
        ctx.lineTo(w - pad.r, y);
        ctx.stroke();
      }

      ctx.strokeStyle = colors.axis;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(pad.l, pad.t);
      ctx.lineTo(pad.l, h - pad.b);
      ctx.lineTo(w - pad.r, h - pad.b);
      ctx.stroke();

      ctx.fillStyle = colors.axis;
      ctx.font = "12px 'Noto Sans SC', sans-serif";
      for (let i = 0; i <= 4; i++) {
        const y = pad.t + (plotH * i / 4);
        const v = maxV - (maxV - minV) * i / 4;
        ctx.fillText(formatValue(v), 6, y + 4);
      }

      if (!points.length) return;
      const n = points.length;
      ctx.strokeStyle = color;
      ctx.lineWidth = 2.4;
      ctx.beginPath();
      for (let i = 0; i < n; i++) {
        const x = pad.l + (plotW * i / Math.max(1, n - 1));
        const v = Number(points[i][valueKey] || 0);
        const y = pad.t + (maxV - v) * plotH / (maxV - minV);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    function fmtNum(v) {
      if (v >= 1000000) return (v / 1000000).toFixed(2) + "M";
      if (v >= 1000) return (v / 1000).toFixed(1) + "K";
      return v.toFixed(1);
    }

    async function tick() {
      try {
        const resp = await fetch("/api/state", { cache: "no-store" });
        const data = await resp.json();
        const points = data.points || [];
        const latest = points.length ? points[points.length - 1] : null;

        drawChart(document.getElementById("throughput"), points, "throughput_mbps", colors.throughput, v => v.toFixed(1));
        drawChart(document.getElementById("pps"), points, "packet_rate_pps", colors.pps, v => fmtNum(v));
        drawChart(document.getElementById("cpu"), points, "cpu_percent", colors.cpu, v => v.toFixed(1));
        drawChart(
          document.getElementById("drop"),
          points,
          "drop_count_sec",
          colors.drop,
          v => v.toFixed(3),
          { robustScale: true }
        );

        if (latest) {
          document.getElementById("runTime").textContent = "运行时间: " + Math.round(latest.elapsed_sec) + "s";
          document.getElementById("latestText").textContent =
            "最新: 吞吐 " + latest.throughput_mbps.toFixed(2) + " Mbps, 包率 " + fmtNum(latest.packet_rate_pps) +
            " pps, CPU " + (latest.cpu_percent == null ? "-" : latest.cpu_percent.toFixed(2) + "%") +
            ", 丢包 " + latest.drop_count_sec.toFixed(3) + "/s";
        }
        document.getElementById("statusText").textContent = data.last_error ? "状态: 指标抓取异常" : "状态: 运行中";
        document.getElementById("errText").textContent = data.last_error ? ("错误: " + data.last_error) : "";
      } catch (e) {
        document.getElementById("statusText").textContent = "状态: 面板连接异常";
        document.getElementById("errText").textContent = String(e);
      }
    }

    setInterval(tick, 1000);
    tick();
  </script>
</body>
</html>
"""


def build_handler(state: DashboardState):
    html = dashboard_html().encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/" or self.path == "/index.html":
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(html)
                return
            if self.path == "/api/state":
                payload = json.dumps(state.snapshot(), ensure_ascii=False).encode("utf-8")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(payload)
                return
            self.send_response(HTTPStatus.NOT_FOUND)
            self.end_headers()

        def log_message(self, fmt: str, *args) -> None:  # noqa: A003
            return

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description="Local real-time dashboard for receiver performance.")
    parser.add_argument("--host", default="127.0.0.1", help="Dashboard listen host.")
    parser.add_argument("--port", type=int, default=8090, help="Dashboard listen port.")
    parser.add_argument("--metrics-url", default="http://127.0.0.1:8080/metrics", help="Receiver metrics URL.")
    parser.add_argument("--interval-sec", type=float, default=1.0, help="Polling interval seconds.")
    parser.add_argument("--history-sec", type=int, default=300, help="History window in seconds.")
    parser.add_argument("--packet-size-bytes", type=int, default=1024, help="Packet size used in throughput display.")
    parser.add_argument("--receiver-pid", type=int, default=0, help="Receiver process PID for CPU usage.")
    parser.add_argument("--receiver-proc-name", default="receiver_app", help="Process name for PID auto-detection.")
    args = parser.parse_args()

    receiver_pid = args.receiver_pid if args.receiver_pid > 0 else find_pid_by_name(args.receiver_proc_name)
    cpu_sampler = ProcessCpuSampler(receiver_pid) if receiver_pid else None

    state = DashboardState(max_points=max(30, int(args.history_sec / max(0.2, args.interval_sec))))
    poller = Poller(
        state=state,
        metrics_url=args.metrics_url,
        interval_sec=max(0.2, args.interval_sec),
        packet_size_bytes=args.packet_size_bytes,
        cpu_sampler=cpu_sampler,
    )
    poller.start()

    handler = build_handler(state)
    server = ThreadingHTTPServer((args.host, args.port), handler)

    target = f"http://{args.host}:{args.port}"
    print(f"[dashboard] listening on {target}")
    print(f"[dashboard] metrics source: {args.metrics_url}")
    if receiver_pid:
        print(f"[dashboard] cpu source pid: {receiver_pid}")
    else:
        print("[dashboard] cpu source pid: not found (CPU chart will be empty)")
    print("[dashboard] press Ctrl+C to stop")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        poller.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
