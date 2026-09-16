/**
 * @file  probe_event_bus.cpp
 * @brief `EventBus` publish / subscribe / dispatch.
 *
 * @warning **This row is dirty on purpose and must stay visible.**
 *          `EventChannel<T>::Subscribe` calls `DynamicArray::PushBack`, which is
 *          the throwing overload, and `EventBus::Assure<T>` calls it twice more.
 *          None of this produced a C4530 - it is exactly the class of violation
 *          the warning cannot see. ECS 2-1 rewrites `Subscribe` (it has to grow
 *          an unsubscribe anyway); **change this line to CLEAN then**, and if the
 *          rewrite forgets, this row fails loudly instead of passing quietly.
 */
// EXPECT: THROW
// WHY: known N-2 violation in Subscribe / Assure (DynamicArray::PushBack). Fixed in ECS 2-1.

#include "Events/EventBus.h"
#include "Events/Events.h"

namespace { struct Ping { int value; }; }

void UseBus(GLFD::Events::EventBus& bus) {
  bus.Register<Ping>();
  bus.Subscribe<Ping>([](const Ping&) {});
  bus.Publish(Ping{ 1 });
  bus.DispatchAll();
  (void)bus.Counters();
  bus.ResetCounters();
}
