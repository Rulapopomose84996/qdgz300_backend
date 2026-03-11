#pragma once

#include "qdgz300/control/event_dispatcher.h"
#include "qdgz300/control/state_machine.h"

#include <functional>
#include <string>
#include <vector>

namespace qdgz300::control
{
    class Orchestrator
    {
    public:
        struct ModuleHandle
        {
            std::string name{};
            std::function<bool()> start{};
            std::function<void()> stop{};
            std::function<bool()> is_healthy{};
        };

        Orchestrator(StateMachine &fsm, EventDispatcher &events) : fsm_(fsm), events_(events) {}

        void register_module(ModuleHandle handle);
        bool boot();
        bool start();
        void stop();
        bool reset();

        size_t module_count() const noexcept { return modules_.size(); }

    private:
        StateMachine &fsm_;
        EventDispatcher &events_;
        std::vector<ModuleHandle> modules_;
    };
}
