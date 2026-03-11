#include "qdgz300/control/boot_checker.h"
#include "qdgz300/control/command_bridge.h"
#include "qdgz300/control/event_dispatcher.h"
#include "qdgz300/control/orchestrator.h"
#include "qdgz300/control/runtime_monitor.h"
#include "qdgz300/control/state_machine.h"

#include <gtest/gtest.h>

using namespace qdgz300;
using namespace qdgz300::control;

TEST(ControlStateMachineTest, FollowsExpectedTransitions)
{
    StateMachine fsm;
    EXPECT_TRUE(fsm.transition(SystemState::Standby, "boot"));
    EXPECT_TRUE(fsm.transition(SystemState::Running, "start"));
    EXPECT_TRUE(fsm.transition(SystemState::Degraded, "warn"));
    EXPECT_TRUE(fsm.transition(SystemState::Running, "recover"));
    EXPECT_TRUE(fsm.transition(SystemState::Fault, "fault"));
    EXPECT_TRUE(fsm.transition(SystemState::Init, "reset"));
}

TEST(ControlBootCheckerTest, AggregatesChecks)
{
    BootChecker checker;
    checker.add_check([] { return BootChecker::CheckResult{"config", true, "ok"}; });
    checker.add_check([] { return BootChecker::CheckResult{"network", true, "ok"}; });

    auto results = checker.run_all();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(checker.all_passed());
}

TEST(ControlEventDispatcherTest, DeliversPublishedEvents)
{
    EventDispatcher dispatcher;
    int count = 0;
    dispatcher.subscribe(Event::Type::Command, [&](const Event &event)
                         {
                             EXPECT_EQ(event.payload, "START");
                             ++count;
                         });

    dispatcher.publish({Event::Type::Command, "START"});
    EXPECT_EQ(count, 1);
}

TEST(ControlRuntimeMonitorTest, DrivesDegradedAndFaultTransitions)
{
    StateMachine fsm;
    ASSERT_TRUE(fsm.transition(SystemState::Standby, "boot"));
    ASSERT_TRUE(fsm.transition(SystemState::Running, "start"));

    RuntimeMonitor monitor(fsm);
    monitor.report_health(false, T1_DEGRADED_MS);
    monitor.tick();
    EXPECT_EQ(fsm.current(), SystemState::Degraded);

    monitor.report_health(false, T2_FAULT_MS);
    monitor.tick();
    EXPECT_EQ(fsm.current(), SystemState::Fault);
}

TEST(ControlCommandBridgeTest, InvokesOrchestratorStart)
{
    StateMachine fsm;
    EventDispatcher dispatcher;
    Orchestrator orchestrator(fsm, dispatcher);
    ASSERT_TRUE(orchestrator.boot());

    bool started = false;
    orchestrator.register_module({
        "m01",
        [&] {
            started = true;
            return true;
        },
        [] {},
        [] { return true; },
    });

    CommandBridge bridge(orchestrator);
    auto ack = bridge.handle("START");
    EXPECT_TRUE(ack.success);
    EXPECT_TRUE(started);
    EXPECT_EQ(fsm.current(), SystemState::Running);
}
