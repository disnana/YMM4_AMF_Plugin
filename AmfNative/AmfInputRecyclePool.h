#pragma once
#include "AmfNative.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

// MF's tracked-sample pattern expressed independently of COM/AMF: an input
// lease and its encoded output have separate lifetimes. Tickets never alias
// a newer lease of the same texture. Both textures and pending outputs are bounded.
class AmfInputRecyclePool
{
public:
    struct Lease { uint64_t ticket = 0; uint32_t slot = 0; explicit operator bool() const { return ticket != 0; } };
private:
    struct Entry
    {
        Lease lease;
        bool inputReleased = false, outputComplete = false;
        uint64_t start = 0;
    };
    std::vector<bool> busy;
    std::vector<Entry> entries;
    mutable std::mutex mutex;
    std::condition_variable changed;
    AmfInputRecycleStatus status{};
    uint64_t nextTicket = 1;
    size_t nextSlot = 0;
    bool stopped = false;
    uint64_t inputExhaustions = 0, outputBudgetExhaustions = 0;

    static uint64_t Now()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    Entry* Find(uint64_t ticket)
    {
        if (ticket == 0) return nullptr; // Zero denotes an unused entry, never a live lease.
        for (auto& entry : entries) if (entry.lease.ticket == ticket) return &entry;
        return nullptr;
    }
    bool Ready() const
    {
        return status.pendingOutputs < status.maxPendingOutputs
            && std::find(busy.begin(), busy.end(), false) != busy.end();
    }
    Lease RentLocked()
    {
        if (stopped || !Ready()) return {};
        Entry* target = nullptr;
        for (auto& entry : entries) if (!entry.lease) { target = &entry; break; }
        if (!target) { ++status.invalidEvents; stopped = true; return {}; }
        for (size_t offset = 0; offset < busy.size(); ++offset)
        {
            const size_t index = (nextSlot + offset) % busy.size();
            if (busy[index]) continue;
            for (const auto& entry : entries)
            {
                if (entry.lease && entry.lease.slot == index && !entry.outputComplete)
                { ++status.reusesBeforeOutput; break; }
            }
            busy[index] = true;
            *target = { { nextTicket++, static_cast<uint32_t>(index) }, false, false, Now() };
            nextSlot = (index + 1) % busy.size();
            ++status.inputsInUse; ++status.pendingOutputs;
            status.peakInputsInUse = std::max(status.inputsInUse, status.peakInputsInUse);
            status.peakPendingOutputs = std::max(status.pendingOutputs, status.peakPendingOutputs);
            return target->lease;
        }
        return {};
    }
public:
    AmfInputRecyclePool(uint32_t slots, uint32_t maxPending)
        : busy(slots, false), entries(static_cast<size_t>(slots) + maxPending)
    {
        status.version = 1; status.requested = 1;
        status.poolSize = slots; status.maxPendingOutputs = maxPending;
    }
    Lease TryRent()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return RentLocked();
    }
    Lease Rent(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!stopped)
        {
            if (status.inputsInUse == status.poolSize) ++inputExhaustions;
            if (status.pendingOutputs == status.maxPendingOutputs) ++outputBudgetExhaustions;
        }
        if (!changed.wait_for(lock, timeout, [&]() { return stopped || Ready(); })) return {};
        return RentLocked();
    }
    bool ReleaseInput(uint64_t ticket)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto* entry = Find(ticket);
        if (!entry || entry->inputReleased)
        { ++status.invalidEvents; stopped = true; changed.notify_all(); return false; }
        entry->inputReleased = true;
        busy[entry->lease.slot] = false;
        --status.inputsInUse; ++status.inputReleases;
        if (!entry->outputComplete) ++status.inputReleasesBeforeOutput;
        const uint64_t duration = Now() - entry->start;
        ++status.inputResidence.count;
        status.inputResidence.totalNanoseconds += duration;
        status.inputResidence.maxNanoseconds = std::max(status.inputResidence.maxNanoseconds, duration);
        if (entry->outputComplete) *entry = {};
        changed.notify_all();
        return true;
    }
    bool CompleteOutput(uint64_t ticket, uint64_t& residenceNs)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto* entry = Find(ticket);
        if (!entry || entry->outputComplete)
        { ++status.invalidEvents; stopped = true; changed.notify_all(); return false; }
        residenceNs = Now() - entry->start;
        entry->outputComplete = true;
        --status.pendingOutputs; ++status.outputCompletions;
        if (entry->inputReleased) *entry = {};
        changed.notify_all();
        return true;
    }
    // Only used before a submission was accepted. With an observer attached,
    // its ReleaseInput callback owns the input half of the cancellation.
    void Cancel(uint64_t ticket, bool observerAttached)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto* entry = Find(ticket);
        if (!entry) return;
        if (!entry->outputComplete) { entry->outputComplete = true; --status.pendingOutputs; }
        if (!observerAttached && !entry->inputReleased)
        {
            busy[entry->lease.slot] = false; --status.inputsInUse; entry->inputReleased = true;
        }
        if (entry->inputReleased) *entry = {};
        changed.notify_all();
    }
    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true; changed.notify_all();
    }
    AmfInputRecycleStatus Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return status;
    }
    void FillPoolStatus(AmfTexturePoolStatus& result) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        result.inputsInUse = status.inputsInUse;
        result.peakInputsInUse = status.peakInputsInUse;
        result.pendingOutputLimit = status.maxPendingOutputs;
        result.inputExhaustionCount = inputExhaustions;
        result.outputBudgetExhaustionCount = outputBudgetExhaustions;
    }
};

static_assert(sizeof(AmfInputRecycleStatus) == 96, "Input recycle status ABI mismatch");
