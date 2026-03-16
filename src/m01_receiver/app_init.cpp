/**
 * @file app_init.cpp
 * @brief M01 接收器初始化实现——分层设计：实时接收层 + 后续处理层
 *
 * 本文件实现 app_init() 函数，按照以下顺序构建子系统：
 *
 *  ┌──────────────────────────────────────────────────────────────┐
 *  │  1. ConfigManager::load_from_file()  加载 YAML              │
 *  │  2. Logger / MetricsCollector        初始化基础设施          │
 *  │  3. DeliveryInterface                创建投递层 (shm/uds/cb)│
 *  │  4. Reorderer                        序号重排 (后续处理层)   │
 *  │  5. Reassembler                      分片重组 (后续处理层)   │
 *  │  6. Dispatcher                       类型分发 (后续处理层)   │
 *  │  7. RxStage                          实时接收层 (SPSC 入口)│
 *  │  8. UdpReceiver (+ PcapWriter)       网络层                  │
 *  └──────────────────────────────────────────────────────────────┘
 *
 * 构建顺序是「从下游到上游」：先创建消费者，再创建生产者。
 *
 * 分层设计：
 * - 实时接收层 (CPU 16/17/18)：UdpReceiver → RxStage (parse+validate+SPSC push)
 * - 后续处理层 (无指定核心)：处理线程 pop SPSC → Dispatcher → Reassembler → Reorderer
 * - 两层通过 SPSC 队列完全解耦
 *
 * @note 匿名命名空间中的辅助函数只在本编译单元内可见。
 */

#include "qdgz300/m01_receiver/app_init.h"

// ── 项目内部依赖 ────────────────────────────────────────────────────
#include "qdgz300/m01_receiver/capture/pcap_writer.h"     // PcapWriter / PcapWriterConfig
#include "qdgz300/m01_receiver/config/config_manager.h"   // ConfigManager 单例 + ReceiverConfig
#include "qdgz300/m01_receiver/monitoring/logger.h"       // Logger 单例 + LogLevel + LOG_XXX 宏
#include "qdgz300/m01_receiver/monitoring/metrics.h"      // MetricsCollector 单例 + 指标索引枚举
#include "qdgz300/m01_receiver/protocol/protocol_types.h" // CommonHeader / PacketType / ValidationResult

// ── 标准库 ──────────────────────────────────────────────────────────
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <mutex>
#include <memory>
#include <vector>

// =============================================================================
// 匿名命名空间：文件内部辅助工具函数（仅本编译单元可见）
// =============================================================================
namespace
{
    /**
     * @brief 将配置文件中的日志级别字符串映射为 LogLevel 枚举
     *
     * @param level 大写的日志级别字符串（"DEBUG" / "WARN" / "ERROR" / "INFO"）
     * @return 对应的 LogLevel 枚举值；若无法匹配则默认返回 INFO
     */
    receiver::monitoring::LogLevel parse_log_level(const std::string &level)
    {
        if (level == "DEBUG")
            return receiver::monitoring::LogLevel::DEBUG;
        if (level == "WARN")
            return receiver::monitoring::LogLevel::WARN;
        if (level == "ERROR")
            return receiver::monitoring::LogLevel::ERROR;
        return receiver::monitoring::LogLevel::INFO;
    }

    std::vector<int> resolve_processing_cpu_affinity_map(
        const receiver::config::ReceiverConfig &config,
        size_t num_faces)
    {
        if (config.performance.processing_cpu_affinity_map.size() == num_faces)
        {
            return config.performance.processing_cpu_affinity_map;
        }

        std::set<int> reserved_cpus(
            config.network.cpu_affinity_map.begin(),
            config.network.cpu_affinity_map.end());
        std::vector<int> resolved;
        resolved.reserve(num_faces);

        for (int cpu = 16; cpu <= 31 && resolved.size() < num_faces; ++cpu)
        {
            if (reserved_cpus.find(cpu) != reserved_cpus.end())
            {
                continue;
            }
            resolved.push_back(cpu);
        }

        while (resolved.size() < num_faces)
        {
            const int fallback_cpu = config.network.cpu_affinity_map.empty()
                                         ? static_cast<int>(16 + resolved.size())
                                         : config.network.cpu_affinity_map[resolved.size() % config.network.cpu_affinity_map.size()];
            resolved.push_back(fallback_cpu);
        }

        return resolved;
    }

} // namespace

namespace receiver
{
    int app_init(const AppOptions &options, AppContext &ctx)
    {
        // ══════════════════════════════════════════════════════════════
        // Phase 1: 加载 YAML 配置文件
        // ══════════════════════════════════════════════════════════════
        auto &config_mgr = config::ConfigManager::instance();
        if (!config_mgr.load_from_file(options.config_file))
        {
            std::cerr << "Failed to load config file: " << options.config_file << std::endl;
            return 1;
        }

        // 将启动选项复制到上下文，供后续阶段（如 app_run 中的 dry-run 判断）使用
        ctx.config_file = options.config_file;
        ctx.dry_run = options.dry_run;

        const auto &config = config_mgr.get_config();

        // ══════════════════════════════════════════════════════════════
        // Phase 2: 初始化基础设施——日志 & 指标
        // ══════════════════════════════════════════════════════════════
        auto &logger = monitoring::Logger::instance();
        logger.initialize(parse_log_level(config.logging.level), config.logging.log_file);

        auto &metrics = monitoring::MetricsCollector::instance();
        metrics.initialize(config.monitoring.metrics_port, config.monitoring.metrics_bind_ip);

        // ══════════════════════════════════════════════════════════════
        // Phase 3: 创建数据投递层（管线最终出口）
        //   根据配置选择投递方式：
        //   - shared_memory : 通过 POSIX 共享内存段投递给 HMI 进程
        //   - unix_socket   : 通过 Unix Domain Socket 投递（支持断线重连）
        //   - 其他 / 默认   : 仅用回调打印日志（开发调试用途）
        // ══════════════════════════════════════════════════════════════
        if (config.delivery.method == "shared_memory")
        {
            ctx.delivery_interface = std::make_unique<delivery::SharedMemoryDelivery>(
                config.delivery.shm_name,
                config.delivery.shm_size_mb * 1024 * 1024); // MB → bytes
        }
        else if (config.delivery.method == "unix_socket")
        {
            ctx.delivery_interface = std::make_unique<delivery::UnixSocketDelivery>(
                config.delivery.socket_path,
                config.delivery.reconnect_interval_ms);
        }
        else
        {
            // 默认回调投递——仅在 DEBUG 级别日志中打印序号，适用于开发环境
            ctx.delivery_interface = std::make_unique<delivery::CallbackDelivery>(
                [](const pipeline::OrderedPacket &packet)
                {
                    LOG_DEBUG("Delivered packet seq=%u", packet.sequence_number);
                });
        }

        // ══════════════════════════════════════════════════════════════
        // Phase 4: 确定阵面拓扑
        // ══════════════════════════════════════════════════════════════

        // ── 4a. 配置 UDP 接收器 ─────────────────────────────────────
        network::UdpReceiverConfig udp_config;
        udp_config.listen_port = config.network.listen_port;
        udp_config.bind_ip = config.network.bind_ip;
        udp_config.recv_batch_size = config.network.recvmmsg_batch_size;
        udp_config.recv_drain_rounds = config.performance.recv_drain_rounds;
        udp_config.socket_rcvbuf_mb = config.network.socket_rcvbuf_mb;
        udp_config.packet_pool_mb_per_face = config.performance.packet_pool_mb_per_face;
        udp_config.enable_so_reuseport = false;
        udp_config.enable_ip_freebind = config.network.enable_ip_freebind;
        udp_config.worker_threads = 0;
        udp_config.numa_node = config.performance.numa_node;
        udp_config.cpu_affinity_start = 16;
        udp_config.cpu_affinity_end = 31;
        udp_config.prefetch_hints_enabled = config.performance.prefetch_hints_enabled;

        // 三阵面拓扑：三个阵面各自绑定不同的 IP 地址（对应独立网卡）
        const bool has_three_face_topology =
            config.network.bind_ips.size() == 3 &&
            config.network.source_id_map.size() == 3 &&
            config.network.cpu_affinity_map.size() == 3;

        size_t num_faces = 1;
        if (has_three_face_topology)
        {
            num_faces = 3;
            udp_config.array_faces.reserve(3);
            for (size_t i = 0; i < 3; ++i)
            {
                network::ArrayFaceBinding face{};
                face.array_id = static_cast<uint8_t>(i + 1);
                face.bind_ip = config.network.bind_ips[i];
                face.listen_port = config.network.listen_port;
                face.expected_source_id = config.network.source_id_map[i];
                face.cpu_affinity = config.network.cpu_affinity_map[i];
                face.source_filter_enabled = config.network.source_filter_enabled;
                udp_config.array_faces.push_back(face);
            }
        }
        else
        {
            network::ArrayFaceBinding fallback{};
            fallback.array_id = 1;
            fallback.bind_ip = config.network.bind_ip;
            fallback.listen_port = config.network.listen_port;
            fallback.expected_source_id = 0x11;
            fallback.cpu_affinity = 16;
            fallback.source_filter_enabled = false;
            udp_config.array_faces.push_back(fallback);
        }

        const std::vector<int> processing_cpu_map = resolve_processing_cpu_affinity_map(config, num_faces);

        // ══════════════════════════════════════════════════════════════
        // Phase 5: 为每个阵面构建分层管线
        //
        // 实时接收层：RxStage (parse + validate + SPSC push) — CPU 16/17/18
        // 后续处理层：Dispatcher → Reassembler → Reorderer — 处理线程
        // 两层通过 SPSC 队列解耦，仅 DeliveryInterface 被所有阵面共享。
        // ══════════════════════════════════════════════════════════════
        ctx.face_pipelines.resize(num_faces);

        for (size_t face_idx = 0; face_idx < num_faces; ++face_idx)
        {
            FacePipeline &fp = ctx.face_pipelines[face_idx];
            fp.array_id = static_cast<uint8_t>(face_idx + 1);
            fp.processing_cpu_affinity = processing_cpu_map[face_idx];

            // ── 5a. 序号重排器 (Reorderer) —— 后续处理层 ──────────────
            pipeline::ReorderConfig reorder_config;
            reorder_config.window_size = config.reorder.window_size;
            reorder_config.timeout_ms = config.reorder.timeout_ms;
            reorder_config.enable_zero_fill = config.reorder.enable_zero_fill;

            fp.reorderer = std::make_unique<pipeline::Reorderer>(
                reorder_config,
                [&ctx, face_idx](pipeline::OrderedPacket &&packet)
                {
                    // 路径 A：原有 DeliveryInterface 投递（mutex 保护共享资源）
                    if (ctx.delivery_interface)
                    {
                        std::lock_guard<std::mutex> guard(ctx.delivery_mutex);
                        ctx.delivery_interface->deliver(packet);
                    }

                    // 路径 B：RawBlock 投递（无锁 — 每阵面独立的 SPSC 队列）
                    auto &fp = ctx.face_pipelines[face_idx];
                    if (fp.rawblock_adapter)
                    {
                        fp.rawblock_adapter->adapt_and_push(std::move(packet));
                    }
                });

            // ── 5b. 分片重组器 (Reassembler) —— 后续处理层 ────────────
            pipeline::ReassemblerConfig reasm_config;
            reasm_config.timeout_ms = config.reassembly.timeout_ms;
            reasm_config.max_contexts = config.reassembly.max_contexts;
            reasm_config.max_total_frags = config.reassembly.max_total_frags;
            reasm_config.sample_count_fixed = config.reassembly.sample_count_fixed;
            reasm_config.max_reasm_bytes_per_key = config.reassembly.max_reasm_bytes_per_key;
            reasm_config.numa_node = config.performance.numa_node;
            reasm_config.cache_align_bytes = config.performance.reassembler_cache_align_bytes;
            reasm_config.prefetch_hints_enabled = config.performance.prefetch_hints_enabled;

            // 捕获 fp.reorderer 的裸指针——FacePipeline 生命周期 >= Reassembler
            pipeline::Reorderer *reorderer_ptr = fp.reorderer.get();
            fp.reassembler = std::make_unique<pipeline::Reassembler>(
                reasm_config,
                [reorderer_ptr](pipeline::ReassembledFrame &&frame)
                {
                    protocol::CommonHeader header{};
                    header.magic = protocol::PROTOCOL_MAGIC;
                    header.protocol_version = protocol::PROTOCOL_VERSION;
                    header.packet_type = static_cast<uint8_t>(protocol::PacketType::DATA);
                    header.source_id = frame.key.source_id;
                    header.control_epoch = frame.key.control_epoch;
                    header.sequence_number = frame.key.frame_counter;
                    header.payload_len = static_cast<uint16_t>(frame.total_size);
                    header.timestamp = frame.data_timestamp;
                    header.ext_flags = frame.is_complete ? 0u : 0x01u;
                    reorderer_ptr->insert_owned(header, std::move(frame.data), frame.total_size);
                });

            // ── 5c. 类型分发器 (Dispatcher) —— 后续处理层 ─────────────
            pipeline::Reassembler *reassembler_ptr = fp.reassembler.get();
            fp.dispatcher = std::make_unique<pipeline::Dispatcher>(
                // DATA 处理回调
                [reassembler_ptr, &logger](const protocol::ParsedPacket &packet)
                {
                    if (logger.is_enabled(monitoring::LogLevel::DEBUG))
                    {
                        logger.with_trace_id(
                                  packet.header.source_id,
                                  packet.header.control_epoch,
                                  packet.header.sequence_number)
                            .debug("DATA packet payload_len=%u", static_cast<unsigned>(packet.header.payload_len));
                    }
                    reassembler_ptr->process_packet(packet);
                },
                // HEARTBEAT 处理回调
                [&metrics, &logger](const protocol::ParsedPacket &packet)
                {
                    metrics.set_heartbeat_state(2);
                    if (!logger.is_enabled(monitoring::LogLevel::DEBUG))
                    {
                        return;
                    }
                    LOG_DEBUG("Heartbeat received source=%u seq=%u",
                              static_cast<unsigned>(packet.header.source_id),
                              static_cast<unsigned>(packet.header.sequence_number));
                });
            fp.dispatcher->set_heartbeat_max_queue_depth(config.performance.heartbeat_max_queue_depth);

            // ── 零拷贝 DATA 处理路径：dispatcher → reassembler 传递 PacketBuffer 所有权 ──
            fp.dispatcher->set_zero_copy_data_handler(
                [reassembler_ptr, &logger](const protocol::ParsedPacket &packet,
                                            network::PacketBuffer &&buffer)
                {
                    if (logger.is_enabled(monitoring::LogLevel::DEBUG))
                    {
                        logger.with_trace_id(
                                  packet.header.source_id,
                                  packet.header.control_epoch,
                                  packet.header.sequence_number)
                            .debug("DATA packet (zero-copy) payload_len=%u",
                                   static_cast<unsigned>(packet.header.payload_len));
                    }
                    reassembler_ptr->process_packet_zero_copy(packet, std::move(buffer));
                });

            // ── 5d. 实时接收层 (RxStage) —— CPU 16/17/18 热路径 ───
            // RxStage 内部拥有独立的 PacketParser 和 Validator，
            // 仅做：parse → validate → 打时间戳 → SPSC push
            fp.rx_stage = std::make_unique<pipeline::RxStage>(
                fp.array_id, config.network.local_device_id);
        }

        // ══════════════════════════════════════════════════════════════
        // Phase 5e: RawBlock 投递路径（可选 — 由 config.consumer 控制）
        //   当 consumer.print_summary 或 consumer.write_to_file 为 true 时激活。
        //   为每阵面独立创建：
        //     RawBlockQueue（SPSC<shared_ptr<RawBlock>, 64>）
        //     RawBlockAdapter（OrderedPacket → RawBlock 封装）
        //     StubConsumer（从队列消费并统计/写文件）
        // ══════════════════════════════════════════════════════════════
        if (config.consumer.print_summary || config.consumer.write_to_file)
        {
            LOG_INFO("[Init] Phase 5e: Creating RawBlock delivery path (%zu faces)", num_faces);
            ctx.stub_consumers.reserve(num_faces);

            for (size_t face_idx = 0; face_idx < num_faces; ++face_idx)
            {
                FacePipeline &fp = ctx.face_pipelines[face_idx];

                // 1. 创建 SPSC 队列
                fp.rawblock_queue = std::make_shared<delivery::RawBlockAdapter::RawBlockQueue>();

                // 2. 创建适配器
                fp.rawblock_adapter = std::make_unique<delivery::RawBlockAdapter>(
                    fp.rawblock_queue, fp.array_id);

                // 3. 创建 StubConsumer
                delivery::StubConsumerConfig sc_cfg;
                sc_cfg.array_id = fp.array_id;
                sc_cfg.print_summary = config.consumer.print_summary;
                sc_cfg.write_to_file = config.consumer.write_to_file;
                sc_cfg.output_file = config.consumer.output_dir;
                sc_cfg.stats_interval_ms = config.consumer.stats_interval_ms;

                ctx.stub_consumers.push_back(
                    std::make_unique<delivery::StubConsumer>(sc_cfg, fp.rawblock_queue));
            }
        }

        // ══════════════════════════════════════════════════════════════
        // Phase 6: 配置 PCAP 抓包（可选）
        // ══════════════════════════════════════════════════════════════
        if (config.capture.enabled)
        {
            capture::PcapWriterConfig capture_cfg;
            capture_cfg.enabled = config.capture.enabled;
            capture_cfg.spool_dir = config.capture.spool_dir;
            capture_cfg.archive_dir = config.capture.archive_dir;
            capture_cfg.max_file_size_mb = config.capture.max_file_size_mb;
            capture_cfg.max_files = config.capture.max_files;
            capture_cfg.filter_packet_types = config.capture.filter_packet_types;
            capture_cfg.filter_source_ids = config.capture.filter_source_ids;
            auto pcap_writer = std::make_shared<capture::PcapWriter>(capture_cfg);
            if (pcap_writer->start())
            {
                std::atomic_store_explicit(&ctx.pcap_writer, std::move(pcap_writer), std::memory_order_release);
                LOG_INFO("Capture spool enabled: spool_dir=%s archive_dir=%s max_file_size_mb=%u max_files=%u",
                         config.capture.spool_dir.c_str(),
                         config.capture.archive_dir.c_str(),
                         static_cast<unsigned>(config.capture.max_file_size_mb),
                         static_cast<unsigned>(config.capture.max_files));
                udp_config.capture_hook = [&](const uint8_t *raw_data, size_t length, uint64_t timestamp_us)
                {
                    auto writer = std::atomic_load_explicit(&ctx.pcap_writer, std::memory_order_acquire);
                    if (writer)
                    {
                        writer->write_packet(raw_data, length, timestamp_us);
                    }
                };
            }
            else
            {
                LOG_WARN("Capture enabled but spool writer failed to start: spool_dir=%s",
                         config.capture.spool_dir.c_str());
            }
        }

        // ══════════════════════════════════════════════════════════════
        // Phase 7: 创建 UdpReceiver 并注册报文处理回调
        //
        // 分层设计：回调仅调用 RxStage::on_packet()，
        // 在收包线程（CPU 16/17/18）中完成最轻量的工作：
        //   parse → validate → 打时间戳 → SPSC push
        //
        // 完整数据面处理（Dispatcher/Reassembler/Reorderer）从热路径移除，
        // 由独立的 processing_thread 从 SPSC 队列消费。
        // ══════════════════════════════════════════════════════════════
        ctx.udp_receiver = std::make_unique<network::UdpReceiver>(
            udp_config,
            [&ctx](network::ReceivedPacket &&raw_packet)
            {
                // 根据 array_id 路由到对应阵面的 RxStage（array_id 从 1 开始）
                const size_t face_idx = static_cast<size_t>(raw_packet.array_id) - 1;
                if (face_idx >= ctx.face_pipelines.size())
                {
                    return;
                }

                // 热路径唯一操作：parse + validate + SPSC push
                ctx.face_pipelines[face_idx].rx_stage->on_packet(std::move(raw_packet));
            });

        // 全部组件构建完毕，返回 0 表示初始化成功
        return 0;
    }
} // namespace receiver
