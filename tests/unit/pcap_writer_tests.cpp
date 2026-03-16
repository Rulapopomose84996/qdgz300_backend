#include "qdgz300/m01_receiver/capture/pcap_writer.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

using receiver::capture::PcapWriter;
using receiver::capture::PcapWriterConfig;

namespace
{
    constexpr size_t kCommonHeaderSize = receiver::protocol::COMMON_HEADER_SIZE;

    std::string make_temp_dir(const std::string &name)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string dir = "/tmp/pcap_writer_test_" + name + "_" + std::to_string(now);
        (void)::mkdir(dir.c_str(), 0755);
        return dir;
    }

    void remove_dir_tree(const std::string &dir)
    {
        if (dir.empty())
        {
            return;
        }
        const std::string cmd = "rm -rf '" + dir + "'";
        (void)std::system(cmd.c_str());
    }

    std::string path_join(const std::string &dir, const std::string &file)
    {
        if (dir.empty())
        {
            return file;
        }
        if (dir.back() == '/')
        {
            return dir + file;
        }
        return dir + "/" + file;
    }

    std::string first_pcap_file(const std::string &dir)
    {
        DIR *dp = ::opendir(dir.c_str());
        if (dp == nullptr)
        {
            return std::string();
        }

        std::string result;
        dirent *entry = nullptr;
        while ((entry = ::readdir(dp)) != nullptr)
        {
            const std::string name(entry->d_name);
            if (name.size() >= 5 && name.substr(name.size() - 5) == ".pcap")
            {
                result = path_join(dir, name);
                break;
            }
        }

        ::closedir(dp);
        return result;
    }

    std::vector<std::string> list_pcap_files(const std::string &dir)
    {
        std::vector<std::string> files;
        DIR *dp = ::opendir(dir.c_str());
        if (dp == nullptr)
        {
            return files;
        }

        dirent *entry = nullptr;
        while ((entry = ::readdir(dp)) != nullptr)
        {
            const std::string name(entry->d_name);
            if (name.size() >= 5 && name.substr(name.size() - 5) == ".pcap")
            {
                files.push_back(path_join(dir, name));
            }
        }

        ::closedir(dp);
        return files;
    }

    std::vector<std::string> list_partial_files(const std::string &dir)
    {
        std::vector<std::string> files;
        DIR *dp = ::opendir(dir.c_str());
        if (dp == nullptr)
        {
            return files;
        }

        dirent *entry = nullptr;
        while ((entry = ::readdir(dp)) != nullptr)
        {
            const std::string name(entry->d_name);
            if (name.size() >= 10 && name.substr(name.size() - 10) == ".pcap.part")
            {
                files.push_back(path_join(dir, name));
            }
        }

        ::closedir(dp);
        return files;
    }

    uint64_t file_size_bytes(const std::string &path)
    {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
        {
            return 0u;
        }
        return static_cast<uint64_t>(st.st_size);
    }

    std::vector<uint8_t> make_raw_packet(uint8_t packet_type, uint8_t source_id, size_t length = kCommonHeaderSize)
    {
        std::vector<uint8_t> packet(length, 0);
        if (length > 18)
        {
            packet[18] = packet_type;
        }
        if (length > 20)
        {
            packet[20] = source_id;
        }
        return packet;
    }

    uint32_t read_u32_le(const std::array<uint8_t, 24> &bytes, size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset]) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
               (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
    }

    uint16_t read_u16_le(const std::array<uint8_t, 24> &bytes, size_t offset)
    {
        return static_cast<uint16_t>(bytes[offset]) |
               static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8u);
    }
} // namespace

TEST(PcapWriterTests, WriteAndReadGlobalHeader)
{
    const auto temp_dir = make_temp_dir("global_header");

    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = temp_dir;

    PcapWriter writer(cfg);
    ASSERT_TRUE(writer.start());
    writer.stop();

    const auto pcap_file = first_pcap_file(temp_dir);
    ASSERT_FALSE(pcap_file.empty());

    std::ifstream ifs(pcap_file, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    std::array<uint8_t, 24> header{};
    ifs.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    ASSERT_EQ(ifs.gcount(), static_cast<std::streamsize>(header.size()));

    EXPECT_EQ(read_u32_le(header, 0), 0xA1B2C3D4u);
    EXPECT_EQ(read_u16_le(header, 4), 2u);
    EXPECT_EQ(read_u16_le(header, 6), 4u);
    EXPECT_EQ(read_u32_le(header, 20), 147u);

    remove_dir_tree(temp_dir);
}

TEST(PcapWriterTests, WritePacketRecord)
{
    const auto temp_dir = make_temp_dir("record_size");

    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = temp_dir;

    PcapWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    const auto packet = make_raw_packet(static_cast<uint8_t>(receiver::protocol::PacketType::DATA), 0x01, 32);
    writer.write_packet(packet.data(), packet.size(), 1000000);
    writer.write_packet(packet.data(), packet.size(), 1000001);
    writer.write_packet(packet.data(), packet.size(), 1000002);
    writer.stop();

    const auto pcap_file = first_pcap_file(temp_dir);
    ASSERT_FALSE(pcap_file.empty());

    const uint64_t size = file_size_bytes(pcap_file);
    const uint64_t expected = 24u + 3u * (16u + static_cast<uint64_t>(packet.size()));
    EXPECT_EQ(size, expected);
    EXPECT_TRUE(list_partial_files(temp_dir).empty());

    remove_dir_tree(temp_dir);
}

TEST(PcapWriterTests, FileRotation)
{
    const auto temp_dir = make_temp_dir("rotation");

    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = temp_dir;
    cfg.max_file_size_mb = 0;
    cfg.max_files = 3;

    PcapWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    const auto packet = make_raw_packet(static_cast<uint8_t>(receiver::protocol::PacketType::DATA), 0x01, 32);
    for (uint64_t i = 0; i < 6; ++i)
    {
        writer.write_packet(packet.data(), packet.size(), 1000000 + i);
    }
    writer.stop();

    const auto files = list_pcap_files(temp_dir);
    EXPECT_GE(files.size(), 2u);
    EXPECT_LE(files.size(), 3u);

    remove_dir_tree(temp_dir);
}

TEST(PcapWriterTests, FilterByPacketType)
{
    const auto temp_dir = make_temp_dir("filter_type");

    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = temp_dir;
    cfg.filter_packet_types = {static_cast<uint8_t>(receiver::protocol::PacketType::DATA)};

    PcapWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    const auto control_packet = make_raw_packet(static_cast<uint8_t>(receiver::protocol::PacketType::CONTROL), 0x01, 32);
    const auto data_packet = make_raw_packet(static_cast<uint8_t>(receiver::protocol::PacketType::DATA), 0x01, 32);

    writer.write_packet(control_packet.data(), control_packet.size(), 1000000);
    writer.write_packet(data_packet.data(), data_packet.size(), 1000001);
    writer.stop();

    const auto pcap_file = first_pcap_file(temp_dir);
    ASSERT_FALSE(pcap_file.empty());

    const uint64_t size = file_size_bytes(pcap_file);
    const uint64_t expected = 24u + 16u + static_cast<uint64_t>(data_packet.size());
    EXPECT_EQ(size, expected);
    EXPECT_EQ(writer.packets_written(), 1u);

    remove_dir_tree(temp_dir);
}

TEST(PcapWriterTests, QueueFullDropsPacketsWithoutBlockingCaller)
{
    const auto temp_dir = make_temp_dir("queue_full");

    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = temp_dir;
    cfg.max_file_size_mb = 0;
    cfg.max_files = 2;
    cfg.pending_queue_capacity = 8;

    PcapWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    const auto packet = make_raw_packet(static_cast<uint8_t>(receiver::protocol::PacketType::DATA), 0x01, 1500);
    for (size_t i = 0; i < 2000; ++i)
    {
        writer.write_packet(packet.data(), packet.size(), 1000000 + i);
    }
    writer.stop();

    const auto stats = writer.get_statistics();
    EXPECT_GT(stats.dropped_queue_full, 0u);
    EXPECT_GT(stats.written_packets, 0u);

    remove_dir_tree(temp_dir);
}

TEST(PcapWriterTests, StartFailsWhenSpoolDirectoryCannotBeCreated)
{
    PcapWriterConfig cfg;
    cfg.enabled = true;
    cfg.spool_dir = "/dev/null/receiver_spool";

    PcapWriter writer(cfg);
    EXPECT_FALSE(writer.start());
}
