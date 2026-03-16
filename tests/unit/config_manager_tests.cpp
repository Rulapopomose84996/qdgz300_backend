#include "qdgz300/m01_receiver/config/config_manager.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

using receiver::config::ConfigManager;
using receiver::config::ReceiverConfig;

namespace
{
    std::string make_temp_yaml_path(const std::string &name)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "/tmp/receiver_cfg_" + name + "_" + std::to_string(now) + ".yaml";
    }
}

TEST(ConfigManagerTests, LoadValidConfig)
{
    const auto path = make_temp_yaml_path("valid");
    std::ofstream ofs(path, std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "network:\n";
    ofs << "  listen_port: 12001\n";
    ofs << "  bind_ips: [127.0.0.1, 127.0.0.2, 127.0.0.3]\n";
    ofs << "  bind_ip: 127.0.0.1\n";
    ofs << "  local_device_id: \"0x11\"\n";
    ofs << "  source_id_map: [\"0x11\", \"0x12\", \"0x13\"]\n";
    ofs << "  cpu_affinity_map: [16, 17, 18]\n";
    ofs << "  source_filter_enabled: true\n";
    ofs << "  recv_threads: 2\n";
    ofs << "reassembly:\n";
    ofs << "  max_contexts: 2048\n";
    ofs << "control_reliability:\n";
    ofs << "  rto_ms: 2500\n";
    ofs << "  max_retry: 3\n";
    ofs << "  dup_cmd_cache_size: 1000\n";
    ofs << "flow_control:\n";
    ofs << "  max_buffer_size: 200\n";
    ofs << "  high_watermark: 150\n";
    ofs << "  low_watermark: 50\n";
    ofs << "  policy: REJECT_NEW\n";
    ofs << "capture:\n";
    ofs << "  enabled: true\n";
    ofs << "  spool_dir: /tmp/receiver-spool\n";
    ofs << "  archive_dir: /data/receiver-archive\n";
    ofs << "  archive_max_files: 12\n";
    ofs << "  archive_max_age_days: 14\n";
    ofs << "  max_file_size_mb: 128\n";
    ofs << "  max_files: 5\n";
    ofs << "  filter_packet_types: [\"0x03\", \"0x04\"]\n";
    ofs << "  filter_source_ids: [\"0x01\", \"0x11\"]\n";
    ofs << "queue:\n";
    ofs << "  rawcpi_q_capacity: 128\n";
    ofs << "  rawcpi_q_slot_size_mb: 4\n";
    ofs << "consumer:\n";
    ofs << "  print_summary: false\n";
    ofs << "  write_to_file: true\n";
    ofs << "  output_dir: /tmp/test_consumer_output\n";
    ofs << "  stats_interval_ms: 2000\n";
    ofs.close();

    auto &manager = ConfigManager::instance();
    ASSERT_TRUE(manager.load_from_file(path));
    const auto &cfg = manager.get_config();

    EXPECT_EQ(cfg.network.listen_port, 12001);
    EXPECT_EQ(cfg.network.bind_ip, "127.0.0.1");
    EXPECT_EQ(cfg.network.local_device_id, 0x11);
    EXPECT_EQ(cfg.network.recv_threads, 2);
    ASSERT_EQ(cfg.network.bind_ips.size(), 3u);
    EXPECT_EQ(cfg.network.bind_ips[0], "127.0.0.1");
    EXPECT_EQ(cfg.network.bind_ips[1], "127.0.0.2");
    EXPECT_EQ(cfg.network.bind_ips[2], "127.0.0.3");
    ASSERT_EQ(cfg.network.source_id_map.size(), 3u);
    EXPECT_EQ(cfg.network.source_id_map[0], 0x11);
    EXPECT_EQ(cfg.network.source_id_map[1], 0x12);
    EXPECT_EQ(cfg.network.source_id_map[2], 0x13);
    ASSERT_EQ(cfg.network.cpu_affinity_map.size(), 3u);
    EXPECT_EQ(cfg.network.cpu_affinity_map[0], 16);
    EXPECT_EQ(cfg.network.cpu_affinity_map[1], 17);
    EXPECT_EQ(cfg.network.cpu_affinity_map[2], 18);
    EXPECT_TRUE(cfg.network.source_filter_enabled);
    EXPECT_EQ(cfg.reassembly.max_contexts, 2048u);
    EXPECT_EQ(cfg.control_reliability.rto_ms, 2500u);
    EXPECT_EQ(cfg.control_reliability.max_retry, 3u);
    EXPECT_EQ(cfg.control_reliability.dup_cmd_cache_size, 1000u);
    EXPECT_EQ(cfg.flow_control.policy, "REJECT_NEW");
    EXPECT_TRUE(cfg.capture.enabled);
    EXPECT_EQ(cfg.capture.spool_dir, "/tmp/receiver-spool");
    EXPECT_EQ(cfg.capture.archive_dir, "/data/receiver-archive");
    EXPECT_EQ(cfg.capture.archive_max_files, 12u);
    EXPECT_EQ(cfg.capture.archive_max_age_days, 14u);
    EXPECT_EQ(cfg.capture.max_file_size_mb, 128u);
    EXPECT_EQ(cfg.capture.max_files, 5u);
    ASSERT_EQ(cfg.capture.filter_packet_types.size(), 2u);
    EXPECT_EQ(cfg.capture.filter_packet_types[0], 0x03);
    EXPECT_EQ(cfg.capture.filter_packet_types[1], 0x04);
    ASSERT_EQ(cfg.capture.filter_source_ids.size(), 2u);
    EXPECT_EQ(cfg.capture.filter_source_ids[0], 0x01);
    EXPECT_EQ(cfg.capture.filter_source_ids[1], 0x11);
    EXPECT_EQ(cfg.queue.rawcpi_q_capacity, 128u);
    EXPECT_EQ(cfg.queue.rawcpi_q_slot_size_mb, 4u);
    EXPECT_FALSE(cfg.consumer.print_summary);
    EXPECT_TRUE(cfg.consumer.write_to_file);
    EXPECT_EQ(cfg.consumer.output_dir, "/tmp/test_consumer_output");
    EXPECT_EQ(cfg.consumer.stats_interval_ms, 2000u);

    (void)std::remove(path.c_str());
}

TEST(ConfigManagerTests, LoadMissingFile)
{
    const auto path = make_temp_yaml_path("missing");
    auto &manager = ConfigManager::instance();
    EXPECT_FALSE(manager.load_from_file(path));
}

TEST(ConfigManagerTests, LoadMalformedYaml)
{
    const auto path = make_temp_yaml_path("malformed");
    std::ofstream ofs(path, std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "network:\n";
    ofs << "  listen_port: [10001\n";
    ofs.close();

    auto &manager = ConfigManager::instance();
    EXPECT_FALSE(manager.load_from_file(path));

    (void)std::remove(path.c_str());
}

TEST(ConfigManagerTests, ValidateGoodConfig)
{
    ReceiverConfig cfg;
    cfg.network.listen_port = 9999;
    cfg.network.recv_threads = 2;
    cfg.reassembly.max_total_frags = 1024;
    cfg.reassembly.max_reasm_bytes_per_key = 1024 * 1024;
    cfg.reorder.window_size = 64;
    cfg.control_reliability.rto_ms = 2500;
    cfg.control_reliability.max_retry = 3;
    cfg.control_reliability.dup_cmd_cache_size = 1000;
    cfg.flow_control.max_buffer_size = 1000;
    cfg.flow_control.high_watermark = 800;
    cfg.flow_control.low_watermark = 200;
    cfg.flow_control.policy = "DROP_OLDEST";
    cfg.delivery.reconnect_interval_ms = 100;
    cfg.capture.max_files = 10;
    EXPECT_TRUE(ConfigManager::validate(cfg));

    cfg.capture.archive_max_files = 0;
    EXPECT_FALSE(ConfigManager::validate(cfg));
    cfg.capture.archive_max_files = 256;
    cfg.capture.archive_max_age_days = 0;
    EXPECT_FALSE(ConfigManager::validate(cfg));
}

TEST(ConfigManagerTests, ValidateBadConfig)
{
    ReceiverConfig cfg;
    cfg.network.listen_port = 0;
    cfg.reassembly.max_contexts = 0;
    cfg.flow_control.policy = "INVALID_POLICY";
    EXPECT_FALSE(ConfigManager::validate(cfg));
}

TEST(ConfigManagerTests, ValidateRejectsPartialArrayFaceConfig)
{
    ReceiverConfig cfg;
    cfg.network.bind_ips = {"127.0.0.1", "127.0.0.2", "127.0.0.3"};
    cfg.network.source_id_map = {0x11, 0x12}; // invalid size
    cfg.network.cpu_affinity_map = {16, 17, 18};
    EXPECT_FALSE(ConfigManager::validate(cfg));
}

TEST(ConfigManagerTests, DefaultValues)
{
    ReceiverConfig cfg;
    EXPECT_EQ(cfg.network.listen_port, 9999);
    EXPECT_EQ(cfg.network.local_device_id, 0x01);
    EXPECT_EQ(cfg.network.bind_ip, "0.0.0.0");
    EXPECT_TRUE(cfg.network.bind_ips.empty());
    EXPECT_TRUE(cfg.network.source_id_map.empty());
    EXPECT_TRUE(cfg.network.cpu_affinity_map.empty());
    EXPECT_TRUE(cfg.network.source_filter_enabled);
    EXPECT_EQ(cfg.reorder.window_size, 512u);
    EXPECT_EQ(cfg.flow_control.policy, "DROP_OLDEST");
    EXPECT_FALSE(cfg.capture.enabled);
    EXPECT_EQ(cfg.capture.max_files, 10u);
    EXPECT_EQ(cfg.capture.spool_dir, "/var/spool/qdgz300/receiver");
    EXPECT_EQ(cfg.capture.archive_dir, "/data/qdgz300/receiver");
    EXPECT_EQ(cfg.capture.archive_max_files, 256u);
    EXPECT_EQ(cfg.capture.archive_max_age_days, 7u);
    EXPECT_EQ(cfg.control_reliability.rto_ms, 2500u);
    EXPECT_EQ(cfg.control_reliability.max_retry, 3u);
    EXPECT_EQ(cfg.control_reliability.dup_cmd_cache_size, 1000u);
    EXPECT_EQ(cfg.queue.rawcpi_q_capacity, 64u);
    EXPECT_EQ(cfg.queue.rawcpi_q_slot_size_mb, 2u);
    EXPECT_TRUE(cfg.consumer.print_summary);
    EXPECT_FALSE(cfg.consumer.write_to_file);
    EXPECT_EQ(cfg.consumer.output_dir, "/tmp/receiver_rawblocks");
    EXPECT_EQ(cfg.consumer.stats_interval_ms, 1000u);
}

TEST(ConfigManagerTests, ValidateQueueConfig)
{
    ReceiverConfig cfg;
    cfg.network.listen_port = 9999;
    cfg.queue.rawcpi_q_capacity = 0; // invalid
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.queue.rawcpi_q_capacity = 2000; // too large
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.queue.rawcpi_q_capacity = 64;    // valid
    cfg.queue.rawcpi_q_slot_size_mb = 0; // invalid
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.queue.rawcpi_q_slot_size_mb = 20; // too large
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.queue.rawcpi_q_slot_size_mb = 2; // valid
    EXPECT_TRUE(ConfigManager::validate(cfg));
}

TEST(ConfigManagerTests, ValidateConsumerConfig)
{
    ReceiverConfig cfg;
    cfg.network.listen_port = 9999;
    cfg.consumer.stats_interval_ms = 0; // invalid
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.consumer.stats_interval_ms = 100000; // too large
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.consumer.stats_interval_ms = 1000; // valid
    cfg.consumer.write_to_file = true;
    cfg.consumer.output_dir = ""; // invalid when write enabled
    EXPECT_FALSE(ConfigManager::validate(cfg));

    cfg.consumer.output_dir = "/tmp/output"; // valid
    EXPECT_TRUE(ConfigManager::validate(cfg));
}

TEST(ConfigManagerTests, LoadLegacyCaptureOutputDirAsSpoolDir)
{
    const auto path = make_temp_yaml_path("legacy_capture_dir");
    std::ofstream ofs(path, std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "network:\n";
    ofs << "  listen_port: 12001\n";
    ofs << "  bind_ips: [127.0.0.1, 127.0.0.2, 127.0.0.3]\n";
    ofs << "  source_id_map: [\"0x11\", \"0x12\", \"0x13\"]\n";
    ofs << "  cpu_affinity_map: [16, 17, 18]\n";
    ofs << "capture:\n";
    ofs << "  enabled: true\n";
    ofs << "  output_dir: /tmp/legacy-spool\n";
    ofs.close();

    auto &manager = ConfigManager::instance();
    ASSERT_TRUE(manager.load_from_file(path));
    const auto &cfg = manager.get_config();
    EXPECT_EQ(cfg.capture.spool_dir, "/tmp/legacy-spool");

    (void)std::remove(path.c_str());
}
