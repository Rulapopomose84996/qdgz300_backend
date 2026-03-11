#include "qdgz300/control/event_dispatcher.h"

namespace qdgz300::control
{
    void EventDispatcher::subscribe(Event::Type type, Handler handler)
    {
        handlers_[type].push_back(std::move(handler));
    }

    void EventDispatcher::publish(const Event &event) const
    {
        const auto it = handlers_.find(event.type);
        if (it == handlers_.end())
        {
            return;
        }
        for (const auto &handler : it->second)
        {
            handler(event);
        }
    }
}
