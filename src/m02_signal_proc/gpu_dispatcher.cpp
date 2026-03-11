#include "qdgz300/m02_signal_proc/gpu_dispatcher.h"
#include "qdgz300/common/error_codes.h"
#include "qdgz300/common/numa_utils.h"

#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>

namespace qdgz300::m02
{

    GpuDispatcher::GpuDispatcher(
        SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY> *rawcpi_qs,
        SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY> *plots_qs,
        GpuPipeline &pipeline,
        SignalProcMetrics &metrics)
        : rawcpi_qs_(rawcpi_qs), plots_qs_(plots_qs), pipeline_(pipeline), metrics_(metrics) {}

    void GpuDispatcher::start()
    {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this]()
                              { run(); });
        spdlog::info("[M02] GpuDispatcher started");
    }

    void GpuDispatcher::stop()
    {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
        spdlog::info("[M02] GpuDispatcher stopped");
    }

    void GpuDispatcher::run() noexcept
    {
        // TODO: bind_thread_to_cpu(19); set_realtime_priority(70);
        while (running_.load(std::memory_order_relaxed))
        {
            bool did_work = false;

            for (uint8_t i = 0; i < GPU_STREAM_COUNT; ++i)
            {
                const uint8_t q_idx = static_cast<uint8_t>((rr_index_ + i) % GPU_STREAM_COUNT);

                PlotBatch *result = nullptr;
                if (pipeline_.poll_completion(q_idx, &result))
                {
                    if (result)
                    {
                        plots_qs_[q_idx].drop_oldest_push(result);
                        metrics_.gpu_frames_completed.increment();
                        did_work = true;
                    }
                }

                const uint32_t inflight_before_timeout = pipeline_.inflight_count(q_idx);
                pipeline_.handle_timeout(q_idx);
                const uint32_t inflight_after_timeout = pipeline_.inflight_count(q_idx);
                if (inflight_after_timeout < inflight_before_timeout)
                {
                    metrics_.gpu_timeout_total.increment(inflight_before_timeout - inflight_after_timeout);
                    did_work = true;
                }

                metrics_.gpu_inflight_count.set(inflight_after_timeout);
                metrics_.gpu_queue_depth.set(static_cast<int64_t>(rawcpi_qs_[q_idx].size()));

                if (inflight_after_timeout >= GPU_INFLIGHT_MAX)
                    continue;

                auto block_opt = rawcpi_qs_[q_idx].try_pop();
                if (!block_opt.has_value())
                    continue;

                RawBlock *block = block_opt.value();
                const auto ec = pipeline_.submit_cpi(block, q_idx);
                if (ec == ErrorCode::OK)
                {
                    metrics_.gpu_frames_submitted.increment();
                    metrics_.gpu_inflight_count.set(pipeline_.inflight_count(q_idx));
                    metrics_.gpu_queue_depth.set(static_cast<int64_t>(rawcpi_qs_[q_idx].size()));
                    did_work = true;
                    continue;
                }

                spdlog::warn("[M02] submit_cpi failed on stream {}: {}",
                             q_idx, error_name(ec));
            }

            rr_index_ = static_cast<uint8_t>((rr_index_ + 1) % GPU_STREAM_COUNT);

            if (!did_work)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

} // namespace qdgz300::m02
