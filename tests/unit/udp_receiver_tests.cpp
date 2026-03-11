#include "qdgz300/m01_receiver/network/udp_receiver.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using receiver::network::ReceivedPacket;
using receiver::network::UdpReceiver;
using receiver::network::UdpReceiverConfig;

namespace
{
    std::vector<uint8_t> make_minimal_packet(uint8_t source_id)
    {
        std::vector<uint8_t> payload(receiver::protocol::COMMON_HEADER_SIZE, 0);
        payload[offsetof(receiver::protocol::CommonHeader, source_id)] = source_id;
        payload[offsetof(receiver::protocol::CommonHeader, packet_type)] =
            static_cast<uint8_t>(receiver::protocol::PacketType::DATA);
        return payload;
    }

    uint16_t reserve_free_udp_port()
    {
        const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            return 0;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            close(sock);
            return 0;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &len) < 0)
        {
            close(sock);
            return 0;
        }

        close(sock);
        return ntohs(bound.sin_port);
    }

    bool send_udp_to_loopback(uint16_t port, const std::vector<uint8_t> &payload)
    {
        const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            return false;
        }

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &target.sin_addr) != 1)
        {
            close(sock);
            return false;
        }

        const int sent = static_cast<int>(::sendto(
            sock,
            reinterpret_cast<const char *>(payload.data()),
            static_cast<int>(payload.size()),
            0,
            reinterpret_cast<const sockaddr *>(&target),
            sizeof(target)));

        close(sock);
        return sent == static_cast<int>(payload.size());
    }
} // namespace

TEST(UdpReceiverTests, StartAndStop)
{
    UdpReceiverConfig cfg{};
    cfg.listen_port = reserve_free_udp_port();
    ASSERT_NE(cfg.listen_port, 0);
    cfg.bind_ip = "127.0.0.1";
    cfg.worker_threads = 1;
    cfg.recv_batch_size = 8;
    cfg.enable_so_reuseport = false;

    UdpReceiver receiver(cfg, [](ReceivedPacket &&) {});
    const bool started = receiver.start();
    EXPECT_TRUE(started);
    receiver.stop();
}

TEST(UdpReceiverTests, ReceivePacketViaLoopback)
{
    std::mutex mu;
    std::condition_variable cv;
    bool got_packet = false;
    std::vector<uint8_t> received;

    UdpReceiverConfig cfg{};
    cfg.listen_port = reserve_free_udp_port();
    ASSERT_NE(cfg.listen_port, 0);
    cfg.bind_ip = "127.0.0.1";
    cfg.worker_threads = 1;
    cfg.recv_batch_size = 1;
    cfg.enable_so_reuseport = false;

    UdpReceiver receiver(cfg, [&](ReceivedPacket &&pkt)
                         {
                             std::lock_guard<std::mutex> lock(mu);
                             got_packet = true;
                             received.assign(pkt.data.get(), pkt.data.get() + pkt.length);
                             cv.notify_one();
                         });
    ASSERT_TRUE(receiver.start());

    const std::vector<uint8_t> payload = {'t', 'e', 's', 't', '1'};
    ASSERT_TRUE(send_udp_to_loopback(cfg.listen_port, payload));

    std::unique_lock<std::mutex> lock(mu);
    const bool ok = cv.wait_for(lock, std::chrono::seconds(2), [&]() { return got_packet; });
    receiver.stop();

    ASSERT_TRUE(ok);
    EXPECT_EQ(received, payload);
}

TEST(UdpReceiverTests, StatisticsIncrement)
{
    UdpReceiverConfig cfg{};
    cfg.listen_port = reserve_free_udp_port();
    ASSERT_NE(cfg.listen_port, 0);
    cfg.bind_ip = "127.0.0.1";
    cfg.worker_threads = 1;
    cfg.recv_batch_size = 4;
    cfg.enable_so_reuseport = false;

    UdpReceiver receiver(cfg, [](ReceivedPacket &&) {});
    ASSERT_TRUE(receiver.start());

    const std::vector<uint8_t> payload = {'p', 'k', 't'};
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(send_udp_to_loopback(cfg.listen_port, payload));
    }

    bool reached = false;
    for (int i = 0; i < 40; ++i)
    {
        const uint64_t count = receiver.get_statistics().packets_received.load(std::memory_order_relaxed);
        if (count >= 10)
        {
            reached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    receiver.stop();

    EXPECT_TRUE(reached);
}

TEST(UdpReceiverTests, InvalidPort)
{
    const uint16_t occupied_port = reserve_free_udp_port();
    ASSERT_NE(occupied_port, 0);

    const int holder = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(holder, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(occupied_port);
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(::bind(holder, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);

    UdpReceiverConfig cfg{};
    cfg.listen_port = occupied_port;
    cfg.bind_ip = "127.0.0.1";
    cfg.worker_threads = 1;
    cfg.recv_batch_size = 1;
    cfg.enable_so_reuseport = false;

    UdpReceiver receiver(cfg, [](ReceivedPacket &&) {});
    const bool started = receiver.start();

    bool graceful_fail = !started;
    for (int i = 0; i < 20 && !graceful_fail; ++i)
    {
        if (receiver.get_statistics().recv_errors.load(std::memory_order_relaxed) > 0)
        {
            graceful_fail = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    receiver.stop();
    close(holder);
    EXPECT_TRUE(graceful_fail);
}

TEST(UdpReceiverTests, ArrayFaceSourceFilterRoutesBySourceId)
{
    const uint16_t port1 = reserve_free_udp_port();
    const uint16_t port2 = reserve_free_udp_port();
    const uint16_t port3 = reserve_free_udp_port();
    ASSERT_NE(port1, 0);
    ASSERT_NE(port2, 0);
    ASSERT_NE(port3, 0);

    std::mutex mu;
    std::condition_variable cv;
    bool got_packet = false;
    uint8_t array_id = 0;

    UdpReceiverConfig cfg{};
    cfg.bind_ip = "127.0.0.1";
    cfg.array_faces = {
        receiver::network::ArrayFaceBinding{1, 0x11, "127.0.0.1", port1, 0, true},
        receiver::network::ArrayFaceBinding{2, 0x12, "127.0.0.1", port2, 0, true},
        receiver::network::ArrayFaceBinding{3, 0x13, "127.0.0.1", port3, 0, true}};

    UdpReceiver receiver(cfg, [&](ReceivedPacket &&pkt)
                         {
                             std::lock_guard<std::mutex> lock(mu);
                             got_packet = true;
                             array_id = pkt.array_id;
                             cv.notify_one();
                         });
    ASSERT_TRUE(receiver.start());

    ASSERT_TRUE(send_udp_to_loopback(port1, make_minimal_packet(0x12)));
    ASSERT_TRUE(send_udp_to_loopback(port1, make_minimal_packet(0x11)));

    std::unique_lock<std::mutex> lock(mu);
    const bool ok = cv.wait_for(lock, std::chrono::seconds(2), [&]() { return got_packet; });
    receiver.stop();

    ASSERT_TRUE(ok);
    EXPECT_EQ(array_id, 1);
    EXPECT_GE(receiver.get_statistics().packets_filtered.load(std::memory_order_relaxed), 1u);
}
