#ifndef RECEIVER_PROTOCOL_TYPES_H
#define RECEIVER_PROTOCOL_TYPES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace receiver
{
    namespace protocol
    {

        constexpr uint32_t PROTOCOL_MAGIC = 0x55AA55AA;
        constexpr uint8_t PROTOCOL_VERSION_MAJOR = 3;
        constexpr uint8_t PROTOCOL_VERSION_MINOR = 1;
        constexpr uint8_t PROTOCOL_VERSION = 0x31;

        constexpr size_t COMMON_HEADER_SIZE = 32;
        constexpr size_t MAX_PAYLOAD_SIZE = 65507 - COMMON_HEADER_SIZE;
        constexpr size_t CONTROL_TABLE_HEADER_SIZE = 12;
        constexpr size_t CONTROL_BEAM_ITEM_SIZE = 32;
        constexpr size_t SCHEDULE_APPLY_ACK_PAYLOAD_SIZE = 24;
        constexpr size_t HEARTBEAT_PAYLOAD_SIZE = 48;
        constexpr size_t RMA_HEADER_SIZE = 16;

        /**
         * @brief 协议报文类型
         */
        enum class PacketType : uint8_t
        {
            /** @brief 控制面报文 */
            CONTROL = 0x01,
            /** @brief 应答报文 */
            ACK = 0x02,
            /** @brief 数据面报文 */
            DATA = 0x03,
            /** @brief 心跳报文 */
            HEARTBEAT = 0x04,
            /** @brief 远程维护报文 */
            RMA = 0xFF
        };

        enum class PacketPriority : uint8_t
        {
            HIGH = 0,
            NORMAL = 1
        };

        // Minimal control-plane ack tracking placeholder kept for API compatibility.
        class AckTracker
        {
        public:
            struct Entry
            {
            };
        };

        template <typename T>
        class PriorityQueue
        {
        public:
            explicit PriorityQueue(size_t max_depth = 1000)
                : max_depth_(max_depth == 0 ? 1 : max_depth),
                  total_depth_(0)
            {
            }

            bool push(const T &value, PacketPriority priority)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (total_depth_ >= max_depth_)
                {
                    return false;
                }
                if (priority == PacketPriority::HIGH)
                {
                    high_priority_.push_back(value);
                }
                else
                {
                    normal_priority_.push_back(value);
                }
                ++total_depth_;
                return true;
            }

            bool push(T &&value, PacketPriority priority)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (total_depth_ >= max_depth_)
                {
                    return false;
                }
                if (priority == PacketPriority::HIGH)
                {
                    high_priority_.push_back(std::move(value));
                }
                else
                {
                    normal_priority_.push_back(std::move(value));
                }
                ++total_depth_;
                return true;
            }

            bool pop(T &out)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!high_priority_.empty())
                {
                    out = std::move(high_priority_.front());
                    high_priority_.pop_front();
                    --total_depth_;
                    return true;
                }
                if (!normal_priority_.empty())
                {
                    out = std::move(normal_priority_.front());
                    normal_priority_.pop_front();
                    --total_depth_;
                    return true;
                }
                return false;
            }

            size_t depth() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return total_depth_;
            }

            size_t high_depth() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return high_priority_.size();
            }

            size_t normal_depth() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return normal_priority_.size();
            }

            void set_max_depth(size_t max_depth)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                max_depth_ = max_depth == 0 ? 1 : max_depth;
            }

        private:
            mutable std::mutex mutex_;
            size_t max_depth_;
            std::deque<T> high_priority_;
            std::deque<T> normal_priority_;
            size_t total_depth_;
        };

        /**
         * @brief 设备ID定义
         */
        enum class DeviceID : uint8_t
        {
            UNPROVISIONED = 0x00,
            SPS = 0x01,
            BROADCAST = 0x10,
            DACS_01 = 0x11,
            DACS_02 = 0x12,
            DACS_03 = 0x13,
            RESERVED = 0xFF
        };

        /**
         * @brief 计算16位值中置位bit个数
         * @param value 输入值
         * @return 置位bit计数
         */
        inline uint8_t popcount16(uint16_t value)
        {
            uint8_t count = 0;
            while (value != 0)
            {
                count += static_cast<uint8_t>(value & 0x1u);
                value = static_cast<uint16_t>(value >> 1);
            }
            return count;
        }

#pragma pack(push, 1)
        /**
         * @brief 通用报文头（固定32字节）
         */
        struct CommonHeader
        {
            uint32_t magic;
            uint32_t sequence_number;
            uint64_t timestamp;
            uint16_t payload_len;
            uint8_t packet_type;
            uint8_t protocol_version;
            uint8_t source_id;
            uint8_t dest_id;
            uint16_t control_epoch;
            uint8_t ext_flags;
            uint8_t reserved1;
            uint16_t reserved2;
            uint8_t reserved3[4];

            /**
             * @brief 检查Magic字段是否合法
             * @return Magic合法返回true
             */
            bool is_valid_magic() const
            {
                return magic == PROTOCOL_MAGIC;
            }

            /**
             * @brief 检查协议版本主版本是否合法
             * @return 主版本合法返回true
             */
            bool is_valid_version() const
            {
                return (protocol_version >> 4) == PROTOCOL_VERSION_MAJOR;
            }

            /**
             * @brief 获取PacketType枚举值
             * @return 当前报文类型
             */
            PacketType get_packet_type() const
            {
                return static_cast<PacketType>(packet_type);
            }

            /**
             * @brief 获取主版本号
             * @return 主版本号
             */
            uint8_t get_major_version() const
            {
                return protocol_version >> 4;
            }

            /**
             * @brief 获取次版本号
             * @return 次版本号
             */
            uint8_t get_minor_version() const
            {
                return protocol_version & 0x0F;
            }
        };
#pragma pack(pop)

        static_assert(sizeof(CommonHeader) == COMMON_HEADER_SIZE,
                      "CommonHeader size must be exactly 32 bytes");
        static_assert(offsetof(CommonHeader, ext_flags) == 24,
                      "CommonHeader.ext_flags offset must be 24");
        static_assert(offsetof(CommonHeader, reserved1) == 25,
                      "CommonHeader.reserved1 offset must be 25");
        static_assert(offsetof(CommonHeader, reserved2) == 26,
                      "CommonHeader.reserved2 offset must be 26");
        static_assert(offsetof(CommonHeader, reserved3) == 28,
                      "CommonHeader.reserved3 offset must be 28");

        /**
         * @brief 解析后的报文视图
         */
        struct ParsedPacket
        {
            CommonHeader header;
            const uint8_t *payload;
            size_t total_size;

            /**
             * @brief 默认构造并清零header
             */
            ParsedPacket()
                : payload(nullptr), total_size(0)
            {
                std::memset(&header, 0, sizeof(header));
            }
        };

        /**
         * @brief 数据载荷类型
         */
        enum class DataType : uint8_t
        {
            RAW_BINARY = 0x00,
            WAVEFORM = 0x01,
            DA_DATA = 0x02
        };

#pragma pack(push, 1)
        /**
         * @brief 数据包专用头（固定40字节）
         */
        struct DataSpecificHeader
        {
            uint32_t frame_counter;
            uint32_t cpi_count;
            uint32_t pulse_index;
            uint32_t sample_offset;
            uint32_t sample_count;
            uint64_t data_timestamp;
            uint16_t health_summary;
            uint16_t reserved_health;
            uint16_t beam_id;
            uint16_t frag_index;
            uint16_t total_frags;
            uint16_t tail_frag_payload_bytes;

            // PROTOCOL PENDING CLARIFICATION:
            // v3.1 §4.2 defines offset 30 as Reserved_Health, while §4.1/§4.4 still
            // require ChannelMask/DataType semantics. Temporarily tunnel them here:
            // - reserved_health[11:0]  -> channel_mask (12-bit)
            // - reserved_health[15:12] -> data_type (4-bit)
            static constexpr uint16_t RESERVED_HEALTH_CHANNEL_MASK_MASK = 0x0FFFu;
            static constexpr uint16_t RESERVED_HEALTH_DATA_TYPE_MASK = 0xF000u;
            static constexpr uint8_t RESERVED_HEALTH_DATA_TYPE_SHIFT = 12u;

            /**
             * @brief 判断当前分片是否尾分片
             * @return 尾分片返回true
             */
            bool is_tail_fragment() const
            {
                return frag_index == (total_frags - 1);
            }

            /**
             * @brief 判断是否单分片报文
             * @return 单分片返回true
             */
            bool is_single_fragment() const
            {
                return total_frags == 1;
            }

            /**
             * @brief 获取通道数量
             * @return 通道数量
             */
            uint8_t get_channel_count() const
            {
                return popcount16(get_channel_mask());
            }

            /**
             * @brief 获取数据类型枚举
             * @return 数据类型
             */
            DataType get_data_type() const
            {
                return static_cast<DataType>(get_data_type_raw());
            }

            /**
             * @brief 获取通道掩码
             * @return 12位通道掩码
             */
            uint16_t get_channel_mask() const
            {
                return static_cast<uint16_t>(reserved_health & RESERVED_HEALTH_CHANNEL_MASK_MASK);
            }

            /**
             * @brief 获取原始数据类型字段
             * @return 4位原始数据类型
             */
            uint8_t get_data_type_raw() const
            {
                return static_cast<uint8_t>((reserved_health & RESERVED_HEALTH_DATA_TYPE_MASK) >>
                                            RESERVED_HEALTH_DATA_TYPE_SHIFT);
            }

            /**
             * @brief 兼容写入通道掩码与数据类型到reserved_health
             * @param channel_mask 12位通道掩码
             * @param data_type 4位数据类型
             * @return void
             */
            void set_channel_mask_data_type_compat(uint16_t channel_mask, uint8_t data_type)
            {
                const uint16_t masked_channel =
                    static_cast<uint16_t>(channel_mask & RESERVED_HEALTH_CHANNEL_MASK_MASK);
                const uint16_t masked_type = static_cast<uint16_t>(
                    (static_cast<uint16_t>(data_type & 0x0Fu) << RESERVED_HEALTH_DATA_TYPE_SHIFT) &
                    RESERVED_HEALTH_DATA_TYPE_MASK);
                reserved_health = static_cast<uint16_t>(masked_channel | masked_type);
            }
        };
#pragma pack(pop)

        static_assert(sizeof(DataSpecificHeader) == 40,
                      "DataSpecificHeader size must be exactly 40 bytes");
        static_assert(offsetof(DataSpecificHeader, health_summary) == 28,
                      "DataSpecificHeader.health_summary offset must be 28");
        static_assert(offsetof(DataSpecificHeader, reserved_health) == 30,
                      "DataSpecificHeader.reserved_health offset must be 30");

#pragma pack(push, 1)
        /**
         * @brief 执行态快照
         */
        struct ExecutionSnapshot
        {
            int16_t azimuth_angle;
            int16_t elevation_angle;
            uint8_t work_freq_index;
            uint8_t reserved;
            uint16_t mgc_gain;
            uint16_t signal_bandwidth;
            uint16_t pulse_width;
            uint16_t pulse_period;
            uint16_t accumulation_count;
            int32_t tilt_sensor_x;
            int32_t tilt_sensor_y;
            uint32_t heading_angle;
            int32_t bds_longitude;
            int32_t bds_latitude;
            int32_t bds_altitude;
        };
#pragma pack(pop)

        static_assert(sizeof(ExecutionSnapshot) == 40,
                      "ExecutionSnapshot size must be exactly 40 bytes");

        /**
         * @brief 分片重组键
         */
        struct ReassemblyKey
        {
            uint16_t control_epoch;
            uint8_t source_id;
            uint32_t frame_counter;
            uint16_t beam_id;
            uint32_t cpi_count;
            uint32_t pulse_index;
            uint16_t channel_mask;
            uint8_t data_type;

            /**
             * @brief 小于比较（用于map键排序）
             * @param other 对比键
             * @return 当前键小于other返回true
             */
            bool operator<(const ReassemblyKey &other) const
            {
                if (control_epoch != other.control_epoch)
                    return control_epoch < other.control_epoch;
                if (source_id != other.source_id)
                    return source_id < other.source_id;
                if (frame_counter != other.frame_counter)
                    return frame_counter < other.frame_counter;
                if (beam_id != other.beam_id)
                    return beam_id < other.beam_id;
                if (cpi_count != other.cpi_count)
                    return cpi_count < other.cpi_count;
                if (pulse_index != other.pulse_index)
                    return pulse_index < other.pulse_index;
                if (channel_mask != other.channel_mask)
                    return channel_mask < other.channel_mask;
                return data_type < other.data_type;
            }

            /**
             * @brief 等值比较
             * @param other 对比键
             * @return 字段完全一致返回true
             */
            bool operator==(const ReassemblyKey &other) const
            {
                return control_epoch == other.control_epoch &&
                       source_id == other.source_id &&
                       frame_counter == other.frame_counter &&
                       beam_id == other.beam_id &&
                       cpi_count == other.cpi_count &&
                       pulse_index == other.pulse_index &&
                       channel_mask == other.channel_mask &&
                       data_type == other.data_type;
            }
        };

        struct ReassemblyKeyHash
        {
            size_t operator()(const ReassemblyKey &key) const noexcept
            {
                size_t h = std::hash<uint16_t>{}(key.control_epoch);
                h ^= std::hash<uint8_t>{}(key.source_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(key.frame_counter) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(key.beam_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(key.cpi_count) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(key.pulse_index) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(key.channel_mask) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint8_t>{}(key.data_type) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                return h;
            }
        };

        /**
         * @brief 校验结果枚举
         */
        enum class ValidationResult
        {
            OK = 0,
            INVALID_MAGIC,
            INVALID_VERSION,
            INVALID_DEST_ID,
            PAYLOAD_LEN_MISMATCH,
            UNSUPPORTED_PACKET_TYPE,
            CRC_MISMATCH,
            MALFORMED_PAYLOAD,
            RESERVED_FIELD_WARNING
        };

        /**
         * @brief 丢包原因枚举
         */
        enum class DropReason
        {
            INVALID_MAGIC,
            VERSION_MISMATCH,
            DEST_ID_MISMATCH,
            LENGTH_MISMATCH,
            NON_DATA_PACKET,
            SEQUENCE_DUPLICATE,
            SEQUENCE_OUT_OF_WINDOW,
            BUFFER_OVERFLOW,
            TIMEOUT,
            CLOCK_UNLOCKED,
            LATE_FRAGMENT,
            REASM_DUPLICATE_FRAG,
            REASM_TIMEOUT,
            MAX_CONTEXTS_EXCEEDED,
            REASM_BYTES_OVERFLOW
        };

        /**
         * @brief 丢包原因转字符串
         * @param reason 丢包原因
         * @return 丢包原因字符串
         */
        inline const char *to_string(DropReason reason)
        {
            switch (reason)
            {
            case DropReason::INVALID_MAGIC:
                return "INVALID_MAGIC";
            case DropReason::VERSION_MISMATCH:
                return "VERSION_MISMATCH";
            case DropReason::DEST_ID_MISMATCH:
                return "DEST_ID_MISMATCH";
            case DropReason::LENGTH_MISMATCH:
                return "LENGTH_MISMATCH";
            case DropReason::NON_DATA_PACKET:
                return "NON_DATA_PACKET";
            case DropReason::SEQUENCE_DUPLICATE:
                return "SEQUENCE_DUPLICATE";
            case DropReason::SEQUENCE_OUT_OF_WINDOW:
                return "SEQUENCE_OUT_OF_WINDOW";
            case DropReason::BUFFER_OVERFLOW:
                return "BUFFER_OVERFLOW";
            case DropReason::TIMEOUT:
                return "TIMEOUT";
            case DropReason::CLOCK_UNLOCKED:
                return "CLOCK_UNLOCKED";
            case DropReason::LATE_FRAGMENT:
                return "LATE_FRAGMENT";
            case DropReason::REASM_DUPLICATE_FRAG:
                return "REASM_DUPLICATE_FRAG";
            case DropReason::REASM_TIMEOUT:
                return "REASM_TIMEOUT";
            case DropReason::MAX_CONTEXTS_EXCEEDED:
                return "MAX_CONTEXTS_EXCEEDED";
            case DropReason::REASM_BYTES_OVERFLOW:
                return "REASM_BYTES_OVERFLOW";
            default:
                return "UNKNOWN";
            }
        }

        /**
         * @brief 判断序列号是否更新（支持回绕）
         * @param seq_new 新序列号
         * @param seq_old 旧序列号
         * @return 新序列号更新返回true
         */
        inline bool is_sequence_newer(uint32_t seq_new, uint32_t seq_old)
        {
            uint32_t delta = seq_new - seq_old;
            return delta > 0 && delta < (1u << 31);
        }

        inline bool is_epoch_newer(uint16_t epoch_new, uint16_t epoch_old)
        {
            const uint16_t delta = static_cast<uint16_t>(epoch_new - epoch_old);
            return delta > 0 && delta < static_cast<uint16_t>(1u << 15);
        }

#pragma pack(push, 1)
        /**
         * @brief 控制表头
         */
        struct ControlTableHeader
        {
            uint32_t frame_counter;
            uint16_t frame_beam_total;
            uint16_t beams_per_second;
            uint32_t cpi_count_base;
        };
#pragma pack(pop)

        static_assert(sizeof(ControlTableHeader) == CONTROL_TABLE_HEADER_SIZE,
                      "ControlTableHeader size must be exactly 12 bytes");

#pragma pack(push, 1)
        /**
         * @brief 控制表波位项
         */
        struct ControlBeamItem
        {
            uint16_t beam_id;
            uint8_t beam_work_status;
            uint8_t antenna_broadening_sel;
            int16_t azimuth_angle;
            int16_t elevation_angle;
            uint8_t work_freq_index;
            uint16_t mgc_gain;
            uint8_t period_combo_mode;
            uint8_t long_code_waveform;
            uint8_t short_code_waveform;
            uint16_t signal_bandwidth;
            uint16_t pulse_width;
            uint16_t pulse_period;
            uint16_t accumulation_count;
            uint16_t sim_target_range;
            int16_t sim_target_velocity;
            uint16_t short_code_sample_count;
            uint16_t long_code_sample_count;
            uint8_t data_rate_mbps;
            uint8_t reserved;
        };
#pragma pack(pop)

        static_assert(sizeof(ControlBeamItem) == CONTROL_BEAM_ITEM_SIZE,
                      "ControlBeamItem size must be exactly 32 bytes");

#pragma pack(push, 1)
        /**
         * @brief 调度应用应答载荷
         */
        struct ScheduleApplyAckPayload
        {
            uint32_t ack_frame_counter;
            uint32_t acked_sequence_number;
            uint8_t ack_result;
            uint8_t error_code;
            uint16_t error_detail;
            uint64_t applied_timestamp;
            uint32_t payload_crc32c;
        };
#pragma pack(pop)

        static_assert(sizeof(ScheduleApplyAckPayload) == SCHEDULE_APPLY_ACK_PAYLOAD_SIZE,
                      "ScheduleApplyAckPayload size must be exactly 24 bytes");

        enum class ControlSessionState : uint8_t
        {
            UNINITIALIZED = 0,
            ACTIVE = 1
        };

        struct DupCmdKey
        {
            uint8_t source_id{0};
            uint16_t control_epoch{0};
            uint32_t sequence_number{0};

            bool operator==(const DupCmdKey &other) const
            {
                return source_id == other.source_id &&
                       control_epoch == other.control_epoch &&
                       sequence_number == other.sequence_number;
            }
        };

        struct DupCmdKeyHasher
        {
            size_t operator()(const DupCmdKey &key) const noexcept
            {
                size_t h = std::hash<uint8_t>{}(key.source_id);
                h ^= std::hash<uint16_t>{}(key.control_epoch) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(key.sequence_number) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                return h;
            }
        };

        struct DupCmdCacheEntry
        {
            DupCmdKey key{};
            ScheduleApplyAckPayload last_result{};
            uint64_t cached_at_ms{0};
        };

        class DupCmdCache
        {
        public:
            explicit DupCmdCache(size_t max_entries = 1000)
                : max_entries_(max_entries == 0 ? 1 : max_entries)
            {
            }

            bool find(const DupCmdKey &key, ScheduleApplyAckPayload &ack_out) const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = entries_.find(key);
                if (it == entries_.end())
                {
                    return false;
                }
                ack_out = it->second.last_result;
                return true;
            }

            void upsert(const DupCmdKey &key, const ScheduleApplyAckPayload &ack, uint64_t now_ms)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = entries_.find(key);
                if (it != entries_.end())
                {
                    it->second.last_result = ack;
                    it->second.cached_at_ms = now_ms;
                    return;
                }

                if (entries_.size() >= max_entries_ && !fifo_.empty())
                {
                    const DupCmdKey evict_key = fifo_.front();
                    fifo_.pop_front();
                    entries_.erase(evict_key);
                }

                DupCmdCacheEntry entry{};
                entry.key = key;
                entry.last_result = ack;
                entry.cached_at_ms = now_ms;
                entries_.emplace(key, entry);
                fifo_.push_back(key);
            }

            void clear()
            {
                std::lock_guard<std::mutex> lock(mutex_);
                entries_.clear();
                fifo_.clear();
            }

            void set_max_entries(size_t max_entries)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                max_entries_ = (max_entries == 0) ? 1 : max_entries;
                while (entries_.size() > max_entries_ && !fifo_.empty())
                {
                    const DupCmdKey evict_key = fifo_.front();
                    fifo_.pop_front();
                    entries_.erase(evict_key);
                }
            }

            size_t size() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return entries_.size();
            }

            size_t max_entries() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return max_entries_;
            }

        private:
            mutable std::mutex mutex_;
            size_t max_entries_;
            std::unordered_map<DupCmdKey, DupCmdCacheEntry, DupCmdKeyHasher> entries_;
            std::deque<DupCmdKey> fifo_;
        };

#pragma pack(push, 1)
        /**
         * @brief 心跳载荷
         */
        struct HeartbeatPayload
        {
            uint32_t system_status_alive;
            uint32_t system_status_state;
            int16_t core_temp;
            uint8_t op_mode;
            uint8_t time_coord_source;
            uint8_t device_number[4];
            int32_t time_offset;
            uint32_t error_counters[4];
            uint8_t lo_lock_flags;
            uint8_t supply_current;
            uint8_t tr_comm_status;
            uint8_t reserved_align2;
            uint16_t voltage_stability_flags;
            int16_t panel_temp;
            uint32_t crc32c;
        };
#pragma pack(pop)

        static_assert(sizeof(HeartbeatPayload) == HEARTBEAT_PAYLOAD_SIZE,
                      "HeartbeatPayload size must be exactly 48 bytes");

#pragma pack(push, 1)
        /**
         * @brief 远程维护报文头
         */
        struct RmaHeader
        {
            uint32_t session_id;
            uint64_t session_token;
            uint8_t cmd_type;
            uint8_t cmd_flags;
            uint16_t cmd_seq;
        };
#pragma pack(pop)

        static_assert(sizeof(RmaHeader) == RMA_HEADER_SIZE,
                      "RmaHeader size must be exactly 16 bytes");

    } // namespace protocol
} // namespace receiver

#endif // RECEIVER_PROTOCOL_TYPES_H
