/**
 * @file stub_consumer_tests.cpp
 * @brief StubConsumer 单元测试
 *
 * M01 新增模块测试
 */

#include "qdgz300/m01_receiver/delivery/stub_consumer.h"
#include "qdgz300/m01_receiver/delivery/raw_block.h"
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>

using namespace receiver::delivery;

/**
 * @brief StubConsumer 基本功能测试
 */
class StubConsumerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        queue_ = std::make_shared<StubConsumer::RawBlockQueue>();
    }

    void TearDown() override
    {
        if (consumer_)
        {
            consumer_->stop();
            consumer_.reset();
        }
        queue_.reset();
    }

    std::shared_ptr<StubConsumer::RawBlockQueue> queue_;
    std::unique_ptr<StubConsumer> consumer_;

    /**
     * @brief 创建测试用的RawBlock
     */
    std::shared_ptr<RawBlock> create_test_block(uint8_t array_id, uint32_t cpi_seq,
                                                bool incomplete = false)
    {
        auto block = std::make_shared<RawBlock>();
        block->array_id = array_id;
        block->cpi_seq = cpi_seq;
        block->fragment_count = 10;
        block->data_size = 1024;
        block->flags = 0;
        block->ingest_ts = 1000000000ULL;
        block->data_ts = 999999000ULL;

        if (incomplete)
        {
            block->set_flag(RawBlockFlags::INCOMPLETE_FRAME);
        }

        // 填充一些测试数据
        for (size_t i = 0; i < 1024 && i < sizeof(block->payload); ++i)
        {
            block->payload[i] = static_cast<uint8_t>(i % 256);
        }

        return block;
    }
};

/**
 * @brief 测试StubConsumer构造和基本配置
 */
TEST_F(StubConsumerTest, Construction)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = true;
    config.write_to_file = false;
    config.stats_interval_ms = 1000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    ASSERT_NE(consumer_, nullptr);

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 0);
    EXPECT_EQ(stats.complete_frames.load(), 0);
    EXPECT_EQ(stats.incomplete_frames.load(), 0);
}

/**
 * @brief 测试启动和停止
 */
TEST_F(StubConsumerTest, StartStop)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false; // 关闭打印避免测试输出混乱

    consumer_ = std::make_unique<StubConsumer>(config, queue_);

    EXPECT_TRUE(consumer_->start());

    // 短暂等待确保线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    consumer_->stop();

    // 再次停止应该安全
    consumer_->stop();
}

/**
 * @brief 测试处理单个RawBlock
 */
TEST_F(StubConsumerTest, ProcessSingleBlock)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.stats_interval_ms = 10000; // 避免统计输出干扰

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    // 推入一个完整帧
    auto block = create_test_block(1, 100, false);
    queue_->drop_oldest_push(std::move(block));

    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 1);
    EXPECT_EQ(stats.complete_frames.load(), 1);
    EXPECT_EQ(stats.incomplete_frames.load(), 0);
    EXPECT_EQ(stats.last_cpi_seq.load(), 100);
    EXPECT_EQ(stats.total_bytes.load(), 1024);
}

/**
 * @brief 测试处理不完整帧
 */
TEST_F(StubConsumerTest, ProcessIncompleteFrame)
{
    StubConsumerConfig config;
    config.array_id = 2;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    // 推入一个不完整帧
    auto block = create_test_block(2, 200, true);
    queue_->drop_oldest_push(std::move(block));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 1);
    EXPECT_EQ(stats.complete_frames.load(), 0);
    EXPECT_EQ(stats.incomplete_frames.load(), 1);
}

/**
 * @brief 测试处理多个RawBlock
 */
TEST_F(StubConsumerTest, ProcessMultipleBlocks)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    constexpr int block_count = 50;
    constexpr int incomplete_count = 10;

    // 推入多个块
    for (int i = 0; i < block_count; ++i)
    {
        bool incomplete = (i % (block_count / incomplete_count)) == 0;
        auto block = create_test_block(1, 1000 + i, incomplete);
        queue_->drop_oldest_push(std::move(block));
    }

    // 等待处理完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), block_count);
    EXPECT_EQ(stats.complete_frames.load(), block_count - incomplete_count);
    EXPECT_EQ(stats.incomplete_frames.load(), incomplete_count);
    EXPECT_EQ(stats.last_cpi_seq.load(), 1000 + block_count - 1);
}

/**
 * @brief 测试高负载场景
 */
TEST_F(StubConsumerTest, HighLoadProcessing)
{
    StubConsumerConfig config;
    config.array_id = 3;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    constexpr int block_count = 500;

    // 快速推入大量块
    for (int i = 0; i < block_count; ++i)
    {
        auto block = create_test_block(3, 10000 + i, i % 10 == 0);
        queue_->drop_oldest_push(std::move(block));
    }

    // 等待处理
    std::this_thread::sleep_for(std::chrono::seconds(1));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();

    // 由于队列容量为64，可能会有丢弃
    const uint64_t dropped = queue_->drop_count();
    const uint64_t processed = stats.total_blocks.load();

    // 处理数量 + 丢弃数量 应该等于生产数量
    EXPECT_EQ(processed + dropped, block_count);
}

/**
 * @brief 测试RawBlock标志位识别
 */
TEST_F(StubConsumerTest, FlagRecognition)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    // 创建带不同标志的块
    auto block1 = create_test_block(1, 1, false);
    auto block2 = create_test_block(1, 2, true);

    auto block3 = create_test_block(1, 3, false);
    block3->set_flag(RawBlockFlags::SNAPSHOT_PRESENT);

    auto block4 = create_test_block(1, 4, false);
    block4->set_flag(RawBlockFlags::HEARTBEAT_RELATED);

    queue_->drop_oldest_push(std::move(block1));
    queue_->drop_oldest_push(std::move(block2));
    queue_->drop_oldest_push(std::move(block3));
    queue_->drop_oldest_push(std::move(block4));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 4);
    EXPECT_EQ(stats.complete_frames.load(), 3);
    EXPECT_EQ(stats.incomplete_frames.load(), 1);
}

/**
 * @brief 测试文件写入功能（可选）
 *
 * 注意：此测试会实际创建文件，需要清理
 */
TEST_F(StubConsumerTest, DISABLED_FileWriting)
{
    const std::string output_file = "/tmp/test_rawblocks_" +
                                    std::to_string(std::time(nullptr)) + ".bin";

    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.write_to_file = true;
    config.output_file = output_file;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);

    // 如果文件打开失败，start会返回false
    bool started = consumer_->start();
    if (!started)
    {
        GTEST_SKIP() << "Cannot create output file: " << output_file;
    }

    // 推入几个块
    for (int i = 0; i < 5; ++i)
    {
        auto block = create_test_block(1, 100 + i, false);
        queue_->drop_oldest_push(std::move(block));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    consumer_->stop();

    // 验证文件存在且非空
    std::ifstream file(output_file, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file.is_open());
    const auto file_size = file.tellg();
    EXPECT_GT(file_size, 0);
    file.close();

    // 清理文件
    std::remove(output_file.c_str());
}

/**
 * @brief 测试空队列处理
 */
TEST_F(StubConsumerTest, EmptyQueueHandling)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    // 让消费者运行一段时间但不推入任何数据
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 0);
    EXPECT_EQ(stats.complete_frames.load(), 0);
    EXPECT_EQ(stats.incomplete_frames.load(), 0);
}

/**
 * @brief 测试统计信息累积
 */
TEST_F(StubConsumerTest, StatisticsAccumulation)
{
    StubConsumerConfig config;
    config.array_id = 1;
    config.print_summary = false;
    config.stats_interval_ms = 10000;

    consumer_ = std::make_unique<StubConsumer>(config, queue_);
    EXPECT_TRUE(consumer_->start());

    uint64_t expected_bytes = 0;

    // 分批推入不同大小的块
    for (int batch = 0; batch < 3; ++batch)
    {
        for (int i = 0; i < 10; ++i)
        {
            auto block = create_test_block(1, batch * 10 + i, false);
            block->data_size = 512 + i * 100; // 变化的数据大小
            expected_bytes += block->data_size;
            queue_->drop_oldest_push(std::move(block));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    consumer_->stop();

    const auto &stats = consumer_->get_statistics();
    EXPECT_EQ(stats.total_blocks.load(), 30);
    EXPECT_EQ(stats.total_bytes.load(), expected_bytes);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
