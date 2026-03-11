#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qdgz300::control
{
    struct Event
    {
        enum class Type
        {
            Command,
            StateChange,
            ModuleHealth,
            MetricAlert,
            ConfigUpdate,
        };

        Type type{Type::Command};
        std::string payload{};
    };

    class EventDispatcher
    {
    public:
        using Handler = std::function<void(const Event &)>;

        void subscribe(Event::Type type, Handler handler);
        void publish(const Event &event) const;

    private:
        std::unordered_map<Event::Type, std::vector<Handler>> handlers_;
    };
}
