#pragma once

#include "qdgz300/common/error_codes.h"
#include "qdgz300/common/system_state.h"

#include <functional>
#include <string>
#include <vector>

namespace qdgz300::control
{
    class StateMachine
    {
    public:
        using Callback = std::function<void(SystemState from, SystemState to, const std::string &reason)>;

        bool transition(SystemState target, const std::string &reason);
        SystemState current() const noexcept { return current_; }
        void on_transition(Callback callback);

    private:
        SystemState current_{SystemState::Init};
        std::vector<Callback> callbacks_;
    };
}
