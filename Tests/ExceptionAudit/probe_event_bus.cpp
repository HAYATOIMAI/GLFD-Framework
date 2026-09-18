/**
 * @file  probe_event_bus.cpp
 * @brief `EventBus` publish / subscribe / unsubscribe / dispatch.
 *
 * @details
 *  **This row was THROW until ECS 2-1, and the reason is worth keeping.**
 *  `EventChannel<T>::Subscribe` stored a `std::function` in a `DynamicArray`
 *  through the throwing `PushBack`, and `EventBus::Assure<T>` called it twice
 *  more and never checked what `Allocate` returned. None of it produced a
 *  C4530 - it is exactly the class of violation the warning cannot see.
 *
 *  2-1 replaced the callback with a plain function pointer plus a `void*`
 *  context and moved every container call to `Try*`. The callback type is the
 *  reason the row can be CLEAN at all: `std::function` allocates whenever the
 *  capture outgrows MSVC's inline buffer, so with it here, the row's colour
 *  would depend on how much each caller happened to capture.
 */
// EXPECT: CLEAN
// WHY: subscribe / unsubscribe / publish all run on the per-frame path (N-2).

#include "Events/EventBus.h"
#include "Events/Events.h"

namespace { struct Ping { int value; }; }

static void OnPing(void* context, const Ping& ping) {
  *static_cast<int*>(context) += ping.value;
}

void UseBus(GLFD::Events::EventBus& bus, int& sink) {
  (void)bus.Register<Ping>();
  const GLFD::Events::SubscriptionId id = bus.Subscribe<Ping>(&sink, &OnPing);
  bus.Publish(Ping{ 1 });
  bus.DispatchAll();
  (void)bus.Counters();
  bus.ResetCounters();
  (void)bus.SubscriberCount();
  (void)bus.SubscriberCountSince(bus.NextSerial());
  (void)bus.Unsubscribe(id);
  (void)bus.UnsubscribeSince(1u);
}
