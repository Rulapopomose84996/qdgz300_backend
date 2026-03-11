// include/qdgz300/m02_signal_proc/gpu_dispatcher.h
#pragma once
#include <atomic>
#include <thread>
#include "qdgz300/common/spsc_queue.h"
#include "qdgz300/common/types.h"
#include "qdgz300/common/constants.h"
#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include "qdgz300/m02_signal_proc/signal_proc_metrics.h"

namespace qdgz300::m02
{

    /// GPU Dispatcher — 单线程统一调度 3 CUDA Streams（Round-Robin）
    class GpuDispatcher
    {
    public:
        GpuDispatcher(
            SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY> *rawcpi_qs,
            SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY> *plots_qs,
            GpuPipeline &pipeline,
            SignalProcMetrics &metrics);

        void start();
        void stop();
        void run() noexcept;

    private:
        SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY> *rawcpi_qs_;
        SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY> *plots_qs_;
        GpuPipeline &pipeline_;
        SignalProcMetrics &metrics_;
        std::atomic<bool> running_{false};
        std::thread thread_;
        uint8_t rr_index_{0};
    };

} // namespace qdgz300::m02
