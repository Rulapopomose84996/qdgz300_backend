# M01 FPGA联调完整数据面性能基线

更新时间：待填写（首次联调测试后更新）

## 目的

记录 M01 完整数据面（RxStage → Dispatcher → Reassembler → Reorderer → RawBlockAdapter → StubConsumer）在 FPGA 模拟器 CPI 帧模式下的端到端性能基线。

本文档是 P2（FPGA联调验证与极限压测）的交付件，与 `run_fpga_stress_test.sh` 5阶段压测配合使用。

## 测试范围

### 已覆盖组件

| 层级 | 组件 | 说明 |
|------|------|------|
| 网络接收 | UdpReceiver / RxStage | recvmmsg 批量收包 |
| 协议解析 | PacketParser | CommonHeader(32B) 解析、Magic/Version 校验 |
| 分发 | Dispatcher | DATA(0x03) + HEARTBEAT(0x04) 路由 |
| 重组 | Reassembler | 多分片 CPI 重组（最大1024分片） |
| 排序 | Reorderer | 滑动窗口排序（window=512, timeout=50ms） |
| 交付 | RawBlockAdapter → SPSC → StubConsumer | RawBlock 打包入队 |

### 未覆盖

- M02 信号处理（GPU pipeline）
- 控制面（ControlSession / AckTracker）
- PCAP 写盘

## 测试环境

> **首次联调前请更新以下信息**

- 主机：`kds`
- CPU：待填写
- 内存：待填写
- 网卡：待填写（型号 + NUMA node）
- OS：待填写
- 内核参数：参考 `deploy/sysctl/90-qdgz300.conf`
  - `net.core.rmem_default = 67108864`
  - `net.core.rmem_max = 536870912`
  - `net.core.udp_rmem_min = 262144`
  - `net.core.netdev_max_backlog = 250000`
- 编译选项：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`

## 测试协议参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 协议版本 | V3.1 | Magic=0x55AA55AA, Version=0x31 |
| CPI 分片数 | 128 | 默认 `--cpi-frags 128` |
| 分片 payload | 1400B | 默认 `--frag-payload-bytes 1400` |
| 单 CPI 大小 | ~184KB | 128 × (32+40+1400+4) ≈ 188928B |
| 帧率 | 20 fps / 阵面 | `--frame-rate 20` |
| 心跳间隔 | 1000ms | `--heartbeat-ms 1000` |
| 阵面数 | 1 / 2 / 3 | 分阶段递增 |

## 基线指标定义

| 指标 | 定义 | 采集方式 |
|------|------|----------|
| **RX PPS** | 每秒接收报文数 | Prometheus: `qdgz300_socket_packets_received_total` |
| **RX Mbps** | 每秒接收吞吐量 | Prometheus: `qdgz300_socket_bytes_received_total` × 8 / 时间 |
| **CPI 交付率** | 成功交付的 RawBlock 数 / 发送的 CPI 数 | StubConsumer stats vs emulator report |
| **重组超时率** | Reassembler timeout 次数 / 总 CPI 数 | Prometheus: `qdgz300_reasm_timeout_total` |
| **丢包率** | 丢弃报文数 / 收到报文数 | Prometheus: drop reason counters |
| **端到端延迟** | CPI 首包到达 → RawBlock 入队的时间差 | 日志时间戳差异（抽样） |
| **CPU 占用** | 各工作线程 CPU 使用率 | `top` / `htop` / `pidstat` |

## 压测结果

### 阶段 1: 单阵面 (DACS_01, 5分钟)

| 指标 | 值 | 备注 |
|------|-----|------|
| TX CPI/s | 待填写 | |
| RX PPS | 待填写 | |
| RX Mbps | 待填写 | |
| CPI 交付数 | 待填写 | |
| CPI 交付率 | 待填写 | |
| 重组超时 | 待填写 | |
| CPU (接收线程) | 待填写 | |
| CPU (消费线程) | 待填写 | |

### 阶段 2: 双阵面 (DACS_01 + DACS_02, 5分钟)

| 指标 | 值 | 备注 |
|------|-----|------|
| TX CPI/s (总) | 待填写 | |
| RX PPS | 待填写 | |
| RX Mbps | 待填写 | |
| CPI 交付数 | 待填写 | |
| CPI 交付率 | 待填写 | |
| 重组超时 | 待填写 | |
| CPU (max) | 待填写 | |

### 阶段 3: 三阵面 (全量, 5分钟)

| 指标 | 值 | 备注 |
|------|-----|------|
| TX CPI/s (总) | 待填写 | |
| RX PPS | 待填写 | |
| RX Mbps | 待填写 | |
| CPI 交付数 | 待填写 | |
| CPI 交付率 | 待填写 | |
| 重组超时 | 待填写 | |
| CPU (max) | 待填写 | |

### 阶段 4: 过载测试 (3阵面, 40fps, 1%丢包, 2分钟)

| 指标 | 值 | 备注 |
|------|-----|------|
| TX CPI/s (总) | 待填写 | |
| RX PPS | 待填写 | |
| CPI 交付数 | 待填写 | |
| CPI 交付率 | 待填写 | 预期低于100% |
| 重组超时 | 待填写 | |
| 丢包率 (注入) | 1% | |
| 丢包率 (实测) | 待填写 | |

### 阶段 5: 24小时稳定性 (可选)

| 指标 | 值 | 备注 |
|------|-----|------|
| 总运行时间 | 待填写 | |
| 总 CPI 发送 | 待填写 | |
| 总 CPI 交付 | 待填写 | |
| 交付率 | 待填写 | |
| 内存泄漏 | 待填写 | RSS 变化 |

## 已知瓶颈与优化建议

> 首次联调后根据实测数据补充

1. **待确认**: recvmmsg batch size (当前 64) 是否需要增大
2. **待确认**: SPSC queue capacity (RxStage=8192, RawBlock=64) 是否匹配帧率
3. **待确认**: Reassembler 内存池 NUMA 对齐效果
4. **待确认**: 多阵面场景下 CPU affinity 分配策略

## 与 P1 基线对比

| 指标 | P1 基线 (轻量级) | P2 基线 (完整数据面) | 变化 |
|------|-------------------|----------------------|------|
| 最大 RX PPS | 参考 `m01单阵面轻量接收性能基线.md` | 待填写 | — |
| CPU 占用 (单核) | 待填写 | 待填写 | — |
| 端到端延迟 | N/A | 待填写 | — |

## 附录

### 复现命令

```bash
# 阶段1~4 自动化
cd /path/to/qdgz300_backend
./tools/run_fpga_stress_test.sh \
    --emulator ./build/fpga_emulator \
    --target 192.168.1.101:9999 \
    --report-dir ./reports/p2_baseline

# 手动单阶段示例
./build/fpga_emulator \
    --target 192.168.1.101:9999 \
    --arrays 3 \
    --frame-rate 20 \
    --cpi-frags 128 \
    --duration 300
```

### 相关文档

- [M01 单阵面轻量接收性能基线](m01单阵面轻量接收性能基线.md)
- [M01 性能优化方案](m01性能优化方案.md)
- [系统侧调优检查清单](../系统侧调优检查清单.md)
