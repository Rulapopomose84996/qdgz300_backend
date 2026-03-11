// tests/common_metrics_tests.cpp
// AtomicCounter / AtomicGauge + MetricsExporter 单元测试
#include <gtest/gtest.h>
#include "qdgz300/common/metrics.h"
#include "qdgz300/logging/metrics_exporter.h"

#include <thread>
#include <vector>

using namespace qdgz300;

// ═══ AtomicCounter 测试 ═══

TEST(AtomicCounterTest, DefaultZero)
{
    AtomicCounter c("test_counter");
    EXPECT_EQ(c.get(), 0u);
    EXPECT_EQ(c.name(), "test_counter");
}

TEST(AtomicCounterTest, Increment)
{
    AtomicCounter c;
    c.increment();
    EXPECT_EQ(c.get(), 1u);
    c.increment(10);
    EXPECT_EQ(c.get(), 11u);
}

TEST(AtomicCounterTest, Reset)
{
    AtomicCounter c;
    c.increment(100);
    c.reset();
    EXPECT_EQ(c.get(), 0u);
}

TEST(AtomicCounterTest, ConcurrentIncrement)
{
    AtomicCounter c;
    constexpr uint64_t PER_THREAD = 100000;
    constexpr size_t THREADS = 4;

    std::vector<std::thread> threads;
    for (size_t i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (uint64_t j = 0; j < PER_THREAD; ++j)
            {
                c.increment();
            } });
    }
    for (auto &t : threads)
    {
        t.join();
    }

    EXPECT_EQ(c.get(), PER_THREAD * THREADS);
}

// ═══ AtomicGauge 测试 ═══

TEST(AtomicGaugeTest, DefaultZero)
{
    AtomicGauge g("test_gauge");
    EXPECT_EQ(g.get(), 0);
    EXPECT_EQ(g.name(), "test_gauge");
}

TEST(AtomicGaugeTest, SetAndGet)
{
    AtomicGauge g;
    g.set(42);
    EXPECT_EQ(g.get(), 42);
    g.set(-10);
    EXPECT_EQ(g.get(), -10);
}

TEST(AtomicGaugeTest, IncrementDecrement)
{
    AtomicGauge g;
    g.increment(5);
    EXPECT_EQ(g.get(), 5);
    g.decrement(2);
    EXPECT_EQ(g.get(), 3);
    g.decrement(10);
    EXPECT_EQ(g.get(), -7);
}

TEST(AtomicGaugeTest, ConcurrentIncrementDecrement)
{
    AtomicGauge g;
    constexpr int64_t PER_THREAD = 50000;
    constexpr size_t THREADS = 4;

    std::vector<std::thread> threads;
    // 一半线程增，一半线程减
    for (size_t i = 0; i < THREADS / 2; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (int64_t j = 0; j < PER_THREAD; ++j)
            {
                g.increment();
            } });
    }
    for (size_t i = 0; i < THREADS / 2; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (int64_t j = 0; j < PER_THREAD; ++j)
            {
                g.decrement();
            } });
    }
    for (auto &t : threads)
    {
        t.join();
    }

    // 增减数量相同，最终应为 0
    EXPECT_EQ(g.get(), 0);
}

// ═══ MetricsExporter 测试 ═══

TEST(MetricsExporterTest, Singleton)
{
    auto &e1 = MetricsExporter::instance();
    auto &e2 = MetricsExporter::instance();
    EXPECT_EQ(&e1, &e2);
}

TEST(MetricsExporterTest, RegisterAndExport)
{
    auto &exp = MetricsExporter::instance();
    exp.clear();

    AtomicCounter counter("test_counter");
    counter.increment(42);

    exp.register_metric({"qdgz300_test_counter_total",
                         "Test counter metric",
                         MetricType::COUNTER,
                         [&counter]() -> double
                         { return static_cast<double>(counter.get()); }});

    EXPECT_EQ(exp.metric_count(), 1u);

    auto text = exp.export_text();
    EXPECT_NE(text.find("# HELP qdgz300_test_counter_total Test counter metric"), std::string::npos);
    EXPECT_NE(text.find("# TYPE qdgz300_test_counter_total counter"), std::string::npos);
    EXPECT_NE(text.find("qdgz300_test_counter_total 42"), std::string::npos);

    exp.clear();
}

TEST(MetricsExporterTest, MultipleMetrics)
{
    auto &exp = MetricsExporter::instance();
    exp.clear();

    AtomicCounter c1("c1");
    AtomicGauge g1("g1");
    c1.increment(100);
    g1.set(50);

    exp.register_metrics({{"qdgz300_c1_total",
                           "Counter 1",
                           MetricType::COUNTER,
                           [&c1]() -> double
                           { return static_cast<double>(c1.get()); }},
                          {"qdgz300_g1_value",
                           "Gauge 1",
                           MetricType::GAUGE,
                           [&g1]() -> double
                           { return static_cast<double>(g1.get()); }}});

    EXPECT_EQ(exp.metric_count(), 2u);

    auto text = exp.export_text();
    EXPECT_NE(text.find("counter"), std::string::npos);
    EXPECT_NE(text.find("gauge"), std::string::npos);
    EXPECT_NE(text.find("100"), std::string::npos);
    EXPECT_NE(text.find("50"), std::string::npos);

    exp.clear();
}

TEST(MetricsExporterTest, Clear)
{
    auto &exp = MetricsExporter::instance();
    exp.clear();

    exp.register_metric({"test_m", "help", MetricType::COUNTER, []()
                         { return 1.0; }});
    EXPECT_EQ(exp.metric_count(), 1u);

    exp.clear();
    EXPECT_EQ(exp.metric_count(), 0u);
    EXPECT_EQ(exp.export_text(), "");
}
