#include "qdgz300/m04_gateway/hmi_session.h"

namespace qdgz300::m04
{
    void HmiSessionManager::upsert(uint64_t session_id, std::string peer_name, uint64_t now_ns)
    {
        auto &session = sessions_[session_id];
        session.session_id = session_id;
        session.peer_name = std::move(peer_name);
        session.last_pong_ns = now_ns;
        session.alive = true;
    }

    void HmiSessionManager::mark_pong(uint64_t session_id, uint64_t now_ns)
    {
        auto it = sessions_.find(session_id);
        if (it == sessions_.end())
        {
            return;
        }
        it->second.last_pong_ns = now_ns;
        it->second.alive = true;
    }

    void HmiSessionManager::sweep_timeouts(uint64_t now_ns, uint64_t timeout_ns)
    {
        uint64_t newest_seen = 0;
        for (const auto &[_, session] : sessions_)
        {
            if (session.last_pong_ns > newest_seen)
            {
                newest_seen = session.last_pong_ns;
            }
        }

        const uint64_t expire_before = (newest_seen > timeout_ns) ? (newest_seen - timeout_ns) : 0;
        for (auto &[_, session] : sessions_)
        {
            if (session.last_pong_ns < expire_before && session.last_pong_ns + timeout_ns < now_ns)
            {
                session.alive = false;
            }
        }
    }

    size_t HmiSessionManager::alive_count() const
    {
        size_t count = 0;
        for (const auto &[_, session] : sessions_)
        {
            if (session.alive)
            {
                ++count;
            }
        }
        return count;
    }
}
