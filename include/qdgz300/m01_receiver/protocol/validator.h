#ifndef RECEIVER_PROTOCOL_VALIDATOR_H
#define RECEIVER_PROTOCOL_VALIDATOR_H

#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include <cstdint>

namespace receiver
{
    namespace protocol
    {

        /**
         * @brief 报文完整性校验器
         *
         * 职责：
         * - 协议字段合规性检查（DestID、PacketType、PayloadLen）
         * - 保留字段检查（MBZ放宽策略：非零仅告警，不丢包）
         * - 统计各类校验失败原因
         */
        class Validator
        {
        public:
            enum class Scope
            {
                /** @brief 仅校验数据平面报文（PacketType=0x03） */
                DATA_PLANE_ONLY,
                /** @brief 校验Data+Heartbeat（PacketType=0x03/0x04） */
                DATA_AND_HEARTBEAT,
                /** @brief 兼容别名：阶段二后等同 DATA_AND_HEARTBEAT */
                FULL_PROTOCOL = DATA_AND_HEARTBEAT
            };

            /**
             * @brief 构造函数
             * @param local_device_id 本地设备ID（用于DestID匹配）
             * @param scope 校验范围策略
             */
            explicit Validator(uint8_t local_device_id, Scope scope = Scope::DATA_AND_HEARTBEAT);

            /**
             * @brief 析构校验器
             */
            ~Validator() = default;

            /**
             * @brief 校验已解析的数据包
             * @param packet 已解析的数据包
             * @return 校验结果
             */
            ValidationResult validate(const ParsedPacket &packet);

            /**
             * @brief 检查DestID是否匹配本机或广播地址
             * @param dest_id 目标设备ID
             * @return 匹配返回true
             */
            bool check_dest_id(uint8_t dest_id) const;

            /**
             * @brief 检查PacketType是否为数据平面包（0x03）
             * @param type 报文类型
             * @return 是数据包返回true
             */
            bool is_data_packet(PacketType type) const;

            /**
             * @brief 检查保留字段（MBZ放宽策略）
             * @param header 报文头
             * @return 保留字段全零返回true，否则返回false（但仅告警）
             */
            bool check_reserved_fields(const CommonHeader &header) const;

        private:
            uint8_t local_device_id_;
            Scope scope_;

            ValidationResult validate_payload_by_type(const ParsedPacket &packet);
        };

    } // namespace protocol
} // namespace receiver

#endif // RECEIVER_PROTOCOL_VALIDATOR_H
