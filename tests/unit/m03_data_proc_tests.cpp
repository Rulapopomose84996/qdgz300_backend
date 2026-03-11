#include "qdgz300/m03_data_proc/fusion_engine.h"
#include "qdgz300/m03_data_proc/kalman_filter.h"
#include "qdgz300/m03_data_proc/plot_associator.h"
#include "qdgz300/m03_data_proc/track_frame_builder.h"
#include "qdgz300/m03_data_proc/track_manager.h"

#include <gtest/gtest.h>
#include <vector>

using namespace qdgz300;
using namespace qdgz300::m03;

TEST(M03KalmanFilterTest, PredictThenUpdateKeepsStateFinite)
{
    KalmanFilter filter;
    filter.init(0.0, 0.0, 0.0);
    filter.predict(0.05);
    filter.update(10.0, 0.0, 0.0);

    EXPECT_GT(filter.x(), 0.0);
    EXPECT_GE(filter.heading_deg(), -180.0);
    EXPECT_LE(filter.heading_deg(), 180.0);
}

TEST(M03PlotAssociatorTest, AssociatesNearestPlotsAndReportsUnmatched)
{
    PlotAssociator associator;
    std::vector<TrackPrediction> predictions = {
        {1, 100.0, 0.0, 0.0},
        {2, 0.0, 100.0, 0.0},
    };

    std::vector<Plot> plots(3);
    plots[0].range_m = 100.0;
    plots[0].azimuth_deg = 0.0;
    plots[1].range_m = 100.0;
    plots[1].azimuth_deg = 90.0;
    plots[2].range_m = 500.0;
    plots[2].azimuth_deg = 180.0;

    auto associations = associator.associate(predictions, plots, 20.0);
    ASSERT_EQ(associations.size(), 2u);
    ASSERT_EQ(associator.unassociated_plots().size(), 1u);
}

TEST(M03FusionEngineTest, ShouldFuseNearbyTracks)
{
    FusionEngine engine;
    TrackSnapshot a{};
    TrackSnapshot b{};
    a.pos_x = 0.0;
    a.pos_y = 0.0;
    a.vel_x = 1.0;
    a.heading_deg = 10.0;
    a.confidence = 0.5f;
    b.pos_x = 20.0;
    b.pos_y = 10.0;
    b.vel_x = 2.0;
    b.heading_deg = 12.0;
    b.confidence = 0.8f;

    EXPECT_TRUE(engine.should_fuse(a, b));
    auto fused = engine.fuse({a, b});
    ASSERT_EQ(fused.size(), 1u);
    EXPECT_TRUE(fused.front().handover_flag);
}

TEST(M03TrackManagerTest, PromotesTentativeToConfirmed)
{
    TrackManager manager;

    Plot plot{};
    plot.range_m = 100.0;
    plot.azimuth_deg = 0.0;
    plot.elevation_deg = 0.0;
    plot.array_id = 1;

    PlotBatch batch{};
    batch.data_ts = 1000;
    batch.process_ts = 1001;
    batch.array_id = 1;
    batch.cpi_seq = 1;
    batch.plot_count = 1;
    batch.flags = 0;
    batch.plots = &plot;

    std::vector<PlotBatch *> batches = {&batch};
    manager.process_plots(batches);
    manager.process_plots(batches);
    auto tracks = manager.process_plots(batches);

    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().lifecycle_state, static_cast<uint8_t>(TrackLifecycle::CONFIRMED));
}

TEST(M03TrackFrameBuilderTest, BuildsFrameWithCopiedTracks)
{
    TrackSnapshot track{};
    track.track_id = 42;
    track.update_ts = 12345;

    TrackFrameBuilder builder;
    auto frame = builder.build(7, {track}, 0x07);

    EXPECT_EQ(frame.frame_seq, 7u);
    ASSERT_NE(frame.tracks, nullptr);
    EXPECT_EQ(frame.track_count, 1u);
    EXPECT_EQ(frame.tracks[0].track_id, 42u);

    delete[] frame.tracks;
}
