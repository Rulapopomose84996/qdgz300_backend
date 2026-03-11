#!/usr/bin/env bash
# ============================================================================
#  run_fpga_stress_test.sh — FPGA联调 5阶段极限压测脚本
#
#  阶段:
#    1) 单阵面(DACS_01)   5分钟  frame_rate=20
#    2) 双阵面(01+02)     5分钟  frame_rate=20
#    3) 三阵面(01+02+03)  5分钟  frame_rate=20
#    4) 过载测试           2分钟  frame_rate=40 + 1%丢包
#    5) 24小时稳定性       可选（--long 启用，默认跳过）
#
#  用法:
#    ./run_fpga_stress_test.sh [选项]
#
#  选项:
#    --emulator <path>      fpga_emulator 可执行文件路径 (默认: ./build/fpga_emulator)
#    --receiver <path>      receiver_app 可执行文件路径 (默认: ./build/receiver_app)
#    --config <path>        receiver_app 配置文件路径 (默认: ./config/receiver.yaml)
#    --target <ip:port>     目标地址 (默认: 127.0.0.1:9999)
#    --report-dir <dir>     报告输出目录 (默认: ./reports)
#    --long                 启用阶段5 24小时稳定性测试
#    --metrics-url <url>    Prometheus metrics 端点 (默认: http://127.0.0.1:8080/metrics)
#    --skip-receiver        不自动启动 receiver_app（假定已运行）
#    -h, --help             显示帮助
# ============================================================================
set -euo pipefail

# ─── 默认参数 ───
EMULATOR="./build/fpga_emulator"
RECEIVER=""
RECEIVER_CONFIG="./config/receiver.yaml"
TARGET="127.0.0.1:9999"
REPORT_DIR="./reports"
LONG_TEST=false
METRICS_URL="http://127.0.0.1:8080/metrics"
SKIP_RECEIVER=false

# ─── 参数解析 ───
while [[ $# -gt 0 ]]; do
    case "$1" in
        --emulator)     EMULATOR="$2"; shift 2 ;;
        --receiver)     RECEIVER="$2"; shift 2 ;;
        --config)       RECEIVER_CONFIG="$2"; shift 2 ;;
        --target)       TARGET="$2"; shift 2 ;;
        --report-dir)   REPORT_DIR="$2"; shift 2 ;;
        --long)         LONG_TEST=true; shift ;;
        --metrics-url)  METRICS_URL="$2"; shift 2 ;;
        --skip-receiver) SKIP_RECEIVER=true; shift ;;
        -h|--help)
            head -n 30 "$0" | grep -E '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            exit 1
            ;;
    esac
done

TARGET_IP="${TARGET%%:*}"
TARGET_PORT="${TARGET##*:}"

# ─── 工具函数 ───
timestamp() { date '+%Y-%m-%dT%H:%M:%S%z'; }

log() { echo "[$(timestamp)] $*"; }

die() { log "ERROR: $*" >&2; exit 1; }

ensure_dir() { mkdir -p "$1"; }

RECEIVER_PID=""
EMULATOR_PID=""

cleanup() {
    log "清理进程..."
    if [[ -n "$EMULATOR_PID" ]] && kill -0 "$EMULATOR_PID" 2>/dev/null; then
        kill "$EMULATOR_PID" 2>/dev/null || true
        wait "$EMULATOR_PID" 2>/dev/null || true
    fi
    if [[ -n "$RECEIVER_PID" ]] && kill -0 "$RECEIVER_PID" 2>/dev/null; then
        kill "$RECEIVER_PID" 2>/dev/null || true
        wait "$RECEIVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ─── 采集 metrics 快照 ───
collect_metrics() {
    local label="$1"
    local output_file="$2"
    log "采集 metrics 快照: $label"
    if command -v curl &>/dev/null; then
        curl -s --connect-timeout 3 "$METRICS_URL" > "$output_file" 2>/dev/null || \
            echo "# metrics not available" > "$output_file"
    else
        echo "# curl not installed, metrics skipped" > "$output_file"
    fi
}

# ─── 等待 receiver 就绪 ───
wait_receiver_ready() {
    local max_wait=30
    local i=0
    while [[ $i -lt $max_wait ]]; do
        if command -v curl &>/dev/null && curl -s --connect-timeout 1 "$METRICS_URL" >/dev/null 2>&1; then
            log "receiver_app metrics 端点就绪"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log "WARNING: receiver_app metrics 端点在 ${max_wait}s 内未就绪，继续执行..."
    return 0
}

# ─── 运行单个阶段 ───
run_stage() {
    local stage_num="$1"
    local stage_name="$2"
    local arrays="$3"
    local frame_rate="$4"
    local duration="$5"
    local drop_rate="${6:-0.0}"
    local extra_args="${7:-}"

    local stage_dir="$REPORT_DIR/stage${stage_num}"
    ensure_dir "$stage_dir"

    log "═══════════════════════════════════════════"
    log "阶段 $stage_num: $stage_name"
    log "  阵面数=$arrays  帧率=$frame_rate  时长=${duration}s  丢包率=$drop_rate"
    log "═══════════════════════════════════════════"

    # Pre-stage metrics snapshot
    collect_metrics "stage${stage_num}_pre" "$stage_dir/metrics_pre.txt"

    # Run emulator
    local emu_cmd="$EMULATOR --target ${TARGET_IP}:${TARGET_PORT} --arrays $arrays --frame-rate $frame_rate --duration $duration --drop-rate $drop_rate"
    if [[ -n "$extra_args" ]]; then
        emu_cmd="$emu_cmd $extra_args"
    fi

    log "启动: $emu_cmd"
    eval "$emu_cmd" > "$stage_dir/emulator_output.txt" 2>&1 &
    EMULATOR_PID=$!

    # Wait for emulator to finish
    local wait_start
    wait_start=$(date +%s)
    local max_wait=$((duration + 30))  # extra 30s grace

    while kill -0 "$EMULATOR_PID" 2>/dev/null; do
        local now
        now=$(date +%s)
        local elapsed=$((now - wait_start))
        if [[ $elapsed -gt $max_wait ]]; then
            log "WARNING: 模拟器超时，强制终止"
            kill "$EMULATOR_PID" 2>/dev/null || true
            break
        fi
        sleep 5
    done

    wait "$EMULATOR_PID" 2>/dev/null || true
    EMULATOR_PID=""

    # Post-stage metrics snapshot
    collect_metrics "stage${stage_num}_post" "$stage_dir/metrics_post.txt"

    # Copy emulator JSON report if generated
    local json_report="fpga_emulator_report.json"
    if [[ -f "$json_report" ]]; then
        cp "$json_report" "$stage_dir/emulator_report.json"
        log "模拟器JSON报告已保存到 $stage_dir/emulator_report.json"
    fi

    log "阶段 $stage_num 完成"
    echo ""
}

# ─── 生成综合报告 ───
generate_summary() {
    local summary="$REPORT_DIR/summary.txt"
    {
        echo "═══════════════════════════════════════════"
        echo "  FPGA 联调压测综合报告"
        echo "  生成时间: $(timestamp)"
        echo "═══════════════════════════════════════════"
        echo ""

        for stage_dir in "$REPORT_DIR"/stage*; do
            if [[ -d "$stage_dir" ]]; then
                local stage_name
                stage_name=$(basename "$stage_dir")
                echo "--- $stage_name ---"

                # Extract emulator output summary
                if [[ -f "$stage_dir/emulator_output.txt" ]]; then
                    echo "  模拟器输出 (最后10行):"
                    tail -n 10 "$stage_dir/emulator_output.txt" | sed 's/^/    /'
                fi

                # Extract key metrics diff
                if [[ -f "$stage_dir/metrics_pre.txt" ]] && [[ -f "$stage_dir/metrics_post.txt" ]]; then
                    echo "  Metrics快照: pre + post 已保存"
                fi

                if [[ -f "$stage_dir/emulator_report.json" ]]; then
                    echo "  JSON报告: $stage_dir/emulator_report.json"
                fi
                echo ""
            fi
        done
    } > "$summary"
    log "综合报告已生成: $summary"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

log "FPGA 联调极限压测开始"
log "  模拟器: $EMULATOR"
log "  目标: $TARGET"
log "  报告目录: $REPORT_DIR"

# Verify emulator exists
[[ -x "$EMULATOR" ]] || die "模拟器不存在或不可执行: $EMULATOR"

ensure_dir "$REPORT_DIR"

# Start receiver if requested
if [[ "$SKIP_RECEIVER" == false ]] && [[ -n "$RECEIVER" ]]; then
    log "启动 receiver_app: $RECEIVER --config $RECEIVER_CONFIG"
    "$RECEIVER" --config "$RECEIVER_CONFIG" &
    RECEIVER_PID=$!
    wait_receiver_ready
fi

# ─── Stage 1: 单阵面 5分钟 ───
run_stage 1 "单阵面(DACS_01)" 1 20 300

# ─── Stage 2: 双阵面 5分钟 ───
run_stage 2 "双阵面(DACS_01+02)" 2 20 300

# ─── Stage 3: 三阵面 5分钟 ───
run_stage 3 "三阵面(DACS_01+02+03)" 3 20 300

# ─── Stage 4: 过载测试 2分钟 ───
run_stage 4 "过载测试(3阵面+40fps+1%丢包)" 3 40 120 0.01

# ─── Stage 5: 24小时稳定性（可选）───
if [[ "$LONG_TEST" == true ]]; then
    run_stage 5 "24小时稳定性(3阵面+20fps)" 3 20 86400
else
    log "跳过阶段5（24小时稳定性），使用 --long 启用"
fi

# ─── 综合报告 ───
generate_summary

log "FPGA 联调极限压测全部完成"
log "报告目录: $REPORT_DIR"
