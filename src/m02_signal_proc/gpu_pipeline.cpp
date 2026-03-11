#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <spdlog/spdlog.h>

namespace qdgz300::m02
{

    namespace
    {
        constexpr float kAmplitudeThreshold = 100.0f;
        constexpr size_t kMaxPlotsPerCpi = 512;
        constexpr size_t kPlotAlignment = 64;

        size_t align_up(size_t value, size_t alignment) noexcept
        {
            return ((value + alignment - 1) / alignment) * alignment;
        }
    } // namespace

    GpuPipeline::GpuPipeline(CudaStreamPool &streams, PinnedBufferPool &bufs)
        : streams_(streams), bufs_(bufs) {}

    ErrorCode GpuPipeline::submit_cpi(RawBlock *block, uint8_t stream_id) noexcept
    {
        if (!block)
            return ErrorCode::GPU_H2D_FAILED;
        if (stream_id >= GPU_STREAM_COUNT)
            return ErrorCode::GPU_H2D_FAILED;

        const int8_t slot_idx = find_free_slot(stream_id);
        if (slot_idx < 0)
            return ErrorCode::GPU_H2D_FAILED;

        auto &slot = inflight_[stream_id][slot_idx];

#ifndef QDGZ300_HAS_GPU
        const size_t iq_pair_bytes = 2 * sizeof(float);
        const size_t usable_bytes = (block->payload && block->data_size >= iq_pair_bytes)
                                        ? (block->data_size / iq_pair_bytes) * iq_pair_bytes
                                        : 0;
        const size_t num_samples = usable_bytes / iq_pair_bytes;
        const auto *iq = reinterpret_cast<const float *>(block->payload);

        std::vector<Plot> detected;
        detected.reserve(std::min(num_samples, kMaxPlotsPerCpi));

        for (size_t i = 0; i < num_samples && detected.size() < kMaxPlotsPerCpi; ++i)
        {
            const float i_sample = iq[2 * i];
            const float q_sample = iq[2 * i + 1];
            const float amplitude = std::sqrt(i_sample * i_sample + q_sample * q_sample);

            if (amplitude <= kAmplitudeThreshold)
                continue;

            Plot plot{};
            plot.range_m = static_cast<double>(i);
            plot.azimuth_deg = 0.0;
            plot.elevation_deg = 0.0;
            plot.doppler_mps = 0.0;
            plot.snr_db = 20.0f * std::log10(amplitude / kAmplitudeThreshold);
            plot.amplitude = amplitude;
            plot.beam_id = 0;
            plot.array_id = block->array_id;
            detected.push_back(plot);
        }

        auto *batch = new (std::nothrow) PlotBatch{};
        if (!batch)
            return ErrorCode::GPU_H2D_FAILED;

        batch->data_ts = block->data_ts;
        batch->process_ts = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        batch->array_id = block->array_id;
        batch->cpi_seq = block->cpi_seq;
        batch->plot_count = static_cast<uint16_t>(detected.size());
        batch->flags = (block->flags & RawBlock::FLAG_INCOMPLETE_FRAME) ? RawBlock::FLAG_INCOMPLETE_FRAME : 0u;
        batch->plots = nullptr;

        if (!detected.empty())
        {
            const size_t plots_bytes = detected.size() * sizeof(Plot);
            const size_t alloc_bytes = align_up(plots_bytes, kPlotAlignment);
            batch->plots = static_cast<Plot *>(aligned_alloc(kPlotAlignment, alloc_bytes));
            if (!batch->plots)
            {
                delete batch;
                return ErrorCode::GPU_H2D_FAILED;
            }
            std::memcpy(batch->plots, detected.data(), plots_bytes);
        }

        slot.input = block;
        slot.output = batch;
        slot.occupied = true;
        slot.completed = true;
        slot.submit_time = std::chrono::steady_clock::now();
        return ErrorCode::OK;
#else
        // TODO: H2D memcpy async → launch kernels → D2H memcpy async
        return ErrorCode::OK;
#endif
    }

    bool GpuPipeline::poll_completion(uint8_t stream_id, PlotBatch **result) noexcept
    {
        if (!result)
            return false;

        *result = nullptr;
        if (stream_id >= GPU_STREAM_COUNT)
            return false;

        for (uint8_t s = 0; s < GPU_INFLIGHT_MAX; ++s)
        {
            auto &slot = inflight_[stream_id][s];
            if (!slot.occupied || !slot.completed)
                continue;

            *result = slot.output;
            slot.output = nullptr;
            slot.input = nullptr;
            slot.occupied = false;
            slot.completed = false;
            slot.submit_time = std::chrono::steady_clock::time_point{};
            return true;
        }

        return false;
    }

    void GpuPipeline::handle_timeout(uint8_t stream_id) noexcept
    {
        if (stream_id >= GPU_STREAM_COUNT)
            return;

        const auto now = std::chrono::steady_clock::now();

        for (uint8_t s = 0; s < GPU_INFLIGHT_MAX; ++s)
        {
            auto &slot = inflight_[stream_id][s];
            if (!slot.occupied)
                continue;

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - slot.submit_time);
            if (elapsed.count() <= static_cast<int64_t>(T_GPU_MAX_MS))
                continue;

            spdlog::warn("[M02] GPU timeout on stream {} slot {} ({}ms > {}ms)",
                         stream_id, s, elapsed.count(), T_GPU_MAX_MS);
            if (slot.output)
                slot.output->flags |= RawBlock::FLAG_GPU_TIMEOUT;

            slot.output = nullptr;
            slot.occupied = false;
            slot.completed = false;
            slot.input = nullptr;
            slot.submit_time = std::chrono::steady_clock::time_point{};
        }
    }

    uint32_t GpuPipeline::inflight_count(uint8_t stream_id) const noexcept
    {
        if (stream_id >= GPU_STREAM_COUNT)
            return 0;

        uint32_t count = 0;
        for (uint8_t s = 0; s < GPU_INFLIGHT_MAX; ++s)
        {
            if (inflight_[stream_id][s].occupied)
                ++count;
        }
        return count;
    }

    int8_t GpuPipeline::find_free_slot(uint8_t stream_id) const noexcept
    {
        if (stream_id >= GPU_STREAM_COUNT)
            return -1;

        for (uint8_t s = 0; s < GPU_INFLIGHT_MAX; ++s)
        {
            if (!inflight_[stream_id][s].occupied)
                return static_cast<int8_t>(s);
        }
        return -1;
    }

    bool GpuPipeline::has_timeout(uint8_t stream_id) const noexcept
    {
        if (stream_id >= GPU_STREAM_COUNT)
            return false;

        const auto now = std::chrono::steady_clock::now();
        for (uint8_t s = 0; s < GPU_INFLIGHT_MAX; ++s)
        {
            const auto &slot = inflight_[stream_id][s];
            if (!slot.occupied || slot.completed)
                continue;

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - slot.submit_time);
            if (elapsed.count() > static_cast<int64_t>(T_GPU_MAX_MS))
                return true;
        }
        return false;
    }

} // namespace qdgz300::m02
