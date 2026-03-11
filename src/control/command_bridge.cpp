#include "qdgz300/control/command_bridge.h"

namespace qdgz300::control
{
    CommandAck CommandBridge::handle(const std::string &command)
    {
        if (command == "START")
        {
            return orchestrator_.start()
                       ? CommandAck{true, ErrorCode::OK, "started"}
                       : CommandAck{false, ErrorCode::ORC_INVALID_TRANSITION, "start rejected"};
        }
        if (command == "STOP")
        {
            orchestrator_.stop();
            return {true, ErrorCode::OK, "stopped"};
        }
        if (command == "RESET")
        {
            return orchestrator_.reset()
                       ? CommandAck{true, ErrorCode::OK, "reset"}
                       : CommandAck{false, ErrorCode::ORC_INVALID_TRANSITION, "reset rejected"};
        }
        if (command == "QUERY_STATE")
        {
            return {true, ErrorCode::OK, "query accepted"};
        }

        return {false, ErrorCode::ORC_EVENT_THROTTLED, "unsupported command"};
    }
}
