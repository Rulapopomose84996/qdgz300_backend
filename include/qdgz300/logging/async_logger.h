// include/qdgz300/logging/async_logger.h
// 异步日志封装 — spdlog JSON 格式，per-thread buffer
// 数据面日志异步写入，控制面可同步 flush
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <atomic>

// Forward-declare spdlog types to avoid leaking spdlog headers to consumers
namespace spdlog
{
    class logger;
} // namespace spdlog

namespace qdgz300
{

    /// 日志级别（映射 spdlog::level）
    enum class LogLevel : uint8_t
    {
        TRACE = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        CRITICAL = 5,
        OFF = 6,
    };

    /// 异步日志器配置
    struct AsyncLoggerConfig
    {
        std::string log_dir = "/var/log/qdgz300";
        std::string log_name = "qdgz300";
        LogLevel level = LogLevel::INFO;
        size_t async_queue_size = 8192; // spdlog 异步队列大小
        size_t max_file_size_mb = 100;  // 单文件最大 MB
        size_t max_files = 10;          // 滚动日志文件数
        bool console_output = false;    // 是否同时输出到控制台
        uint64_t run_id = 0;            // 当前运行 ID
    };

    /// 异步日志器 — spdlog 封装
    /// - JSON 格式输出，包含 run_id 和 trace_id 字段
    /// - per-thread buffer 减少竞争
    /// - 异步后端写入，不阻塞数据面
    class AsyncLogger
    {
    public:
        /// 初始化全局日志器（应在 main() 启动时调用一次）
        /// @return true=成功, false=失败
        static bool initialize(const AsyncLoggerConfig &config) noexcept;

        /// 关闭日志器，刷写所有缓冲
        static void shutdown() noexcept;

        /// 获取底层 spdlog logger（供模块直接使用 spdlog 宏）
        static std::shared_ptr<spdlog::logger> get_logger() noexcept;

        /// 设置当前线程的 trace_id（在线程入口调用）
        static void set_thread_trace_id(uint64_t trace_id) noexcept;

        /// 获取当前线程的 trace_id
        static uint64_t get_thread_trace_id() noexcept;

        /// 动态调整日志级别（DYNAMIC_IMMEDIATE 参数）
        static void set_level(LogLevel level) noexcept;

        /// 获取当前日志级别
        static LogLevel get_level() noexcept;

        /// 获取 run_id
        static uint64_t get_run_id() noexcept;

        /// 同步 flush（控制面调用）
        static void flush() noexcept;

        // ── 便捷日志方法（JSON 格式，自动注入 run_id + trace_id）──

        static void log_trace(std::string_view module, std::string_view msg) noexcept;
        static void log_debug(std::string_view module, std::string_view msg) noexcept;
        static void log_info(std::string_view module, std::string_view msg) noexcept;
        static void log_warn(std::string_view module, std::string_view msg) noexcept;
        static void log_error(std::string_view module, std::string_view msg) noexcept;
        static void log_critical(std::string_view module, std::string_view msg) noexcept;

        /// 结构化日志（附加 JSON detail）
        static void log_structured(LogLevel level,
                                   std::string_view module,
                                   std::string_view msg,
                                   std::string_view detail_json) noexcept;

    private:
        AsyncLogger() = delete;

        static std::shared_ptr<spdlog::logger> logger_;
        static std::atomic<uint64_t> run_id_;
        static std::atomic<LogLevel> current_level_;
        static thread_local uint64_t thread_trace_id_;
    };

} // namespace qdgz300
