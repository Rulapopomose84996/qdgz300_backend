#ifndef RECEIVER_PROTOCOL_PACKET_PARSER_H
#define RECEIVER_PROTOCOL_PACKET_PARSER_H

#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include <optional>
#include <cstddef>

namespace receiver
{
    namespace protocol
    {

        /**
         * @brief 报文解析器
         *
         * 职责：
         * - 解析UDP载荷中的32字节通用报文头
         * - 执行基础字段合法性检查（Magic、Version）
         * - 提取载荷指针（零拷贝）
         */
        class PacketParser
        {
        public:
            /**
             * @brief 构造报文解析器
             */
            PacketParser() = default;

            /**
             * @brief 析构报文解析器
             */
            ~PacketParser() = default;

            /**
             * @brief 解析UDP原始数据包
             * @param buffer 原始UDP数据缓冲区
             * @param length 数据包总长度
             * @return 解析成功返回ParsedPacket，失败返回std::nullopt
             */
            std::optional<ParsedPacket> parse(const uint8_t *buffer, size_t length);

            /**
             * @brief 快速检查Magic（用于快速路径优化）
             * @param buffer 原始数据缓冲区
             * @param length 数据长度
             * @return Magic匹配返回true
             */
            static bool quick_check_magic(const uint8_t *buffer, size_t length);

        private:
            /**
             * @brief 解析通用报文头（小端序）
             * @param buffer 报文缓冲区
             * @param header 输出参数：解析后的header结构
             */
            void parse_common_header(const uint8_t *buffer, CommonHeader &header);

            /**
             * @brief 基础合法性检查
             * @param header 已解析的header
             * @param total_length 数据包总长度
             * @return 合法返回true
             */
            bool basic_validation(const CommonHeader &header, size_t total_length);
        };

    } // namespace protocol
} // namespace receiver

#endif // RECEIVER_PROTOCOL_PACKET_PARSER_H
