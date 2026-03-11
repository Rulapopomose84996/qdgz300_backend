#include "qdgz300/m04_gateway/hmi_session.h"
#include "qdgz300/m04_gateway/output_throttle.h"
#include "qdgz300/m04_gateway/tcp_server.h"
#include "qdgz300/m04_gateway/trackdata_serializer.h"
#include "qdgz300/m04_gateway/udp_sender.h"

#include <gtest/gtest.h>

using namespace qdgz300;
using namespace qdgz300::m04;

TEST(M04SerializerTest, RoundTripsTrackFrame)
{
    Track track{};
    track.track_id = 9;
    track.confidence = 0.7f;

    TrackFrame frame{};
    frame.frame_seq = 11;
    frame.data_timestamp_ns = 123;
    frame.backend_instance_id = 1;
    frame.clock_domain = 0;
    frame.coverage_mask = 0x07;
    frame.track_count = 1;
    frame.system_quality_flags = 0;
    frame.is_truncated = false;
    frame.dropped_count = 0;
    frame.tracks = &track;

    TrackDataSerializer serializer;
    auto bytes = serializer.serialize(frame);
    auto roundtrip = serializer.deserialize(bytes.data(), bytes.size());

    EXPECT_EQ(roundtrip.frame_seq, 11u);
    ASSERT_NE(roundtrip.tracks, nullptr);
    EXPECT_EQ(roundtrip.tracks[0].track_id, 9u);

    delete[] roundtrip.tracks;
}

TEST(M04ThrottleTest, TruncatesByConfidence)
{
    Track tracks[3]{};
    tracks[0].track_id = 1;
    tracks[0].confidence = 0.2f;
    tracks[1].track_id = 2;
    tracks[1].confidence = 0.9f;
    tracks[2].track_id = 3;
    tracks[2].confidence = 0.5f;

    TrackFrame frame{};
    frame.track_count = 3;
    frame.tracks = tracks;

    OutputThrottle throttle;
    auto truncated = throttle.truncate(frame, 2);

    EXPECT_TRUE(truncated.is_truncated);
    EXPECT_EQ(truncated.track_count, 2u);
    EXPECT_EQ(truncated.dropped_count, 1u);
    EXPECT_EQ(truncated.tracks[0].track_id, 2u);

    delete[] truncated.tracks;
}

TEST(M04SessionTest, MarksTimeoutSessionsDead)
{
    HmiSessionManager sessions;
    sessions.upsert(1, "peer-1", 100);
    sessions.upsert(2, "peer-2", 200);
    sessions.sweep_timeouts(430, 250);

    EXPECT_EQ(sessions.alive_count(), 1u);
}

TEST(M04TcpServerTest, InvokesCommandHandler)
{
    HmiSessionManager sessions;
    TcpServer server(7897, sessions);
    server.set_command_handler([](const std::string &payload)
                               { return CommandAck{payload == "START", 0u, {}}; });

    auto ack = server.handle_command_for_test("START");
    EXPECT_TRUE(ack.success);
}

TEST(M04UdpSenderTest, SerializesAndCachesPayload)
{
    UdpSender sender(9998);
    TrackFrame frame{};
    frame.frame_seq = 1;
    frame.track_count = 0;
    frame.tracks = nullptr;

    EXPECT_TRUE(sender.send_frame(frame));
    EXPECT_FALSE(sender.last_payload().empty());
}
