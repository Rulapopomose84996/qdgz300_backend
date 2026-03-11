#include "qdgz300/m01_receiver/monitoring/logger.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if defined(ERROR)
#undef ERROR
#endif

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace receiver
{
    namespace monitoring
    {
        namespace
        {
            spdlog::level::level_enum to_spdlog_level(LogLevel level)
            {
                switch (level)
                {
                case LogLevel::DEBUG:
                    return spdlog::level::debug;
                case LogLevel::INFO:
                    return spdlog::level::info;
                case LogLevel::WARN:
                    return spdlog::level::warn;
                case LogLevel::ERROR:
                    return spdlog::level::err;
                default:
                    return spdlog::level::info;
                }
            }

            const char *to_level_name(LogLevel level)
            {
                switch (level)
                {
                case LogLevel::DEBUG:
                    return "DEBUG";
                case LogLevel::INFO:
                    return "INFO";
                case LogLevel::WARN:
                    return "WARN";
                case LogLevel::ERROR:
                    return "ERROR";
                default:
                    return "INFO";
                }
            }

            std::string iso8601_now_utc()
            {
                const auto now = std::chrono::system_clock::now();
                const auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::tm tm_utc{};
                gmtime_r(&time_t_now, &tm_utc);
                std::ostringstream oss;
                oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
                return oss.str();
            }

            std::string escape_json(const std::string &input)
            {
                std::string out;
                out.reserve(input.size() + 8);
                for (char ch : input)
                {
                    switch (ch)
                    {
                    case '\\':
                        out += "\\\\";
                        break;
                    case '"':
                        out += "\\\"";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        out.push_back(ch);
                        break;
                    }
                }
                return out;
            }
        } // namespace

        class Logger::Impl
        {
        public:
            std::shared_ptr<spdlog::logger> logger;
            std::mutex mutex;
            std::atomic<LogLevel> current_level{LogLevel::INFO};
            std::atomic<bool> json_format{false};
        };

        Logger::TraceLogger::TraceLogger(Logger *owner, std::string trace_id)
            : owner_(owner), trace_id_(std::move(trace_id))
        {
        }

        Logger::Logger()
            : impl_(std::make_unique<Impl>())
        {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            impl_->logger = std::make_shared<spdlog::logger>("receiver_default", sink);
            impl_->logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
            impl_->logger->set_level(to_spdlog_level(impl_->current_level.load(std::memory_order_relaxed)));
        }

        Logger::~Logger()
        {
            flush();
        }

        Logger &Logger::instance()
        {
            static Logger logger;
            return logger;
        }

        void Logger::initialize(LogLevel level, const std::string &log_file)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!log_file.empty())
            {
                auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
                impl_->logger = std::make_shared<spdlog::logger>("receiver_file", sink);
                impl_->logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
            }
            else
            {
                auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                impl_->logger = std::make_shared<spdlog::logger>("receiver_console", sink);
                impl_->logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
            }

            impl_->current_level.store(level, std::memory_order_relaxed);
            impl_->logger->set_level(to_spdlog_level(level));
        }

        void Logger::set_level(LogLevel level)
        {
            impl_->current_level.store(level, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->logger)
            {
                impl_->logger->set_level(to_spdlog_level(level));
            }
        }

        void Logger::set_json_format(bool enabled)
        {
            impl_->json_format.store(enabled, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->logger)
            {
                impl_->logger->set_pattern(enabled ? "%v" : "%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
            }
        }

        bool Logger::is_enabled(LogLevel level) const
        {
            return level >= impl_->current_level.load(std::memory_order_relaxed);
        }

        Logger::TraceLogger Logger::with_trace_id(uint8_t source_id, uint16_t control_epoch, uint32_t seq_no)
        {
            std::ostringstream oss;
            oss << "0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(source_id)
                << std::dec << ":" << control_epoch << ":" << seq_no;
            return TraceLogger(this, oss.str());
        }

        void Logger::log(LogLevel level, const std::string &message)
        {
            log(level, message, nullptr);
        }

        void Logger::log(LogLevel level, const std::string &message, const char *trace_id)
        {
            if (!is_enabled(level))
            {
                return;
            }

            const bool json_enabled = impl_->json_format.load(std::memory_order_relaxed);
            std::string final_message;
            if (json_enabled)
            {
                std::ostringstream oss;
                oss << "{\"time\":\"" << iso8601_now_utc() << "\","
                    << "\"level\":\"" << to_level_name(level) << "\","
                    << "\"trace_id\":\"" << (trace_id != nullptr ? trace_id : "") << "\","
                    << "\"msg\":\"" << escape_json(message) << "\"}";
                final_message = oss.str();
            }
            else if (trace_id != nullptr && trace_id[0] != '\0')
            {
                final_message.reserve(message.size() + 32);
                final_message += "[trace_id=";
                final_message += trace_id;
                final_message += "] ";
                final_message += message;
            }
            else
            {
                final_message = message;
            }

            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->logger)
            {
                impl_->logger->log(to_spdlog_level(level), final_message);
            }
        }

        void Logger::flush()
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->logger)
            {
                impl_->logger->flush();
            }
        }

    } // namespace monitoring
} // namespace receiver
