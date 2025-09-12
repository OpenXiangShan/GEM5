#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

// Define a simple cycle type for time representation
using SimpleCycle = uint64_t;

// Represents a scheduled event
class Event
{
public:
    using Functor = std::function<void()>;

    Event(Functor f, SimpleCycle t) : func(f), time(t) {}

    void process() { if (func) func(); }
    SimpleCycle when() const { return time; }

    // Comparison operator for priority queue
    bool operator>(const Event& other) const {
        return time > other.time;
    }

private:
    Functor func;
    SimpleCycle time;
};

// A simple event queue for unit testing purposes
class SimpleEventQueue
{
public:
    SimpleEventQueue() : currentTime(0) {}

    // Schedule a new event
    void schedule(Event::Functor func, SimpleCycle scheduled_time) {
        event_queue.emplace(func, scheduled_time);
    }

    // Advance time and process any events that are due
    void advanceTo(SimpleCycle new_time) {
        while (!event_queue.empty() && event_queue.top().when() <= new_time) {
            Event event = event_queue.top();
            event_queue.pop();
            // Update current time to the event's time before processing
            if (event.when() > currentTime) {
                currentTime = event.when();
            }
            event.process();
        }
        currentTime = new_time;
    }

    // Get the current time
    SimpleCycle curCycle() const {
        return currentTime;
    }

    uint64_t size() const {
        return event_queue.size();
    }

private:
    SimpleCycle currentTime;
    // Priority queue to keep events sorted by time
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> event_queue;
};

} // namespace test
} // namespace prefetch
} // namespace gem5
