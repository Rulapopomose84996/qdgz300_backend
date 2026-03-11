#pragma once

#include "qdgz300/common/error_codes.h"
#include "qdgz300/control/orchestrator.h"

#include <string>

namespace qdgz300::control
{
    struct CommandAck
    {
        bool success{false};
        ErrorCode error_code{ErrorCode::OK};
        std::string message{};
    };

    class CommandBridge
    {
    public:
        explicit CommandBridge(Orchestrator &orchestrator) : orchestrator_(orchestrator) {}

        CommandAck handle(const std::string &command);

    private:
        Orchestrator &orchestrator_;
    };
}
