#pragma once
#include <cstdint>
#include <cstddef>

namespace qdgz300::m02
{

    /// FFT/CFAR/MTD 内核声明（stub）
    /// 实际 CUDA 实现在 signal_kernels.cu 中
    namespace kernels
    {

        /// FFT 处理内核（stub 声明）
        void launch_fft(const void *input, void *output,
                        size_t sample_count, void *stream);

        /// CFAR 检测内核（stub 声明）
        void launch_cfar(const void *fft_output, void *detections,
                         size_t range_bins, size_t doppler_bins, void *stream);

        /// MTD 处理内核（stub 声明）
        void launch_mtd(const void *input, void *output,
                        size_t pulse_count, size_t sample_count, void *stream);

    } // namespace kernels
} // namespace qdgz300::m02
