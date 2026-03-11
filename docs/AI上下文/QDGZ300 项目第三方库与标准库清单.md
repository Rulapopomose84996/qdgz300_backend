## QDGZ300 项目第三方库与标准库清单

### 一、C++ 第三方库

| 库 | 版本 | 引入方式 | 用途 |
|---|---|---|---|
| **spdlog** | v1.13.0 | header-only / FetchContent | JSON 格式异步日志、`trace_id` 关联 |
| **yaml-cpp** | v0.8.0 | `find_package` / FetchContent | YAML 配置文件解析（`receiver.yaml` 等） |
| **Google Protocol Buffers** | 3.x | `find_package(Protobuf REQUIRED)` | M04 显控通信序列化（`hmi_protocol.proto`） |
| **fmt** | — | 链接至 `qdgz300_common` | 格式化输出 |
| **nlohmann_json** | — | 链接至 `qdgz300_common` | JSON 序列化（诊断包 / 日志 / 事件） |
| **Boost** | — | `FindBoost.cmake` | 通用工具库 |
| **Google Test** | v1.14.0 | FetchContent（仅测试） | 19 个单元测试套件 |
| **Google Benchmark** | — | FetchContent（仅基准） | 吞吐量 / NUMA / 压力基准测试 |

### 二、GPU SDK

| 组件 | 版本 | 用途 |
|---|---|---|
| **Iluvatar CoreX SDK** | 4.3.8（CUDA 10.2 API 兼容） | GPU 编译器 / 运行时 |
| `IluvatarCorex::Runtime` | — | GPU 运行时库（CMake target） |
| CUDA Streams / Events | — | 3-stream 异步 GPU 流水线 |
| `cudaMalloc` / `cudaHostAlloc` | — | GPU 内存分配 / Pinned Memory 零拷贝 |

### 三、系统库（Linux / POSIX）

| 库 | 链接标志 | 用途 |
|---|---|---|
| **libnuma** | `-lnuma` | `numa_alloc_onnode()` — NUMA 感知内存分配 |
| **pthread** | `-lpthread` | `pthread_setaffinity_np()` — CPU 亲和绑定、线程管理 |
| **librt** | `-lrt` | POSIX 实时扩展（共享内存、高精度时钟） |
| **libdl** | `-ldl` | 动态链接（运行时符号加载） |

### 四、C++17 标准库关键特性

| 特性 | 用途 |
|---|---|
| `std::atomic<T>` + `memory_order_*` | 无锁计数器 / 状态标志 / SPSC 队列头尾指针 |
| `std::optional<T>` | 可空返回值（解析结果等） |
| `std::thread` | 线程创建与管理 |
| `std::function` / `std::bind` | 回调注册（TCP session 工厂、事件处理） |
| `std::shared_ptr` + 自定义 deleter | RCU-style 配置读取 / 对象池 RAII 回收 |
| `std::unordered_map` | 重组上下文映射（自定义哈希） |
| `std::bitset<1024>` | 分片位图追踪 |
| `std::vector` / `std::string` / `std::string_view` | 数据容器 / 轻量引用 |
| `alignas(64)` | 缓存行隔离避免 false sharing |
| `constexpr` / `static_assert` | 编译期常量与协议结构体大小校验 |
| `#pragma pack(push, 1)` | 协议结构紧凑布局（与线格式精确匹配） |

### 五、POSIX / Linux 系统调用与接口

| API | 用途 |
|---|---|
| `recvmmsg()` / `sendmmsg()` | 批量 UDP 收 / 发（batch_size=64） |
| `epoll` | TCP 事件驱动 I/O（M04 Gateway） |
| `SO_REUSEPORT` / `SO_RCVBUF` / `SO_BUSY_POLL` | Socket 选项（负载均衡 / 缓冲 / 忙轮询） |
| `SCHED_FIFO` / `SCHED_OTHER` | 实时线程调度策略（优先级 60–80） |
| POSIX 共享内存 (`shm_open` 等) | 进程间通信 |
| Unix Domain Socket | 进程间通信 |
| `SIGINT` / `SIGTERM` / `SIGHUP` | 信号处理（优雅退出 / 热重载） |

### 六、构建工具链

| 工具 | 版本 | 用途 |
|---|---|---|
| **CMake** | ≥ 3.16 | 构建系统 |
| **Clang** (CoreX SDK 内置) | 18.1.8 | C++17 编译器 |
| `protobuf_generate_cpp()` | — | `.proto` → C++ 自动生成 |
| 编译标志 | `-march=armv8-a+crypto -O3` | ARM64 CRC32C 硬件加速 |

### 七、监控与可观测性

| 组件 | 用途 |
|---|---|
| **Prometheus** text format | 指标导出 `/metrics` 端点（port 8080/9100） |
| **Grafana** | 监控面板（附带 `grafana-dashboard.json`） |

### 八、运维 / 测试辅助工具

| 工具 | 用途 |
|---|---|
| `numactl` / `memhog` | NUMA 亲和性验证与压力测试 |
| `taskset` | CPU 绑核验证 |
| `irqbalance` | NIC 中断亲和设置 |
| `ethtool` / `iperf` / `ping` | 网络诊断 |
| `iptables` / `tc` | 故障注入（网络阻断 / 丢包 / 延迟） |
| `fio` (libaio) | 磁盘 I/O 压力测试 |
| `systemctl` | 服务管理 |
| PTP (`ptp4l`) / NTP | 时间同步服务 |

### 九、V2+ 规划（V1 不纳入）

| 技术 | 预期用途 |
|---|---|
| **AF_XDP** | 内核旁路收包 |
| **DPDK** | 用户态网络栈 |
| **GPU Direct RDMA** | NIC → GPU 直通零拷贝 |
| TLS / mTLS | 传输加密 |

---

**目标平台**：飞腾 S5000C (ARM64 32核) + 天数智芯 MR-V100 GPU，银河麒麟 Linux V10 (Kernel 4.19.90)
