#include "qdgz300/control/state_machine.h"

namespace qdgz300::control
{
    bool StateMachine::transition(SystemState target, const std::string &reason)
    {
        if (!qdgz300::is_transition_allowed(current_, target))
        {
            return false;
        }

        const SystemState from = current_;
        current_ = target;
        for (const auto &callback : callbacks_)
        {
            callback(from, target, reason);
        }
        return true;
    }

    void StateMachine::on_transition(Callback callback)
    {
        callbacks_.push_back(std::move(callback));
    }
}
