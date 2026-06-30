#include "infrastructure/eventbus.h"

EventBus::EventBus(QObject* parent)
    : QObject(parent)
{
}

EventBus* EventBus::instance()
{
    static EventBus s_instance;
    return &s_instance;
}
