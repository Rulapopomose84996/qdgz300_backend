#include "qdgz300/m01_receiver/signal_handler.h"

#include <csignal>

namespace receiver
{
    std::atomic<bool> g_running{true};
    std::atomic<bool> g_reload_requested{false};
    std::atomic<uint64_t> g_sigint_received{0};
    std::atomic<uint64_t> g_sigterm_received{0};
    std::atomic<uint64_t> g_sighup_received{0};

    namespace
    {
        void signal_handler_impl(int signal)
        {
            if (signal == SIGINT || signal == SIGTERM)
            {
                if (signal == SIGINT)
                {
                    g_sigint_received.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    g_sigterm_received.fetch_add(1, std::memory_order_relaxed);
                }
                g_running.store(false, std::memory_order_release); // publish stop flag to loop readers
                return;
            }
#if defined(SIGHUP)
            if (signal == SIGHUP)
            {
                g_sighup_received.fetch_add(1, std::memory_order_relaxed);
                g_reload_requested.store(true, std::memory_order_release); // publish reload request to loop reader
            }
#endif
        }
    } // namespace

    void reset_signal_flags()
    {
        g_running.store(true, std::memory_order_release); // reset flag before run loop observes it
        g_reload_requested.store(false, std::memory_order_release); // reset flag before run loop observes it
        g_sigint_received.store(0, std::memory_order_relaxed);
        g_sigterm_received.store(0, std::memory_order_relaxed);
        g_sighup_received.store(0, std::memory_order_relaxed);
    }

    void install_signal_handlers()
    {
        std::signal(SIGINT, signal_handler_impl);
        std::signal(SIGTERM, signal_handler_impl);
#if defined(SIGHUP)
        std::signal(SIGHUP, signal_handler_impl);
#endif
    }
} // namespace receiver
