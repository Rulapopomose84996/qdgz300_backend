// src/logging/async_logger.cpp
// 异步日志器实现 — spdlog JSON 格式 + per-thread trace_id
#include "qdgz300/logging/async_logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

#include <chrono>
#include <ctime>
#include <mutex>

namespace qdgz300
{

    // ── 静态成员定义 ──
    std::shared_ptr<spdlog::logger> AsyncLogger::logger_;
    std::atomic<uint64_t> AsyncLogger::run_id_{0};
    std::atomic<LogLevel> AsyncLogger::current_level_{LogLevel::INFO};
    thread_local uint64_t AsyncLogger::thread_trace_id_ = 0;

    namespace
    {
        /// 将 qdgz300::LogLevel 映射到 spdlog::level
        spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::TRACE:
                return spdlog::level::trace;
            case LogLevel::DEBUG:
                return spdlog::level::debug;
            case LogLevel::INFO:
                return spdlog::level::info;
            case LogLevel::WARN:
                return spdlog::level::warn;
            case LogLevel::ERROR:
                return spdlog::level::err;
            case LogLevel::CRITICAL:
                return spdlog::level::critical;
            case LogLevel::OFF:
                return spdlog::level::off;
            }
            return spdlog::level::info;
        }

        /// spdlog 日志级别名称
        const char *level_string(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::TRACE:
                return "TRACE";
            case LogLevel::DEBUG:
                return "DEBUG";
            case LogLevel::INFO:
                return "INFO";
            case LogLevel::WARN:
                return "WARN";
            case LogLevel::ERROR:
                return "ERROR";
            case LogLevel::CRITICAL:
                return "CRITICAL";
            case LogLevel::OFF:
                return "OFF";
            }
            return "UNKNOWN";
        }

        /// 获取日志级别字符串用于 JSON
        const char *level_to_json_string(spdlog::level::level_enum lvl) noexcept
        {
            switch (lvl)
            {
            case spdlog::level::trace:
                return "TRACE";
            case spdlog::level::debug:
                return "DEBUG";
            case spdlog::level::info:
                return "INFO";
            case spdlog::level::warn:
                return "WARN";
            case spdlog::level::err:
                return "ERROR";
            case spdlog::level::critical:
                return "CRITICAL";
            default:
                return "OFF";
            }
        }

    } // anonymous namespace

    bool AsyncLogger::initialize(const AsyncLoggerConfig &config) noexcept
    {
        try
        {
            // 初始化 spdlog 异步线程池
            spdlog::init_thread_pool(config.async_queue_size, 1);

            // 创建 sinks
            std::vector<spdlog::sink_ptr> sinks;

            // 滚动文件 sink
            auto file_path = config.log_dir + "/" + config.log_name + ".log";
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path,
                config.max_file_size_mb * 1024 * 1024,
                config.max_files);
            sinks.push_back(file_sink);

            // 可选控制台 sink
            if (config.console_output)
            {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                sinks.push_back(console_sink);
            }

            // 创建异步 logger
            logger_ = std::make_shared<spdlog::async_logger>(
                config.log_name,
                sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);

            // JSON 格式模式：{"ts":"%Y-%m-%dT%H:%M:%S.%f","level":"%l","msg":"%v"}
            // 但 run_id/trace_id/module 通过便捷方法在 message body 中注入
            logger_->set_pattern("%v"); // 裸消息，JSON 在便捷方法中构建
            logger_->set_level(to_spdlog_level(config.level));

            // 注册为默认 logger
            spdlog::set_default_logger(logger_);

            run_id_.store(config.run_id, std::memory_order_relaxed);
            current_level_.store(config.level, std::memory_order_relaxed);

            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    void AsyncLogger::shutdown() noexcept
    {
        try
        {
            if (logger_)
            {
                logger_->flush();
            }
            spdlog::shutdown();
            logger_.reset();
        }
        catch (...)
        {
        }
    }

    std::shared_ptr<spdlog::logger> AsyncLogger::get_logger() noexcept
    {
        return logger_;
    }

    void AsyncLogger::set_thread_trace_id(uint64_t trace_id) noexcept
    {
        thread_trace_id_ = trace_id;
    }

    uint64_t AsyncLogger::get_thread_trace_id() noexcept
    {
        return thread_trace_id_;
    }

    void AsyncLogger::set_level(LogLevel level) noexcept
    {
        current_level_.store(level, std::memory_order_relaxed);
        if (logger_)
        {
            logger_->set_level(to_spdlog_level(level));
        }
    }

    LogLevel AsyncLogger::get_level() noexcept
    {
        return current_level_.load(std::memory_order_relaxed);
    }

    uint64_t AsyncLogger::get_run_id() noexcept
    {
        return run_id_.load(std::memory_order_relaxed);
    }

    void AsyncLogger::flush() noexcept
    {
        if (logger_)
        {
            logger_->flush();
        }
    }

    namespace
    {
        /// 构建 JSON 日志行
        /// 格式: {"ts":"<ISO8601>","level":"<LVL>","run_id":<N>,"trace_id":<N>,"module":"<M>","msg":"<MSG>"}
        std::string build_json_line(spdlog::level::level_enum level,
                                    std::string_view module,
                                    std::string_view msg,
                                    std::string_view detail_json = "")
        {
            // 获取当前时间戳
            auto now = std::chrono::system_clock::now();
            auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch())
                                .count();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
#ifdef _WIN32
            gmtime_s(&tm_buf, &time_t_now);
#else
            gmtime_r(&time_t_now, &tm_buf);
#endif
            char ts_buf[64];
            auto ms_part = epoch_ms % 1000;
            std::snprintf(ts_buf, sizeof(ts_buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                          tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                          tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                          static_cast<int>(ms_part));

            uint64_t run_id = AsyncLogger::get_run_id();
            uint64_t trace_id = AsyncLogger::get_thread_trace_id();

            // 预估大小，避免多次分配
            std::string json;
            json.reserve(256 + msg.size() + detail_json.size());
            json += "{\"ts\":\"";
            json += ts_buf;
            json += "\",\"level\":\"";
            json += level_to_json_string(level);
            json += "\",\"run_id\":";
            json += std::to_string(run_id);
            json += ",\"trace_id\":";
            json += std::to_string(trace_id);
            json += ",\"module\":\"";
            json += module;
            json += "\",\"msg\":\"";
            // 简单转义双引号
            for (char c : msg)
            {
                if (c == '"')
                    json += "\\\"";
                else if (c == '\\')
                    json += "\\\\";
                else if (c == '\n')
                    json += "\\n";
                else
                    json += c;
            }
            json += "\"";
            if (!detail_json.empty())
            {
                json += ",\"detail\":";
                json += detail_json; // 假设调用方已提供合法 JSON
            }
            json += "}";
            return json;
        }
    } // anonymous namespace

    void AsyncLogger::log_trace(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::trace))
        {
            logger_->trace(build_json_line(spdlog::level::trace, module, msg));
        }
    }

    void AsyncLogger::log_debug(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::debug))
        {
            logger_->debug(build_json_line(spdlog::level::debug, module, msg));
        }
    }

    void AsyncLogger::log_info(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::info))
        {
            logger_->info(build_json_line(spdlog::level::info, module, msg));
        }
    }

    void AsyncLogger::log_warn(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::warn))
        {
            logger_->warn(build_json_line(spdlog::level::warn, module, msg));
        }
    }

    void AsyncLogger::log_error(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::err))
        {
            logger_->error(build_json_line(spdlog::level::err, module, msg));
        }
    }

    void AsyncLogger::log_critical(std::string_view module, std::string_view msg) noexcept
    {
        if (logger_ && logger_->should_log(spdlog::level::critical))
        {
            logger_->critical(build_json_line(spdlog::level::critical, module, msg));
        }
    }

    void AsyncLogger::log_structured(LogLevel level,
                                     std::string_view module,
                                     std::string_view msg,
                                     std::string_view detail_json) noexcept
    {
        auto spdlvl = to_spdlog_level(level);
        if (logger_ && logger_->should_log(spdlvl))
        {
            logger_->log(spdlvl, build_json_line(spdlvl, module, msg, detail_json));
        }
    }

} // namespace qdgz300
