# m01_receiver 代码实况文档

> 更新时间：2026-03-10
> 依据范围：`src/m01_receiver/`、`include/qdgz300/m01_receiver/`、`config/receiver.yaml`
> 约束：本文仅描述当前代码已经实现的行为。若与其他设计稿冲突，以代码为准。

## 1. 模块定位

`m01_receiver` 是当前仓库里已经落地的接收应用模块，职责边界是：

1. 从 1 路或 3 路 UDP 阵面入口收包。
2. 解析并校验协议通用头。
3. 通过 `RxStage` 将热路径和后续处理层解耦。
4. 按 `PacketType` 分发 DATA / HEARTBEAT。
5. 对 DATA 做分片重组，再按序列号重排。
6. 将重排后的 `OrderedPacket` 投递给 `DeliveryInterface`。
7. 提供日志、指标、抓包、配置热加载和优雅关闭。

不在当前模块内实现的内容：

- 不做 M02 GPU 信号处理。
- 不做 M03 航迹处理。
- 不做 M04 网关协议编码。
- 不做系统级 Orchestrator 状态机。

## 2. 当前真实数据流

当前代码不是“收包线程直接跑完整管线”，而是两层解耦结构：

```text
ArrayFaceReceiver / UdpReceiver
  -> RxStage::on_packet()
  -> SPSCQueue<RxEnvelope>
  -> processing thread
  -> Dispatcher
  -> Reassembler
  -> Reorderer
  -> DeliveryInterface
```

其中：

- 收包热路径只做 `parse -> validate -> fill RxEnvelope -> SPSC push`。
- 后续处理层由每阵面一个 processing thread 消费 `RxEnvelope`。
- `DeliveryInterface` 是多阵面共享资源，调用时由 `delivery_mutex` 保护。

## 3. 启动与运行时结构

### 3.1 初始化入口

`src/m01_receiver/app_init.cpp` 当前初始化顺序为：

1. `ConfigManager::load_from_file()`
2. `Logger::initialize()`
3. `MetricsCollector::initialize()`
4. 创建 `DeliveryInterface`
5. 解析阵面拓扑，构造 `FacePipeline`
6. 对每个阵面创建：
   - `Reorderer`
   - `Reassembler`
   - `Dispatcher`
   - `RxStage`
7. 可选创建 `PcapWriter`
8. 创建 `UdpReceiver`，其回调仅调用 `RxStage::on_packet()`

### 3.2 运行入口

`src/m01_receiver/app_run.cpp` 当前行为：

- 支持 `dry-run`
- 启动 metrics HTTP 端点
- 启动 `UdpReceiver`
- 为每个阵面启动 1 个 processing thread
- 主线程每 100ms 执行：
  - 信号计数上报
  - `SIGHUP` 热加载
  - `Reassembler::check_timeouts()`
  - `Reorderer::check_timeout()`
  - 聚合重组/重排统计
  - 每 5 秒采集一次系统指标

### 3.3 优雅关闭

`app_shutdown()` 当前逆序关闭为：

1. `UdpReceiver::stop()`
2. 停止并 `join` processing threads
3. 停止 `PcapWriter`
4. `Reassembler::flush_all()`
5. `Reorderer::flush()`
6. `DeliveryInterface::flush()`
7. `MetricsCollector::stop()`

## 4. 核心实现细节

### 4.1 网络层

当前网络层由以下组件组成：

- `UdpReceiver`
- `ArrayFaceReceiver`
- `PacketPool`

代码体现出的行为：

- 支持三阵面配置，也支持单阵面 fallback。
- 三阵面模式要求 `bind_ips`、`source_id_map`、`cpu_affinity_map` 同时为 3 项。
- 每个阵面独立绑定 IP、监听相同端口、绑定独立 CPU。
- 支持 `source_filter_enabled` 进行 `SourceID` 兜底过滤。
- 支持可热替换的 `capture_hook`。

### 4.2 RxStage

`RxStage` 是当前实现里最关键的分层边界。

接口：

```cpp
void on_packet(network::ReceivedPacket &&raw_packet);
Stats get_stats() const noexcept;
```

热路径固定为：

```text
parse -> validate -> build RxEnvelope -> queue_.drop_oldest_push()
```

已确认事实：

- `RxStage` 内部自带 `PacketParser` 和 `Validator`。
- `Validator` 当前使用 `Scope::DATA_AND_HEARTBEAT`。
- `RxEnvelope` 携带：
  - `protocol::CommonHeader`
  - `array_id`
  - `rx_timestamp_ns`
  - `packet_length`
  - `network::PacketBuffer`
- 队列溢出策略是 `drop_oldest_push`，不是阻塞等待。

### 4.3 Dispatcher

当前 `Dispatcher` 既保留了优先队列相关代码，也保留了同步直通路径；但实际主路径是同步直通。

当前真实入口：

```cpp
void dispatch(const protocol::ParsedPacket &packet);
void dispatch(const protocol::ParsedPacket &packet, network::PacketBuffer &&buffer);
```

当前运行路径中：

- DATA 走直通 handler。
- HEARTBEAT 走 `dispatch_heartbeat_direct()`。
- `dispatch_with_priority()` 目前只是调用 `dispatch()`。
- processing thread 中对 DATA 调用的是带 `PacketBuffer` 所有权的零拷贝路径。

因此，对 AI 的正确表述应是：

- “代码里存在优先队列相关实现”，而不是“当前主流程一定经过优先级队列”。

### 4.4 Reassembler

当前 `Reassembler` 的真实公开接口为：

```cpp
void process_packet(const protocol::ParsedPacket &packet);
void process_packet_zero_copy(const protocol::ParsedPacket &packet,
                              network::PacketBuffer &&buffer);
void check_timeouts();
void set_timeout_ms(uint32_t timeout_ms);
size_t flush_all();
```

关键事实：

- 重组键不是简化版三元组。
- 真实 `ReassemblyKey` 包含：
  - `control_epoch`
  - `source_id`
  - `frame_counter`
  - `beam_id`
  - `cpi_count`
  - `pulse_index`
  - `channel_mask`
  - `data_type`
- 支持零拷贝分片缓存，最终在完成重组时一次性顺序拷贝。
- 支持超时输出不完整帧。
- 支持 frozen key 机制处理超时后的迟到分片。

### 4.5 Reorderer

当前 `Reorderer` 的真实公开接口为：

```cpp
void insert(const protocol::ParsedPacket &packet);
void insert_owned(protocol::CommonHeader header,
                  std::unique_ptr<uint8_t[]> payload,
                  size_t payload_size);
void check_timeout();
void set_timeout_ms(uint32_t timeout_ms);
size_t flush();
Statistics get_statistics() const;
```

当前落地行为：

- `Reassembler` 回调里调用的是 `insert_owned(...)`。
- 输出类型是 `OrderedPacket`，核心字段为：
  - `packet`
  - `owned_payload`
  - `payload_size`
  - `is_zero_filled`
  - `sequence_number`

### 4.6 Delivery

当前投递抽象为 `delivery::DeliveryInterface`：

```cpp
virtual bool deliver(const pipeline::OrderedPacket &packet) = 0;
virtual void flush() = 0;
virtual Statistics get_statistics() const = 0;
```

当前代码中实际可创建的实现有：

- `CallbackDelivery`
- `SharedMemoryDelivery`
- `UnixSocketDelivery`

`app_init.cpp` 中根据 `config.delivery.method` 选择实现，未匹配时回退到 `CallbackDelivery`。

### 4.7 Monitoring / Capture / Reload

当前代码已实现：

- `MetricsCollector` 初始化、启动、停止
- 系统指标周期采集
- 信号量计数上报
- 重组/重排统计增量聚合
- `PcapWriter` 启停与热重载替换
- `ConfigManager::register_reload_callback(...)`

当前热加载明确调整的内容只有：

- 日志级别
- `Reassembler` 超时
- `Reorderer` 超时
- 抓包配置

## 5. 关键配置口径

从 `config_manager.h/.cpp` 和 `config/receiver.yaml` 可确认：

- `network`
- `reassembly`
- `reorder`
- `timestamp`
- `control_reliability`
- `flow_control`
- `logging`
- `monitoring`
- `performance`
- `delivery`
- `capture`
- `queue`
- `consumer`

这些配置结构都在 `ReceiverConfig` 中存在。

但需要特别注意：

- 当前主流程真正使用到的配置项比 `ReceiverConfig` 中定义的少。
- `queue.rawcpi_q_capacity`、`queue.rawcpi_q_slot_size_mb` 目前存在于配置结构中，但本文档不将其描述为已接入当前 `RxStage` 队列实现，除非代码显式使用。
- 若某设计文档声称“某子系统已移除”，应再次对照当前 `app_init.cpp`、`app_run.cpp` 和 `config_manager.cpp`，因为现在代码里 `monitoring`、`delivery`、`capture` 都是已接入的。

## 6. 对 AI 最重要的统一口径

为了避免 AI 基于旧设计稿产生错误推断，当前 M01 应固定理解为：

1. 已实现的是接收应用，不是只停留在接口草图。
2. 当前真实主链路是 `UdpReceiver -> RxStage -> SPSC -> processing thread -> Dispatcher -> Reassembler -> Reorderer -> DeliveryInterface`。
3. `RxEnvelope` 是热路径和后续处理层之间的唯一队列载体。
4. `ReassemblyKey` 是扩展键，不是简单的 `(SourceID, ControlEpoch, FrameCounter)`。
5. Dispatcher 的“优先级队列”代码存在，但当前主路径以同步直通和 heartbeat 直接处理为主。
6. 当前项目确实已经接入 `metrics`、`pcap capture`、`config reload`、`delivery`，不应按“已移除”理解。

## 7. 建议作为 AI 上下文的配套文件

若要让 AI 基于当前实现继续做任务拆解，建议同时提供：

- `src/m01_receiver/app_init.cpp`
- `src/m01_receiver/app_run.cpp`
- `src/m01_receiver/pipeline/rx_stage.cpp`
- `include/qdgz300/m01_receiver/pipeline/rx_envelope.h`
- `include/qdgz300/m01_receiver/pipeline/dispatcher.h`
- `include/qdgz300/m01_receiver/pipeline/reassembler.h`
- `include/qdgz300/m01_receiver/pipeline/reorderer.h`
- `include/qdgz300/m01_receiver/protocol/protocol_types.h`
- `include/qdgz300/m01_receiver/config/config_manager.h`
- `config/receiver.yaml`

以上组合足以让 AI 以代码为唯一真实依据理解 M01 当前状态。
