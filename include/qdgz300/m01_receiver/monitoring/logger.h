#ifndef RECEIVER_MONITORING_LOGGER_H
#define RECEIVER_MONITORING_LOGGER_H

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(ERROR)
#undef ERROR
#endif

namespace receiver
{
    namespace monitoring
    {

        enum class LogLevel
        {
            DEBUG,
            INFO,
            WARN,
            ERROR
        };

        class Logger
        {
        public:
            class TraceLogger
            {
            public:
                TraceLogger(Logger *owner, std::string trace_id);

                template <typename... Args>
                void debug(const char *fmt, Args &&...args)
                {
                    if (owner_ != nullptr)
                    {
                        owner_->logf_with_trace(LogLevel::DEBUG, trace_id_.c_str(), fmt, std::forward<Args>(args)...);
                    }
                }

                template <typename... Args>
                void info(const char *fmt, Args &&...args)
                {
                    if (owner_ != nullptr)
                    {
                        owner_->logf_with_trace(LogLevel::INFO, trace_id_.c_str(), fmt, std::forward<Args>(args)...);
                    }
                }

                template <typename... Args>
                void warn(const char *fmt, Args &&...args)
                {
                    if (owner_ != nullptr)
                    {
                        owner_->logf_with_trace(LogLevel::WARN, trace_id_.c_str(), fmt, std::forward<Args>(args)...);
                    }
                }

                template <typename... Args>
                void error(const char *fmt, Args &&...args)
                {
                    if (owner_ != nullptr)
                    {
                        owner_->logf_with_trace(LogLevel::ERROR, trace_id_.c_str(), fmt, std::forward<Args>(args)...);
                    }
                }

            private:
                Logger *owner_{nullptr};
                std::string trace_id_;
            };

            static Logger &instance();

            void initialize(LogLevel level, const std::string &log_file = "");
            void set_level(LogLevel level);
            void set_json_format(bool enabled);
            bool is_enabled(LogLevel level) const;
            TraceLogger with_trace_id(uint8_t source_id, uint16_t control_epoch, uint32_t seq_no);

            template <typename... Args>
            void debug(const char *fmt, Args &&...args)
            {
                logf(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
            }
            void debug(const char *message)
            {
                log(LogLevel::DEBUG, message == nullptr ? std::string{} : std::string(message));
            }

            template <typename... Args>
            void info(const char *fmt, Args &&...args)
            {
                logf(LogLevel::INFO, fmt, std::forward<Args>(args)...);
            }
            void info(const char *message)
            {
                log(LogLevel::INFO, message == nullptr ? std::string{} : std::string(message));
            }

            template <typename... Args>
            void warn(const char *fmt, Args &&...args)
            {
                logf(LogLevel::WARN, fmt, std::forward<Args>(args)...);
            }
            void warn(const char *message)
            {
                log(LogLevel::WARN, message == nullptr ? std::string{} : std::string(message));
            }

            template <typename... Args>
            void error(const char *fmt, Args &&...args)
            {
                logf(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
            }
            void error(const char *message)
            {
                log(LogLevel::ERROR, message == nullptr ? std::string{} : std::string(message));
            }

            void flush();

        private:
            Logger();
            ~Logger();

            Logger(const Logger &) = delete;
            Logger &operator=(const Logger &) = delete;

            template <typename... Args>
            void logf(LogLevel level, const char *fmt, Args &&...args)
            {
                int size = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
                if (size <= 0)
                {
                    return;
                }

                std::vector<char> buffer(static_cast<size_t>(size) + 1);
                std::snprintf(buffer.data(), buffer.size(), fmt, std::forward<Args>(args)...);
                log(level, std::string(buffer.data(), static_cast<size_t>(size)));
            }

            template <typename... Args>
            void logf_with_trace(LogLevel level, const char *trace_id, const char *fmt, Args &&...args)
            {
                int size = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
                if (size <= 0)
                {
                    return;
                }

                std::vector<char> buffer(static_cast<size_t>(size) + 1);
                std::snprintf(buffer.data(), buffer.size(), fmt, std::forward<Args>(args)...);
                log(level, std::string(buffer.data(), static_cast<size_t>(size)), trace_id);
            }

            void log(LogLevel level, const std::string &message);
            void log(LogLevel level, const std::string &message, const char *trace_id);

            class Impl;
            std::unique_ptr<Impl> impl_;
        };

#define LOG_DEBUG(...) \
    receiver::monitoring::Logger::instance().debug(__VA_ARGS__)

#define LOG_INFO(...) \
    receiver::monitoring::Logger::instance().info(__VA_ARGS__)

#define LOG_WARN(...) \
    receiver::monitoring::Logger::instance().warn(__VA_ARGS__)

#define LOG_ERROR(...) \
    receiver::monitoring::Logger::instance().error(__VA_ARGS__)

    } // namespace monitoring
} // namespace receiver

#endif // RECEIVER_MONITORING_LOGGER_H
