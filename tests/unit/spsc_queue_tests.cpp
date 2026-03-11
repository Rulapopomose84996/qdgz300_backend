/**
 * @file spsc_queue_tests.cpp
 * @brief SPSC Lock-free Queue 单元测试
 *
 * M01 新增模块测试
 */

#include "qdgz300/m01_receiver/delivery/spsc_queue.h"
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using namespace receiver::delivery;

/**
 * @brief 简单的测试数据结构
 */
struct TestData
{
    int value{0};
    std::string str{};

    TestData() = default;
    TestData(int v, std::string s) : value(v), str(std::move(s)) {}
};

/**
 * @brief 基本功能测试
 */
class SpscQueueTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 每个测试创建一个容量为10的队列
        queue_ = std::make_unique<SpscQueue<std::shared_ptr<TestData>>>(10);
    }

    void TearDown() override
    {
        queue_.reset();
    }

    std::unique_ptr<SpscQueue<std::shared_ptr<TestData>>> queue_;
};

/**
 * @brief 测试队列构造和初始状态
 */
TEST_F(SpscQueueTest, InitialState)
{
    EXPECT_EQ(queue_->capacity(), 10);
    EXPECT_TRUE(queue_->empty());
    EXPECT_EQ(queue_->size(), 0);
    EXPECT_EQ(queue_->dropped_count(), 0);
}

/**
 * @brief 测试单次Push和Pop操作
 */
TEST_F(SpscQueueTest, SinglePushPop)
{
    auto data = std::make_shared<TestData>(42, "test");

    EXPECT_TRUE(queue_->push(std::move(data)));
    EXPECT_FALSE(queue_->empty());
    EXPECT_EQ(queue_->size(), 1);

    std::shared_ptr<TestData> popped;
    EXPECT_TRUE(queue_->pop(popped));
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->value, 42);
    EXPECT_EQ(popped->str, "test");
    EXPECT_TRUE(queue_->empty());
}

/**
 * @brief 测试空队列Pop操作
 */
TEST_F(SpscQueueTest, PopFromEmptyQueue)
{
    std::shared_ptr<TestData> popped;
    EXPECT_FALSE(queue_->pop(popped));
    EXPECT_EQ(popped, nullptr);
}

/**
 * @brief 测试多次Push和Pop
 */
TEST_F(SpscQueueTest, MultiplePushPop)
{
    constexpr int count = 5;

    // Push 5个元素
    for (int i = 0; i < count; ++i)
    {
        auto data = std::make_shared<TestData>(i, "item" + std::to_string(i));
        EXPECT_TRUE(queue_->push(std::move(data)));
    }

    EXPECT_EQ(queue_->size(), count);

    // Pop 5个元素并验证
    for (int i = 0; i < count; ++i)
    {
        std::shared_ptr<TestData> popped;
        EXPECT_TRUE(queue_->pop(popped));
        ASSERT_NE(popped, nullptr);
        EXPECT_EQ(popped->value, i);
        EXPECT_EQ(popped->str, "item" + std::to_string(i));
    }

    EXPECT_TRUE(queue_->empty());
}

/**
 * @brief 测试队列满时的drop-oldest策略
 */
TEST_F(SpscQueueTest, DropOldestWhenFull)
{
    const size_t capacity = queue_->capacity();

    // 填满队列
    for (size_t i = 0; i < capacity; ++i)
    {
        auto data = std::make_shared<TestData>(static_cast<int>(i), "item" + std::to_string(i));
        EXPECT_TRUE(queue_->push(std::move(data)));
    }

    EXPECT_EQ(queue_->size(), capacity);
    EXPECT_EQ(queue_->dropped_count(), 0);

    // 再Push一个元素，应该触发drop-oldest
    auto new_data = std::make_shared<TestData>(999, "overflow");
    EXPECT_TRUE(queue_->push(std::move(new_data)));

    // 丢弃计数应该增加
    EXPECT_EQ(queue_->dropped_count(), 1);

    // Pop第一个元素，应该是第二个Push的元素（第一个被丢弃）
    std::shared_ptr<TestData> popped;
    EXPECT_TRUE(queue_->pop(popped));
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->value, 1); // 不是0，因为0被丢弃了
}

/**
 * @brief 测试丢弃计数器
 */
TEST_F(SpscQueueTest, DroppedCountTracking)
{
    const size_t capacity = queue_->capacity();

    // 填满队列
    for (size_t i = 0; i < capacity; ++i)
    {
        auto data = std::make_shared<TestData>(static_cast<int>(i), "item");
        queue_->push(std::move(data));
    }

    // 继续Push 5个元素，每个都会触发drop-oldest
    for (int i = 0; i < 5; ++i)
    {
        auto data = std::make_shared<TestData>(100 + i, "overflow");
        queue_->push(std::move(data));
    }

    EXPECT_EQ(queue_->dropped_count(), 5);

    // 重置计数器
    queue_->reset_dropped_count();
    EXPECT_EQ(queue_->dropped_count(), 0);
}

/**
 * @brief 测试循环使用（环形缓冲区）
 */
TEST_F(SpscQueueTest, CircularBehavior)
{
    const size_t capacity = queue_->capacity();

    // 循环Push和Pop多次
    for (int round = 0; round < 3; ++round)
    {
        // 填满队列
        for (size_t i = 0; i < capacity; ++i)
        {
            auto data = std::make_shared<TestData>(static_cast<int>(i), "round" + std::to_string(round));
            queue_->push(std::move(data));
        }

        EXPECT_EQ(queue_->size(), capacity);

        // 清空队列
        for (size_t i = 0; i < capacity; ++i)
        {
            std::shared_ptr<TestData> popped;
            EXPECT_TRUE(queue_->pop(popped));
            ASSERT_NE(popped, nullptr);
            EXPECT_EQ(popped->value, static_cast<int>(i));
        }

        EXPECT_TRUE(queue_->empty());
    }
}

/**
 * @brief 测试单生产者单消费者并发场景
 */
TEST_F(SpscQueueTest, ConcurrentProducerConsumer)
{
    constexpr int item_count = 1000;
    queue_ = std::make_unique<SpscQueue<std::shared_ptr<TestData>>>(2048);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};

    // 生产者线程
    std::thread producer([this, &produced, &producer_done]()
                         {
        for (int i = 0; i < item_count; ++i)
        {
            auto data = std::make_shared<TestData>(i, "item" + std::to_string(i));
            queue_->push(std::move(data));
            produced.fetch_add(1, std::memory_order_relaxed);

            // 模拟生产延迟
            if (i % 100 == 0)
            {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release); });

    // 消费者线程
    std::thread consumer([this, &consumed, &producer_done]()
                         {
        int last_value = -1;
        while (!producer_done.load(std::memory_order_acquire) || !queue_->empty())
        {
            std::shared_ptr<TestData> data;
            if (queue_->pop(data))
            {
                EXPECT_NE(data, nullptr);
                // 验证值的单调性（考虑到可能有丢弃）
                EXPECT_GT(data->value, last_value);
                last_value = data->value;
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                // 队列为空，短暂休眠
                std::this_thread::yield();
            }
        } });

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), item_count);
    EXPECT_EQ(queue_->dropped_count(), 0);
    EXPECT_EQ(consumed.load(), item_count);
}

/**
 * @brief 测试容量为1的极端情况
 */
TEST_F(SpscQueueTest, MinimalCapacity)
{
    auto tiny_queue = std::make_unique<SpscQueue<std::shared_ptr<TestData>>>(1);

    auto data1 = std::make_shared<TestData>(1, "first");
    EXPECT_TRUE(tiny_queue->push(std::move(data1)));
    EXPECT_EQ(tiny_queue->size(), 1);

    // 再Push应该触发drop-oldest
    auto data2 = std::make_shared<TestData>(2, "second");
    EXPECT_TRUE(tiny_queue->push(std::move(data2)));
    EXPECT_EQ(tiny_queue->dropped_count(), 1);

    std::shared_ptr<TestData> popped;
    EXPECT_TRUE(tiny_queue->pop(popped));
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->value, 2); // 得到的是第二个元素
}

/**
 * @brief 测试智能指针的正确释放
 */
TEST_F(SpscQueueTest, SmartPointerLifetime)
{
    std::weak_ptr<TestData> weak_ref;

    {
        auto data = std::make_shared<TestData>(42, "test");
        weak_ref = data;
        EXPECT_FALSE(weak_ref.expired());

        queue_->push(std::move(data));
        EXPECT_FALSE(weak_ref.expired()); // 队列持有引用
    }

    EXPECT_FALSE(weak_ref.expired()); // 队列仍持有

    std::shared_ptr<TestData> popped;
    queue_->pop(popped);

    EXPECT_FALSE(weak_ref.expired()); // popped持有

    popped.reset();
    EXPECT_TRUE(weak_ref.expired()); // 所有引用释放，对象销毁
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
