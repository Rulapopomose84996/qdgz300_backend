#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace qdgz300::m04
{
    struct HmiSession
    {
        uint64_t session_id{0};
        std::string peer_name{};
        uint64_t last_pong_ns{0};
        bool alive{true};
    };

    class HmiSessionManager
    {
    public:
        void upsert(uint64_t session_id, std::string peer_name, uint64_t now_ns);
        void mark_pong(uint64_t session_id, uint64_t now_ns);
        void sweep_timeouts(uint64_t now_ns, uint64_t timeout_ns);
        size_t alive_count() const;

    private:
        std::unordered_map<uint64_t, HmiSession> sessions_;
    };
}
