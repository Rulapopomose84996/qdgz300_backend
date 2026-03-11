#pragma once

#include "qdgz300/common/constants.h"
#include "qdgz300/common/system_state.h"
#include "qdgz300/control/state_machine.h"

namespace qdgz300::control
{
    class RuntimeMonitor
    {
    public:
        explicit RuntimeMonitor(StateMachine &fsm) : fsm_(fsm) {}

        void report_health(bool healthy, uint32_t unhealthy_ms);
        void tick();

    private:
        StateMachine &fsm_;
        bool healthy_{true};
        uint32_t unhealthy_ms_{0};
    };
}
