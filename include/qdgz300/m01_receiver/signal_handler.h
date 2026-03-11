#ifndef RECEIVER_SIGNAL_HANDLER_H
#define RECEIVER_SIGNAL_HANDLER_H

#include <atomic>
#include <cstdint>

namespace receiver
{
    extern std::atomic<bool> g_running;
    extern std::atomic<bool> g_reload_requested;
    extern std::atomic<uint64_t> g_sigint_received;
    extern std::atomic<uint64_t> g_sigterm_received;
    extern std::atomic<uint64_t> g_sighup_received;

    void reset_signal_flags();
    void install_signal_handlers();
} // namespace receiver

#endif // RECEIVER_SIGNAL_HANDLER_H
