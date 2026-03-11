# QDGZ300 后端系统

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Version](https://img.shields.io/badge/version-1.0.0-blue)]()
[![Protocol](https://img.shields.io/badge/protocol-V3.1-orange)]()
[![C++](https://img.shields.io/badge/C++-17-blue)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20ARM64-lightgrey)]()

> **中国铁塔低成本两维相控阵雷达（2025 定制版）信号处理与态势生成后端系统**

> 文档口径说明：仓库内同时存在“已实现代码”和“规划性设计文档”。若两者冲突，以当前代码与 `CMakeLists.txt` 中实际纳入构建的内容为准。

## 📋 概述

QDGZ300 是一个高性能实时雷达信号处理系统，专为低空无人机、地面人员及车辆的探测与监视而设计。系统通过对雷达前端（QDGZ100）采集的高速原始数据进行实时信号处理、数据处理与融合，输出可信、可操作的目标航迹态势（TrackData）。

### 核心使命

> **在严格的实时约束下，持续输出可信、可操作的目标航迹态势（TrackData）**

### 关键特性

- **高性能实时处理**：零锁热路径设计，NUMA 感知内存管理
- **多阵面融合**：同时处理三个阵面雷达数据并进行跨阵面融合
- **GPU 加速**：基于天数智芯 CoreX SDK 的 GPU 信号处理
- **可靠性保障**：完整的状态机管理、健康检查和故障降级机制
- **协议标准化**：完整实现 V3.1 前端感知通信协议
- **可观测性**：Prometheus 指标导出、结构化日志、诊断中心

### 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         QDGZ300 后端系统                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐      │
│  │  DACS_1      │   │  DACS_2      │   │  DACS_3      │      │
│  │  阵面1前端   │   │  阵面2前端   │   │  阵面3前端   │      │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘      │
│         │ UDP                │ UDP              │ UDP          │
│         ▼                    ▼                  ▼              │
│  ┌──────────────────────────────────────────────────────┐     │
│  │             M01 数据接收模块                          │     │
│  │  UDP接收 → RxStage → SPSC → 分发/重组/重排/投递      │     │
│  └──────────────────────┬───────────────────────────────┘     │
│                         │ OrderedPacket / Delivery            │
│                         ▼                                      │
│  ┌──────────────────────────────────────────────────────┐     │
│  │             M02 GPU 信号处理模块                      │     │
│  │  GPU调度 → FFT → CFAR → MTD → 点迹检测              │     │
│  └──────────────────────┬───────────────────────────────┘     │
│                         │ PlotBatch (SPSC)                     │
│                         ▼                                      │
│  ┌──────────────────────────────────────────────────────┐     │
│  │             M03 航迹处理模块                          │     │
│  │  点航关联 → Kalman滤波 → 跨阵面融合 → 航迹维护      │     │
│  └──────────────────────┬───────────────────────────────┘     │
│                         │ TrackFrame                           │
│                         ▼                                      │
│  ┌──────────────────────────────────────────────────────┐     │
│  │             M04 网关模块                              │     │
│  │  TCP控制面 + UDP数据推送 → 显控终端                  │     │
│  └──────────────────────────────────────────────────────┘     │
│                                                                 │
│  ┌──────────────────────────────────────────────────────┐     │
│  │             编排器 Orchestrator                       │     │
│  │  5-State FSM | 事件分发 | 命令桥接 | 配置管理        │     │
│  └──────────────────────────────────────────────────────┘     │
│                                                                 │
│  健康检查 | 时间同步 | 日志 | 指标导出 | 诊断中心 | 录制器    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 🏗️ 系统模块

### M01 数据接收模块

高性能 UDP 数据接收模块，负责从三个阵面雷达前端接收原始数据。

**核心特性**：
- **三线程分立模型**：每个阵面独立接收线程（CPU 16/17/18）
- **V3.1 协议解析**：32 字节通用头解析、Magic/Version/CRC32 校验
- **分片重组**：应用层分片重组引擎（T_reasm=100ms）
- **序列号重排序**：滑动窗口重排序（window_size=512, T_reorder=50ms）
- **零锁设计**：Busy-Poll + SPSC 无锁队列 + std::atomic
- **NUMA 亲和**：接收线程绑定 NUMA Node1

**当前代码数据流**：
```
DACS → UdpReceiver → RxStage(parse/validate/enqueue) → SPSC<RxEnvelope>
     → processing thread → Dispatcher → Reassembler → Reorderer → DeliveryInterface
```

📖 详细文档：[M01 Receiver 设计文档](docs/项目源代码文档/m01_receiver.md)

### M02 GPU 信号处理模块

基于天数智芯 CoreX SDK 的 GPU 加速信号处理模块。

**核心功能**：
- **GPU 调度**：Round-Robin 三路 CUDA Stream 调度
- **信号处理流水线**：FFT → CFAR → MTD
- **点迹检测**：从信号处理结果中提取目标点迹
- **内存管理**：Pinned Memory 双缓冲池，NUMA Node1 分配

**状态**：⚡ **CPU Fallback 可用** - 已具备 CPU fallback 调度、点迹输出与测试链路

### M03 航迹处理模块

目标跟踪与多阵面融合模块，系统的核心业务逻辑。

**核心功能**：
- **单阵面跟踪**：Kalman 滤波器（预测→测量更新）
- **点航关联**：最近邻 / 全局最近邻（GNN）算法
- **跨阵面融合**：ID 继承、航迹合并、接力机制
- **航迹管理**：5 态 FSM（TENT → CONF → COAST → LOST → DEL）
- **TrackFrame 生成**：全量快照 + 质量标志 + 覆盖位图

**状态**：🚧 **规划中**

### M04 网关模块

显控终端通信网关，负责输出处理结果。

**核心功能**：
- **TCP 控制面**：epoll 驱动的连接管理
- **UDP 数据推送**：TrackData 实时推送（≤1200B 分片）
- **会话管理**：Hello → Welcome → StateSnapshot 握手流程
- **流控机制**：4 态流控（NORMAL/CONGESTED/BLOCKED/OFFLINE）
- **Protobuf 序列化**：TrackFrame / Command / StateSnapshot

**状态**：🚧 **规划中**

### 编排器 (Orchestrator)

系统级编排与状态管理核心。

**核心功能**：
- **5-State FSM**：BOOT → STANDBY → WARMUP → RUNNING → DEGRADED
- **事件分发**：MPSC 事件队列消费 + 去重节流
- **命令桥接**：HMI ↔ DACS 幂等命令传递 + ACK 聚合
- **配置管理**：版本化快照 + RCU-style 无锁读

**状态**：🚧 **规划中**

### 支撑模块

- **健康检查**：Boot 检查（6 项）+ Runtime 监控（100ms 周期）
- **时间同步**：4 态 FSM（UNSYNC/SYNCED/HOLDOVER/EXPIRED）
- **日志系统**：spdlog 异步日志，JSON 格式，按模块分级
- **指标导出**：Prometheus text format，100+ 指标（D-M-G/H/S 系列）
- **诊断中心**：事件 Ring Buffer + JSON 诊断包生成
- **录制器**：异步落盘 + 磁盘配额管理

## 🛠️ 技术栈

| 类别 | 技术 |
|------|------|
| **语言** | C++17 |
| **编译器** | Clang 18 / GCC 11+ |
| **目标平台** | Linux ARM64 (aarch64) / 银河麒麟 Kernel 4.19 |
| **构建工具** | CMake ≥ 3.16 |
| **GPU 加速** | 天数智芯 CoreX SDK 4.3.8 (CUDA-compatible) |
| **日志** | spdlog (异步 JSON 日志) |
| **序列化** | Protocol Buffers 3.x |
| **网络** | Linux Socket API (recvmmsg, epoll) |
| **测试框架** | Google Test |
| **性能测试** | Google Benchmark |
| **时间同步** | PTP / NTP |
| **监控** | Prometheus + Grafana |

## 📦 依赖项

### 必需依赖

- **spdlog** ≥ 1.10：异步日志
- **yaml-cpp** ≥ 0.7：配置文件解析
- **Boost** ≥ 1.74：Boost.Asio（网络）、Boost.Circular_Buffer 等
- **Google Test** ≥ 1.12（仅测试）

### 可选依赖

- **Iluvatar CoreX SDK** 4.3.8：GPU 信号处理（需要 `-DENABLE_GPU=ON`）
- **Protobuf** ≥ 3.12：M04 Gateway 模块（需要 `-DENABLE_PROTOBUF=ON`）
- **Google Benchmark**：性能基准测试

### 离线依赖支持

项目提供完整的离线依赖包（`deps_offline/`），用于无网络环境的构建：

```bash
cd deps_offline
./extract_all.sh
```

## 🚀 快速开始

### 环境要求

- **操作系统**：Linux (推荐银河麒麟 v10 / Ubuntu 20.04+)
- **架构**：ARM64 (aarch64)
- **编译器**：Clang 18 / GCC 11+
- **CMake**：≥ 3.16
- **Python**：≥ 3.8（用于工具脚本）

### 1. 克隆仓库

```bash
git clone <repository_url>
cd qdgz300_backend
```

### 2. 构建

#### 开发模式（自动下载依赖）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
```

#### 生产模式（离线构建，推荐）

```bash
# 首次使用需解压离线依赖
cd deps_offline
chmod +x extract_all.sh
./extract_all.sh
cd ..

# 完全离线编译（禁止网络下载）
cmake -B build \
    -DFORCE_OFFLINE_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DENABLE_GPU=OFF \
    -DENABLE_PROTOBUF=OFF
cmake --build build -j$(nproc)
```

#### 启用 GPU 支持

```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_GPU=ON \
    -DIluvatarCorex_ROOT=/path/to/corex_sdk
cmake --build build -j$(nproc)
```

#### WSL 交叉编译（ARM64）

在 WSL 2 环境下进行 ARM64 交叉编译：

```bash
# 安装交叉编译工具链
sudo apt-get update
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# 解压离线依赖
cd deps_offline && ./extract_all.sh && cd ..

# 交叉编译
cmake -B build_wsl_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake \
    -DFORCE_OFFLINE_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build_wsl_arm64 -j$(nproc)
```

📖 详细指南：[WSL交叉编译指南](docs/自用指南/WSL交叉编译指南.md)

### 3. 运行

#### M01 接收器（当前可用）

```bash
# 使用默认配置
./build/receiver_app --config config/receiver.yaml

# 查看帮助
./build/receiver_app --help
```

#### 完整系统（开发中）

```bash
./build/qdgz300_backend --config configs/default.yaml
```

### 4. 测试

```bash
cd build
ctest --output-on-failure

# 运行特定测试
./tests/common_spsc_queue_tests
./tests/integration_tests_m01

# 性能基准测试
./benches/throughput
./benches/stress_test
```

### 5. 监控（可选）

系统内置 Prometheus 指标导出和 Grafana 面板：

```bash
# 启动性能监控面板
python3 tools/receiver_perf_dashboard.py
```

📖 详细说明：[本地性能可视化面板使用说明](docs/自用指南/本地性能可视化面板使用说明.md)

## 🧪 性能测试

项目提供完整的性能测试工具集：

```bash
# 吞吐量测试
./benches/throughput

# 压力测试
./benches/stress_test

# NUMA 亲和性测试
./benches/numa_affinity

# 容量测试
./benches/capacity_test

# FPGA 接收限速测试
./benches/fpga_rx_limit

# 标准化基准测试（自动化）
./tools/run_receiver_standard_bench.sh
```

📖 详细教程：[雷达接收模块压测保姆级教程](docs/自用指南/雷达接收模块压测保姆级教程.md)

## 📂 项目结构

```
qdgz300_backend/
├── include/qdgz300/       # 公共头文件
│   ├── common/            # 全局公共组件（types, SPSC队列, 内存池等）
│   ├── m01_receiver/      # M01 数据接收模块
│   ├── m02_signal_proc/   # M02 GPU 信号处理模块
│   ├── m03_data_proc/     # M03 航迹处理模块（规划中）
│   ├── m04_gateway/       # M04 网关模块（规划中）
│   ├── orchestrator/      # 编排器（规划中）
│   ├── config/            # 配置管理
│   ├── health/            # 健康检查
│   ├── time_sync/         # 时间同步
│   ├── diagnostics/       # 诊断中心
│   └── logging/           # 日志与指标导出
├── src/                   # 实现代码（与 include 对应）
├── tests/                 # 单元测试和集成测试
├── benches/               # 性能基准测试
├── tools/                 # 工具脚本（PCAP 分析、性能监控等）
├── config/                # 配置文件模板
├── configs/               # 默认配置
├── docs/                  # 项目文档
│   ├── 设计文档/          # 顶层架构设计
│   ├── 项目源代码文档/    # 模块详细设计
│   ├── 任务规划/          # 开发规划
│   ├── 自用指南/          # 使用指南
│   └── AI上下文/          # AI 辅助开发资料
├── proto/                 # Protobuf 定义
├── cmake/                 # CMake 脚本
└── deps_offline/          # 离线依赖包
```

## 📖 文档

### 设计文档

- [00_顶层架构](docs/设计文档/00_顶层架构.md) - 系统全局架构
- [顶层设计系列](docs/设计文档/) - 详细设计文档（1-8）
- [全系统项目骨架文档](docs/项目源代码文档/全系统项目骨架文档.md) - 代码结构规范
- [M01 Receiver 设计](docs/项目源代码文档/m01_receiver.md) - 接收模块详细设计

### 协议文档

- [前端感知通信协议 v3.1](docs/冻结的设计资产/前端感知通信协议v3.1.md) - 权威协议定义
- [SECTION 系列文档](docs/冻结的设计资产/) - 协议详细规范

### 使用指南

- [WSL交叉编译指南](docs/自用指南/WSL交叉编译指南.md)
- [雷达接收模块压测保姆级教程](docs/自用指南/雷达接收模块压测保姆级教程.md)
- [本地性能可视化面板使用说明](docs/自用指南/本地性能可视化面板使用说明.md)

### API 文档

```bash
# 生成 Doxygen 文档（待实现）
# doxygen Doxyfile
```

## 🗺️ 项目状态与路线图

### 当前状态（v1.0.0）

| 模块 | 状态 | 进度 |
|------|------|------|
| M01 数据接收 | ✅ **已完成** | 100% - 生产就绪 |
| M02 GPU 信号处理 | ⚡ **CPU Fallback 可用** | 60% - CPU fallback + 调度/测试链路已打通 |
| M03 航迹处理 | 📋 **规划中** | 10% - 接口定义 |
| M04 网关 | 📋 **规划中** | 5% - 协议定义 |
| 编排器 | 📋 **规划中** | 15% - FSM 定义 |
| 配置管理 | ✅ **已完成** | 90% - 核心功能完成 |
| 健康检查 | 🚧 **开发中** | 50% - Boot 检查完成 |
| 日志系统 | ✅ **已完成** | 100% |
| 指标导出 | ✅ **已完成** | 90% |

### 里程碑

- **Phase 1 - M01 接收器** ✅ (已完成)
  - UDP 接收 + 协议解析
  - 分片重组 + 序列号重排序
  - NUMA 亲和 + 零锁设计
  - 完整单元测试 + 性能测试

- **Phase 2 - GPU 信号处理** 🚧 (进行中)
  - CoreX SDK 集成
  - Signal Kernels (FFT/CFAR/MTD)
  - 点迹检测与输出

- **Phase 3 - 航迹处理与融合** 📋 (计划)
  - Kalman 滤波器
  - 点航关联
  - 跨阵面融合
  - TrackFrame 生成

- **Phase 4 - 系统集成** 📋 (计划)
  - 网关模块
  - 编排器
  - 端到端集成测试
  - 完整系统验证

## 🤝 贡献指南

### 代码规范

- **C++ 标准**：严格遵循 C++17 标准
- **命名规范**：
  - 类名：PascalCase（`UdpReceiver`）
  - 函数/变量：snake_case（`parse_packet`）
  - 常量：UPPER_SNAKE_CASE（`MAX_PACKET_SIZE`）
  - 成员变量：snake_case_ 后缀（`buffer_size_`）
- **注释**：使用 Doxygen 格式注释公共 API
- **格式化**：使用 clang-format（配置见 `.clang-format`）

### 提交规范

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Type**：
- `feat`: 新功能
- `fix`: Bug 修复
- `docs`: 文档更新
- `style`: 代码格式（不影响功能）
- `refactor`: 重构
- `perf`: 性能优化
- `test`: 测试相关
- `chore`: 构建/工具链相关

**Scope**：
- `m01`: M01 接收模块
- `m02`: M02 信号处理
- `m03`: M03 航迹处理
- `m04`: M04 网关
- `common`: 公共组件
- `build`: 构建系统

### 开发流程

1. Fork 项目
2. 创建特性分支（`git checkout -b feat/amazing-feature`）
3. 提交更改（`git commit -m 'feat(m01): add amazing feature'`）
4. 推送到分支（`git push origin feat/amazing-feature`）
5. 创建 Pull Request

## 📄 许可证

本项目为中国铁塔内部项目，版权所有 © 2025-2026 中国铁塔股份有限公司。

## 📞 联系方式

- **项目负责人**：[待补充]
- **技术支持**：[待补充]
- **文档维护**：[待补充]

## 🙏 致谢

- 天数智芯 CoreX SDK 团队
- 银河麒麟操作系统团队
- 所有开源项目贡献者

---

**最后更新**：2026-03-05
**文档版本**：v1.0.0
