#include "qdgz300/control/orchestrator.h"

namespace qdgz300::control
{
    void Orchestrator::register_module(ModuleHandle handle)
    {
        modules_.push_back(std::move(handle));
    }

    bool Orchestrator::boot()
    {
        events_.publish({Event::Type::StateChange, "boot"});
        return fsm_.transition(SystemState::Standby, "boot completed");
    }

    bool Orchestrator::start()
    {
        for (auto &module : modules_)
        {
            if (module.start && !module.start())
            {
                return false;
            }
        }
        events_.publish({Event::Type::Command, "start"});
        return fsm_.transition(SystemState::Running, "start command");
    }

    void Orchestrator::stop()
    {
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it)
        {
            if (it->stop)
            {
                it->stop();
            }
        }
        events_.publish({Event::Type::Command, "stop"});
        (void)fsm_.transition(SystemState::Standby, "stop command");
    }

    bool Orchestrator::reset()
    {
        stop();
        return fsm_.transition(SystemState::Init, "reset command");
    }
}
