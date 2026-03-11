// include/qdgz300/common/types.h
// QDGZ300 核心数据结构定义
// 协议层结构体 (#pragma pack) + 数据平面内部结构
#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "qdgz300/common/constants.h"

namespace qdgz300
{

    // ═══════════════════════════════════════════════════════
    //  协议层结构体 — #pragma pack(push, 1) 对线格式
    // ═══════════════════════════════════════════════════════

#pragma pack(push, 1)

    /// V3.1 通用报文头（32 Bytes）— 所有 PacketType 共用
    struct CommonHeader
    {
        uint32_t magic;            // Offset  0: 固定 0x55AA55AA
        uint16_t protocol_version; // Offset  4: 0x31
        uint8_t packet_type;       // Offset  6: 0x01~0x04 / 0xFF
        uint8_t source_id;         // Offset  7: 设备标识
        uint8_t dest_id;           // Offset  8: 目标标识
        uint8_t reserved_0;        // Offset  9: MBZ
        uint16_t payload_len;      // Offset 10: Payload 长度
        uint32_t sequence_number;  // Offset 12: 发送端递增
        uint64_t timestamp;        // Offset 16: Unix Epoch ms
        uint32_t control_epoch;    // Offset 24: 控制纪元
        uint16_t link_epoch;       // Offset 28: 链路纪元
        uint16_t reserved_1;       // Offset 30: MBZ
    };
    static_assert(sizeof(CommonHeader) == 32, "CommonHeader must be 32 bytes");

    /// V3.1 Data Specific Header（40 Bytes）— PacketType=0x03
    struct DataSpecificHeader
    {
        uint32_t frame_counter;           // Offset  0: 搜索帧计数
        uint32_t cpi_count;               // Offset  4: CPI 序列号
        uint32_t pulse_index;             // Offset  8: 脉冲序号
        uint32_t sample_offset;           // Offset 12: 快时间偏移
        uint32_t sample_count;            // Offset 16: 采样点数
        uint64_t data_timestamp;          // Offset 20: 脉冲采集时刻 (ns)
        uint16_t health_summary;          // Offset 28: 关键告警摘要
        uint8_t channel_mask;             // Offset 30: 通道掩码
        uint8_t data_type;                // Offset 31: 数据类型
        uint16_t beam_id;                 // Offset 32: 波束编号
        uint16_t frag_index;              // Offset 34: 分片索引
        uint16_t total_frags;             // Offset 36: 总分片数
        uint16_t tail_frag_payload_bytes; // Offset 38: 尾包 RAW 有效载荷长度
    };
    static_assert(sizeof(DataSpecificHeader) == 40, "DataSpecificHeader must be 40 bytes");

    /// V3.1 Execution Snapshot（40 Bytes）— 仅尾包携带
    struct ExecutionSnapshot
    {
        int16_t azimuth_angle;       // 方位角
        int16_t elevation_angle;     // 俯仰角
        uint8_t work_freq_index;     // 工作频点索引
        uint8_t reserved;            // 保留
        uint16_t mgc_gain;           // MGC 增益
        uint16_t signal_bandwidth;   // 信号带宽
        uint16_t pulse_width;        // 脉冲宽度
        uint16_t pulse_period;       // 脉冲周期
        uint16_t accumulation_count; // 积累点数
        int32_t tilt_sensor_x;       // 倾角 X
        int32_t tilt_sensor_y;       // 倾角 Y
        uint32_t heading_angle;      // 航向角
        int32_t bds_longitude;       // 北斗经度
        int32_t bds_latitude;        // 北斗纬度
        int32_t bds_altitude;        // 北斗高度
    };
    static_assert(sizeof(ExecutionSnapshot) == 40, "ExecutionSnapshot must be 40 bytes");

    /// V3.1 心跳载荷（48 Bytes）— PacketType=0x04
    struct HeartbeatPayload
    {
        uint32_t system_status_alive;   // 存活状态
        uint32_t system_status_state;   // 系统状态
        int16_t core_temp;              // 核心温度
        uint8_t op_mode;                // 运行模式
        uint8_t time_coord_source;      // 时间源
        uint8_t device_number[4];       // 设备号
        int32_t time_offset;            // 时偏
        uint32_t error_counters[4];     // 错误计数器
        uint8_t lo_lock_flags;          // 本振锁定标志
        uint8_t supply_current;         // 供电电流
        uint8_t tr_comm_status;         // TR 通信状态
        uint8_t reserved_align2;        // 对齐保留
        uint16_t voltage_stability_flags; // 电源稳定标志
        int16_t panel_temp;             // 面板温度
        uint32_t crc32c;                // CRC32C
    };
    static_assert(sizeof(HeartbeatPayload) == 48, "HeartbeatPayload must be 48 bytes");

#pragma pack(pop)

    // ═══════════════════════════════════════════════════════
    //  数据平面内部结构 — 自然对齐
    // ═══════════════════════════════════════════════════════

    /// 重组后的完整 CPI 帧（M01 → M02）
    struct RawBlock
    {
        uint64_t ingest_ts;         // 入口时间戳 (ns, CLOCK_MONOTONIC)
        uint64_t data_ts;           // 数据时间戳 (来自 DACS DataTimestamp, ns)
        uint8_t array_id;           // 阵面编号 1/2/3
        uint32_t cpi_seq;           // CPI 序列号
        uint16_t fragment_count;    // 分片总数 (TotalFrags)
        uint32_t data_size;         // RAW 有效载荷总字节数
        uint32_t flags;             // bit0: IncompleteFrame
                                    // bit1: SnapshotPresent
                                    // bit2: HeartbeatRelated
                                    // bit3: GpuTimeout
        ExecutionSnapshot snapshot; // 仅当 flags.bit1=1 有效
        uint8_t *payload;           // 指向 NUMAPool 分配的 payload 缓冲区

        // RawBlock flags 位定义
        static constexpr uint32_t FLAG_INCOMPLETE_FRAME = 1u << 0;
        static constexpr uint32_t FLAG_SNAPSHOT_PRESENT = 1u << 1;
        static constexpr uint32_t FLAG_HEARTBEAT_RELATED = 1u << 2;
        static constexpr uint32_t FLAG_GPU_TIMEOUT = 1u << 3;
    };

    /// 单个点迹
    struct Plot
    {
        double range_m;       // 距离 (m)
        double azimuth_deg;   // 方位角 (°)
        double elevation_deg; // 俯仰角 (°)
        double doppler_mps;   // 多普勒速度 (m/s)
        float snr_db;         // 信噪比 (dB)
        float amplitude;      // 幅度
        uint16_t beam_id;     // 波束编号
        uint8_t array_id;     // 来源阵面
        uint8_t reserved;
    };

    /// 点迹批次（M02 → M03），per-CPI 粒度
    struct PlotBatch
    {
        uint64_t data_ts;    // 数据时间戳 (ns)
        uint64_t process_ts; // GPU 处理完成时间戳 (ns)
        uint8_t array_id;    // 阵面编号
        uint32_t cpi_seq;    // CPI 序列号
        uint16_t plot_count; // 本批次点迹数量
        uint32_t flags;      // bit0: IncompleteSource
                             // bit3: GpuTimeout
        Plot *plots;         // 指向 NUMAPool 分配的 Plot 数组
    };

    /// 单条航迹
    struct Track
    {
        uint64_t track_id;       // 全局唯一 ID，单调递增不复用
        double pos_x;            // 位置 X (m, 本地坐标系)
        double pos_y;            // 位置 Y (m)
        double pos_z;            // 位置 Z (m)
        double vel_x;            // 速度 X (m/s)
        double vel_y;            // 速度 Y (m/s)
        double vel_z;            // 速度 Z (m/s)
        double heading_deg;      // 航向 (°)
        float confidence;        // 置信度 [0, 1]
        uint8_t lifecycle_state; // TrackLifecycle 枚举值
        uint8_t source_array_id; // 当前数据来源阵面
        bool handover_flag;      // 是否为接力航迹
        uint8_t handover_from;   // 接力来源阵面（若有）
        uint32_t coast_frames;   // 连续 coast 帧数
        uint64_t create_ts;      // 航迹创建时间戳 (ns)
        uint64_t update_ts;      // 最近更新时间戳 (ns)
    };

    /// 航迹帧快照（M03 → M04）— 全量快照
    struct TrackFrame
    {
        uint64_t frame_seq;            // 帧序号，单调递增
        uint64_t data_timestamp_ns;    // 数据时间戳
        uint64_t backend_instance_id;  // 后端实例 ID
        uint8_t clock_domain;          // 时间域标识
        uint8_t coverage_mask;         // 3-bit 阵面覆盖位图
        uint16_t track_count;          // 本帧航迹数量
        uint32_t system_quality_flags; // 系统质量标志位
        bool is_truncated;             // UDP 输出时是否被截断
        uint16_t dropped_count;        // 截断时被丢弃的航迹数
        Track *tracks;                 // 指向 Track 数组
    };

    /// 航迹生命周期状态
    enum class TrackLifecycle : uint8_t
    {
        TENTATIVE = 0, // 暂定（首次检测，待确认）
        CONFIRMED = 1, // 已确认（稳定跟踪中）
        COASTING = 2,  // 滑行（丢失更新，纯预测）
        LOST = 3,      // 丢失（超出容忍阈值）
        DELETED = 4,   // 已删除（资源释放）
    };

} // namespace qdgz300
