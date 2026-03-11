#include "qdgz300/control/runtime_monitor.h"

namespace qdgz300::control
{
    void RuntimeMonitor::report_health(bool healthy, uint32_t unhealthy_ms)
    {
        healthy_ = healthy;
        unhealthy_ms_ = unhealthy_ms;
    }

    void RuntimeMonitor::tick()
    {
        if (healthy_)
        {
            if (fsm_.current() == SystemState::Degraded)
            {
                (void)fsm_.transition(SystemState::Running, "runtime recovered");
            }
            return;
        }

        if (unhealthy_ms_ >= qdgz300::T2_FAULT_MS)
        {
            (void)fsm_.transition(SystemState::Fault, "runtime unhealthy beyond fault threshold");
            return;
        }

        if (unhealthy_ms_ >= qdgz300::T1_DEGRADED_MS && fsm_.current() == SystemState::Running)
        {
            (void)fsm_.transition(SystemState::Degraded, "runtime unhealthy beyond degraded threshold");
        }
    }
}
