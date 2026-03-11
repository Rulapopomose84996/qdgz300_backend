// include/qdgz300/common/constants.h
// QDGZ300 冻结常量表 — 绝对不可修改
// 来源: V3.1 初次定版 协议冻结值
#pragma once
#include <cstdint>
#include <cstddef>

namespace qdgz300
{

    // ═══ 协议 V3.1 冻结值 ═══
    constexpr uint32_t PROTOCOL_MAGIC = 0x55AA55AA;
    constexpr uint8_t PROTOCOL_VERSION = 0x31;       // V3.1
    constexpr uint16_t COMMON_HEADER_SIZE = 32;      // bytes
    constexpr uint16_t DATA_SPECIFIC_HDR_SIZE = 40;  // bytes
    constexpr uint16_t EXECUTION_SNAPSHOT_SIZE = 40; // bytes
    constexpr uint16_t HEARTBEAT_PAYLOAD_SIZE = 48;  // bytes
    constexpr uint16_t HEARTBEAT_TOTAL_SIZE = 80;    // 32 + 48

    // ═══ 重组冻结值 ═══
    constexpr uint32_t T_REASM_MS = 100;                           // 重组超时 ms（绝对冻结）
    constexpr uint16_t MAX_TOTALFRAGS = 1024;                      // 单 CPI 最大分片数
    constexpr uint32_t MAX_REASM_BYTES_PER_KEY = 16 * 1024 * 1024; // 16 MiB

    // ═══ UDP / 网络 ═══
    constexpr uint16_t UDP_DATA_PORT = 9999;
    constexpr uint16_t UDP_CONTROL_PORT = 8888;
    constexpr uint16_t UDP_RMA_PORT = 7777;
    constexpr uint16_t MAX_UDP_PAYLOAD = 1200; // HMI TrackData 单帧上限 bytes
    constexpr uint16_t RECVMMSG_BATCH_SIZE = 64;

    // ═══ 设备标识 ═══
    constexpr uint8_t SPS_ID = 0x01;
    constexpr uint8_t DACS_ARRAY_1 = 0x11;
    constexpr uint8_t DACS_ARRAY_2 = 0x12;
    constexpr uint8_t DACS_ARRAY_3 = 0x13;
    constexpr uint8_t DACS_BROADCAST = 0x10;

    // ═══ 数据面 PacketType ═══
    constexpr uint8_t PKT_TYPE_CONTROL = 0x01;
    constexpr uint8_t PKT_TYPE_ACK = 0x02;
    constexpr uint8_t PKT_TYPE_DATA = 0x03;
    constexpr uint8_t PKT_TYPE_HEARTBEAT = 0x04;
    constexpr uint8_t PKT_TYPE_RMA = 0xFF;

    // ═══ 队列容量 ═══
    constexpr size_t RAWCPI_Q_CAPACITY = 64;
    constexpr size_t REC_Q_CAPACITY = 32;
    constexpr size_t PLOTS_Q_CAPACITY = 256;
    constexpr size_t TRACK_Q_CAPACITY = 256;
    constexpr size_t TRACKDATA_Q_CAPACITY = 128;

    // ═══ GPU 超时参数 ═══
    constexpr uint32_t T_GPU_MAX_MS = 50;    // 单帧 GPU 处理上限
    constexpr uint32_t T_GPU_WARN_MS = 30;   // 预警阈值
    constexpr uint32_t T_KERNEL_MAX_MS = 20; // 单 Kernel 上限
    constexpr uint32_t GPU_STREAM_COUNT = 3; // CUDA Streams 数
    constexpr uint32_t GPU_INFLIGHT_MAX = 2; // ping-pong 双缓冲

    // ═══ 控制面重传 ═══
    constexpr uint32_t RTO_MS = 2500; // 首次重传超时
    constexpr uint32_t MAX_RETRY = 3; // 最大重传次数

    // ═══ HMI 心跳 ═══
    constexpr uint32_t HMI_PING_INTERVAL_MS = 1000;
    constexpr uint32_t HMI_PONG_TIMEOUT_MS = 3000;

    // ═══ 跨阵面融合门限 ═══
    constexpr double D_HANDOVER_M = 100.0;      // 位置偏差 ≤ 100m
    constexpr double V_HANDOVER_MS = 10.0;      // 速度偏差 ≤ 10m/s
    constexpr double THETA_HANDOVER_DEG = 15.0; // 航向偏差 ≤ 15°

    // ═══ 系统状态转移 ═══
    constexpr uint32_t T_TRANSITION_MIN_MS = 10000; // 最小转移间隔
    constexpr uint32_t T1_DEGRADED_MS = 500;        // ≥500ms 无 TrackData → Degraded
    constexpr uint32_t T2_FAULT_MS = 2000;          // ≥2s 无 TrackData → Fault

    // ═══ 健康检查 ═══
    constexpr uint32_t RUNTIME_MONITOR_TICK_MS = 100;
    constexpr uint32_t BOOT_UDP_TIMEOUT_MS = 500;
    constexpr uint32_t WARMUP_MIN_INPUT_MS = 1000; // 连续输入 ≥1000ms

    // ═══ 录制 ═══
    constexpr size_t RAW_BLOCK_PAYLOAD_SIZE = 2 * 1024 * 1024; // ~2 MB

    // ═══ 阵面数量 ═══
    constexpr uint8_t ARRAY_FACE_COUNT = 3;

} // namespace qdgz300
