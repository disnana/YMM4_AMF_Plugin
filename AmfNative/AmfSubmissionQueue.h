#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>

// Guarded by the owner's slot mutex. AMFSurface properties are not thread-safe:
// an output callback alone must not permit reuse while SubmitInput is returning.
struct AmfSubmissionReturnGate
{
    bool submitReturned = false, outputReceived = false;
    bool MarkSubmitReturned() { submitReturned = true; return outputReceived; }
    bool MarkOutputReceived() { outputReceived = true; return submitReturned; }
};

// Owns prepared inputs, never a borrowed/reusable host bitmap. Close drains FIFO;
// Abort wakes both sides and leaves pending items for explicit lease cancellation.
template<class T> class AmfSubmissionQueue
{
    const size_t capacity;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<T> items;
    bool closed = false, aborted = false;
    size_t peak = 0;
public:
    explicit AmfSubmissionQueue(size_t limit) : capacity(limit)
    { if (!limit) throw std::invalid_argument("Submission queue capacity must be positive"); }
    bool Push(T& item, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, timeout, [&] { return closed || aborted || items.size() < capacity; })
            || closed || aborted) return false;
        items.push_back(std::move(item));
        peak = std::max(peak, items.size());
        changed.notify_all();
        return true;
    }
    bool Pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return closed || aborted || !items.empty(); });
        if (aborted || items.empty()) return false;
        item = std::move(items.front()); items.pop_front();
        changed.notify_all();
        return true;
    }
    void Close() { std::lock_guard<std::mutex> lock(mutex); closed = true; changed.notify_all(); }
    void Abort() { std::lock_guard<std::mutex> lock(mutex); aborted = true; changed.notify_all(); }
    std::deque<T> TakeAborted()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::deque<T> result;
        if (aborted) result.swap(items);
        return result;
    }
    void Snapshot(uint32_t& depth, uint32_t& maximum) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        depth = static_cast<uint32_t>(items.size()); maximum = static_cast<uint32_t>(peak);
    }
};
