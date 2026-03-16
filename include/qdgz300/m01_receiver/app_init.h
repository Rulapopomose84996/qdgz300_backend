/**
 * @file app_init.h
 * @brief M01 接收器应用程序初始化模块——声明启动选项、运行时上下文及初始化入口
 *
 * 本头文件定义了接收器从启动到就绪的核心数据结构：
 * - AppOptions  : 外部传入的启动参数（配置文件路径、dry-run 开关）
 * - AppContext   : 运行期间所有组件（网络层→协议层→流水线→投递层）的持有容器
 * - app_init()   : 一次性初始化函数，读取配置并构造全部子系统
 *
 * 架构概览（数据流向）：
 * @code
 *  FPGA / 模拟器
 *       │  UDP 报文
 *       ▼
 *  UdpReceiver  ──▶  PacketParser  ──▶  Validator
 *       │                                    │
 *       │                              (丢弃无效包)
 *       │                                    │ OK
 *       ▼                                    ▼
 *  CaptureHook(pcap)                   Dispatcher
 *                                      ├─ DATA      ──▶ Reassembler ──▶ Reorderer ──▶ DeliveryInterface
 *                                      └─ HEARTBEAT ──▶ MetricsCollector
 * @endcode
 *
 * @note 本文件仅提供声明；实现位于 src/m01_receiver/app_init.cpp。
 * @see app_run.h   主事件循环 & 优雅关闭
 * @see main.cpp    应用程序入口
 */

#ifndef RECEIVER_APP_INIT_H
#define RECEIVER_APP_INIT_H

// ── 项目内部依赖（按数据流从底层到上层排列） ──────────────────────────
#include "qdgz300/m01_receiver/capture/pcap_writer.h"         // 原始报文落盘（pcap 格式）
#include "qdgz300/m01_receiver/delivery/delivery_interface.h" // 上层数据投递接口（共享内存 / Unix Socket / 回调）
#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"   // OrderedPacket -> RawBlock 适配
#include "qdgz300/m01_receiver/delivery/stub_consumer.h"      // RawBlock 消费线程
#include "qdgz300/m01_receiver/network/udp_receiver.h"        // 高性能 UDP 批量接收（recvmmsg）
#include "qdgz300/m01_receiver/pipeline/dispatcher.h"         // 按报文类型 (DATA / HEARTBEAT) 分发
#include "qdgz300/m01_receiver/pipeline/reassembler.h"        // 分片重组：将多片 DATA 拼成完整帧
#include "qdgz300/m01_receiver/pipeline/reorderer.h"          // 序号重排序 + 超时零填充
#include "qdgz300/m01_receiver/pipeline/rx_stage.h"           // 实时接收层（parse + validate + SPSC enqueue）
#include "qdgz300/m01_receiver/protocol/packet_parser.h"      // 二进制报文解析（CommonHeader + payload）
#include "qdgz300/m01_receiver/protocol/validator.h"          // 报文合法性校验（magic / 版本 / 目标 ID 等）

// ── 标准库 ───────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace receiver
{
    /**
     * @struct AppOptions
     * @brief 应用程序启动选项（由命令行解析后传入）
     *
     * 这是一个纯值类型结构体，代表从外部（CLI / 测试用例）传入的启动参数。
     * app_init() 会根据这些选项加载配置文件并决定是否进入 dry-run 模式。
     */
    struct AppOptions
    {
        std::string config_file{"config/receiver.yaml"}; ///< YAML 配置文件路径（绝对或相对于 CWD）
        bool dry_run{false};                             ///< true = 仅加载配置并打印摘要，不启动网络监听
    };

    /**
     * @struct FacePipeline
     * @brief 单阵面独立的处理管线实例
     *
     * 分层设计：
     * - 实时接收层 (rx_stage)：运行在 CPU 16/17/18，仅做 parse → validate → SPSC push
     * - 后续处理层 (dispatcher/reassembler/reorderer)：运行在独立处理线程，固定到数据面核心
     *
     * 两层之间通过 RxStage 内置的 SPSC 队列解耦。
     */
    struct FacePipeline
    {
        uint8_t array_id{0}; ///< 阵面编号（1~3）
        int processing_cpu_affinity{-1}; ///< 后续处理线程绑定的 CPU 核心

        // ── 实时接收层（CPU 16/17/18 热路径） ────────────────────
        std::unique_ptr<pipeline::RxStage> rx_stage; ///< 实时接收（parse + validate + SPSC 入队）

        // ── 后续处理层（独立处理线程，固定数据面核心） ─────────────
        std::unique_ptr<pipeline::Dispatcher> dispatcher;   ///< 类型分发器
        std::unique_ptr<pipeline::Reassembler> reassembler; ///< 分片重组器
        std::unique_ptr<pipeline::Reorderer> reorderer;     ///< 序号重排器
        std::shared_ptr<delivery::RawBlockAdapter::RawBlockQueue> rawblock_queue; ///< RawBlock SPSC 队列
        std::unique_ptr<delivery::RawBlockAdapter> rawblock_adapter;               ///< OrderedPacket -> RawBlock 适配器

        // ── 增量指标高水位（per-face） ───────────────────────────────
        uint64_t last_reasm_late_fragments{0};
        uint64_t last_reasm_duplicate_fragments{0};
        uint64_t last_reasm_frames_incomplete{0};
        uint64_t last_reasm_contexts_overflow{0};
        uint64_t last_reasm_bytes_overflow{0};
        uint64_t last_reorder_duplicates{0};
        uint64_t last_reorder_zero_filled{0};
    };

    /**
     * @struct AppContext
     * @brief 接收器运行时上下文——持有全部子系统的生存期和运行状态
     *
     * 分层设计后，数据流分为两层：
     * - 实时接收层：ArrayFaceReceiver → RxStage → SPSC 队列（CPU 16/17/18）
     * - 后续处理层：Processing thread → Dispatcher → Reassembler → Reorderer → Delivery
     *
     * 两层之间通过 SPSC 队列完全解耦。
     *
     * 生命周期：
     *   main() 栈上创建 → app_init() 填充 → app_run() 使用 → app_shutdown() 销毁。
     */
    struct AppContext
    {
        // ── 基本配置 ─────────────────────────────────────────────────
        std::string config_file; ///< 实际使用的配置文件路径（初始化后与 AppOptions 一致）
        bool dry_run{false};     ///< dry-run 模式标志（不启动网络收包）
        bool started{false};     ///< 标记 app_run() 是否已成功启动（用于 app_shutdown 判断）

        // ── 共享组件 ─────────────────────────────────────────────────
        std::unique_ptr<delivery::DeliveryInterface> delivery_interface; ///< 数据投递器（共享，通过 delivery_mutex 保护）
        std::unique_ptr<network::UdpReceiver> udp_receiver;              ///< 网络层：UDP + recvmmsg 批量接收

        // ── 每阵面独立管线 ────────────────────────────────────────────
        std::vector<FacePipeline> face_pipelines;
        std::vector<std::unique_ptr<delivery::StubConsumer>> stub_consumers; ///< RawBlock 消费线程

        // ── 后续处理层线程 ────────────────────────────────────────────
        std::vector<std::thread> processing_threads; ///< 每阵面一个处理线程（pop SPSC → pipeline）
        std::atomic<bool> processing_running{false}; ///< 处理线程运行标志

        // ── 投递层互斥（仅保护 delivery_interface 的并发调用） ─────────
        std::mutex delivery_mutex;

        // ── 抓包支持 ──────────────────────────────────────────────────
        /// pcap 写入器（shared_ptr + atomic load/store 实现运行时热替换）
        std::shared_ptr<capture::PcapWriter> pcap_writer{};

        // ── 信号指标高水位 ────────────────────────────────────────────
        uint64_t last_sigint_received{0};  ///< SIGINT 信号接收累计量
        uint64_t last_sigterm_received{0}; ///< SIGTERM 信号接收累计量
        uint64_t last_sighup_received{0};  ///< SIGHUP 信号接收累计量（用于触发配置热加载）
    };

    /**
     * @brief 一次性应用程序初始化入口
     *
     * 根据 AppOptions 中指定的配置文件完成以下步骤：
     * 1. 加载并解析 YAML 配置（ConfigManager 单例）
     * 2. 初始化日志系统（Logger 单例）和 Prometheus 指标端点（MetricsCollector 单例）
     * 3. 根据 delivery.method 创建 DeliveryInterface（共享内存 / Unix Socket / 回调）
     * 4. 按管线逆序构建：Reorderer → Reassembler → Dispatcher → Parser → Validator
     *    （逆序是因为上游需要下游回调闭包）
     * 5. 配置 UDP 接收器并注册报文处理回调（协议解析→校验→分发→指标上报）
     * 6. 如启用 capture，创建 PcapWriter 并挂载抓包钩子
     *
     * @param[in]  options 启动选项（配置文件路径 + dry-run 标志）
     * @param[out] ctx     待填充的应用上下文（调用者在栈上分配，本函数填充其成员）
     *
     * @return 初始化结果
     * @retval 0 成功
     * @retval 1 配置文件加载失败
     *
     * @pre  ctx 为默认构造的空对象
     * @post 成功时 ctx 中所有 unique_ptr 成员非空，可安全传给 app_run()
     *
     * @see app_run()      使用已初始化的 ctx 进入主循环
     * @see app_shutdown()  在退出时逆序销毁 ctx 中的组件
     */
    int app_init(const AppOptions &options, AppContext &ctx);
} // namespace receiver

#endif // RECEIVER_APP_INIT_H
