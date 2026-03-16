#include "qdgz300/m01_receiver/monitoring/metrics.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    std::string make_temp_metrics_path()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "/tmp/receiver_external_metrics_" + std::to_string(now) + ".prom";
    }

    std::string http_get_metrics(uint16_t port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return {};
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 500 * 1000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
        {
            close(fd);
            return {};
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
        {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            if (errno != EINPROGRESS)
            {
                close(fd);
                return {};
            }

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const int sel = select(fd + 1, nullptr, &wfds, nullptr, &timeout);
            if (sel <= 0)
            {
                close(fd);
                return {};
            }

            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0 || so_error != 0)
            {
                close(fd);
                return {};
            }
        }

        if (flags >= 0)
        {
            (void)fcntl(fd, F_SETFL, flags);
        }

        static const char kReq[] =
            "GET /metrics HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n";
        (void)send(fd, kReq, sizeof(kReq) - 1, 0);

        std::string response;
        char buf[4096];
        while (true)
        {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            response.append(buf, buf + n);
        }
        close(fd);

        const std::string::size_type body_pos = response.find("\r\n\r\n");
        if (body_pos == std::string::npos)
        {
            return response;
        }
        return response.substr(body_pos + 4);
    }
}

TEST(MetricsTests, ExposesRequiredPrometheusMetrics)
{
    const std::string external_metrics = make_temp_metrics_path();
    {
        std::ofstream ofs(external_metrics, std::ios::trunc);
        ASSERT_TRUE(ofs.is_open());
        ofs << "# HELP qdgz300_spool_mover_archived_total Total sealed pcap files archived from spool to archive.\n";
        ofs << "# TYPE qdgz300_spool_mover_archived_total counter\n";
        ofs << "qdgz300_spool_mover_archived_total 3\n";
    }
    ASSERT_EQ(setenv("QDGZ300_EXTERNAL_METRICS_FILE", external_metrics.c_str(), 1), 0);

    auto &metrics = receiver::monitoring::MetricsCollector::instance();
    const uint16_t port = 18081;
    metrics.initialize(port, "127.0.0.1");
    ASSERT_TRUE(metrics.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    metrics.increment_packets_received(3);
    metrics.increment_packets_received_by_type(receiver::monitoring::PacketTypeIndex::DATA, 2);
    metrics.increment_packets_dropped_by_reason(receiver::monitoring::DropReasonIndex::LENGTH_MISMATCH, 1);
    metrics.set_active_reorder_contexts(12);
    metrics.set_missing_fragments_total(7);
    metrics.increment_applied_late();
    metrics.increment_clock_unlocked();
    metrics.increment_config_reloads();
    metrics.increment_signal_received("SIGINT");
    metrics.increment_signal_received("SIGTERM");
    metrics.increment_signal_received("SIGHUP");
    metrics.increment_heartbeat_packets_processed(4);
    metrics.increment_heartbeat_sent();
    metrics.observe_data_delay(0.003);
    metrics.observe_data_delay(0.12);
    metrics.observe_processing_latency(600);
    metrics.observe_processing_latency(9000);
    metrics.set_uptime_seconds(12.5);
    metrics.set_memory_rss_bytes(1024 * 1024);
    metrics.set_numa_local_memory_pct(99.5);
    metrics.set_heartbeat_queue_depth(12);
    metrics.set_heartbeat_state(2);
    metrics.set_face_rx_queue_depth(1, 10);
    metrics.set_face_rx_queue_high_watermark(1, 24);
    metrics.set_face_rx_queue_drops(1, 3);
    metrics.set_face_packet_pool_stats(1, 1024, 900, 2);
    metrics.observe_packet_pool_allocation_latency_ns(800);
    metrics.observe_packet_pool_allocation_latency_ns(1200);
    metrics.collect_system_metrics();

    std::string payload;
    for (int retry = 0; retry < 10; ++retry)
    {
        payload = http_get_metrics(port);
        if (!payload.empty())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    metrics.stop();
    unsetenv("QDGZ300_EXTERNAL_METRICS_FILE");
    (void)std::remove(external_metrics.c_str());

    ASSERT_FALSE(payload.empty());
    EXPECT_NE(payload.find("receiver_packets_received_total{packet_type=\"data\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_packets_dropped_total{reason=\"LENGTH_MISMATCH\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_reasm_contexts_active"), std::string::npos);
    EXPECT_NE(payload.find("receiver_missing_fragments_total"), std::string::npos);
    EXPECT_NE(payload.find("receiver_data_delay_seconds_histogram_bucket"), std::string::npos);
    EXPECT_NE(payload.find("receiver_data_delay_seconds_p50"), std::string::npos);
    EXPECT_NE(payload.find("receiver_data_delay_seconds_p99"), std::string::npos);
    EXPECT_NE(payload.find("receiver_data_delay_seconds_p999"), std::string::npos);
    EXPECT_NE(payload.find("receiver_applied_late_total"), std::string::npos);
    EXPECT_NE(payload.find("receiver_clock_unlocked_total"), std::string::npos);
    EXPECT_NE(payload.find("receiver_config_reload_total"), std::string::npos);
    EXPECT_NE(payload.find("receiver_signal_received_total{signal=\"SIGINT\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_signal_received_total{signal=\"SIGTERM\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_signal_received_total{signal=\"SIGHUP\"}"), std::string::npos);
    EXPECT_NE(payload.find("heartbeat_packets_processed"), std::string::npos);
    EXPECT_NE(payload.find("receiver_heartbeat_sent_total"), std::string::npos);
    EXPECT_NE(payload.find("receiver_uptime_seconds"), std::string::npos);
    EXPECT_NE(payload.find("receiver_memory_rss_bytes"), std::string::npos);
    EXPECT_NE(payload.find("numa_local_memory_pct"), std::string::npos);
    EXPECT_NE(payload.find("heartbeat_queue_depth"), std::string::npos);
    EXPECT_NE(payload.find("receiver_rx_queue_depth{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_rx_queue_high_watermark{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_rx_queue_drops_total{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_packet_pool_total_buffers{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_packet_pool_available_buffers{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("receiver_packet_pool_fallback_alloc_total{face=\"1\"}"), std::string::npos);
    EXPECT_NE(payload.find("packet_pool_allocation_latency_ns_bucket"), std::string::npos);
    EXPECT_NE(payload.find("receiver_heartbeat_state"), std::string::npos);
    EXPECT_NE(payload.find("qdgz300_spool_mover_archived_total 3"), std::string::npos);
}
