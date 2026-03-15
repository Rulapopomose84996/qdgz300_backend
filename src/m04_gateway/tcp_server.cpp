#include "qdgz300/m04_gateway/tcp_server.h"

namespace qdgz300::m04
{
    CommandAck TcpServer::handle_command_for_test(const std::string &payload) const
    {
        (void)port_;
        (void)sessions_;

        if (!handler_)
        {
            return {false, 1u, "command handler not configured"};
        }
        return handler_(payload);
    }
}
