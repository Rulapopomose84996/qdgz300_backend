#pragma once

#include "qdgz300/m04_gateway/hmi_session.h"

#include <cstdint>
#include <functional>
#include <string>

namespace qdgz300::m04
{
    struct CommandAck
    {
        bool success{false};
        uint32_t error_code{0};
        std::string error_message{};
    };

    class TcpServer
    {
    public:
        using CommandHandler = std::function<CommandAck(const std::string &)>;

        explicit TcpServer(uint16_t port, HmiSessionManager &sessions) : port_(port), sessions_(sessions) {}

        void start() { running_ = true; }
        void stop() { running_ = false; }
        bool running() const { return running_; }
        void set_command_handler(CommandHandler handler) { handler_ = std::move(handler); }
        CommandAck handle_command_for_test(const std::string &payload) const;

    private:
        [[maybe_unused]] uint16_t port_{0};
        [[maybe_unused]] HmiSessionManager &sessions_;
        CommandHandler handler_{};
        bool running_{false};
    };
}
