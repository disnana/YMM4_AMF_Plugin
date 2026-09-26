#pragma once

#include "AmfNative.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Wait for observable encoder progress after AMF_INPUT_FULL. A generation is
// captured before SubmitInput, so an encoded-output notification racing the
// failed call cannot be lost. The finite fallback keeps driver anomalies from
// turning this experiment into an unbounded wait.
class AmfInputWaitSignal
{
public:
    enum class Result { Progress, Timeout, Stopped };
    static constexpr uint32_t FallbackTimeoutMs = 10;

private:
    const bool requested;
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t generation = 0;
    bool stopped = false;
    uint64_t waits = 0, signaledWaits = 0, timedOutWaits = 0;
    uint64_t inputReleaseNotifications = 0, outputNotifications = 0;
    AmfProfileMetric waitMetric{};

    static uint64_t Nanoseconds(std::chrono::steady_clock::duration duration)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

    void Notify(bool inputRelease)
    {
        if (!requested) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (inputRelease)
            {
                // Surface release is useful diagnostic evidence, but it does
                // not prove that the AMF component input queue has capacity.
                ++inputReleaseNotifications;
                return;
            }
            ++outputNotifications;
            ++generation;
        }
        changed.notify_all();
    }

public:
    explicit AmfInputWaitSignal(bool enabled) : requested(enabled) {}

    uint64_t Observe() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return generation;
    }

    Result WaitForProgress(uint64_t observed, std::chrono::steady_clock::time_point overallDeadline)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto fallback = start + std::chrono::milliseconds(FallbackTimeoutMs);
        const auto deadline = std::min(overallDeadline, fallback);
        std::unique_lock<std::mutex> lock(mutex);
        ++waits;
        const bool woke = changed.wait_until(lock, deadline, [&]() { return stopped || generation != observed; });
        const uint64_t duration = Nanoseconds(std::chrono::steady_clock::now() - start);
        ++waitMetric.count;
        waitMetric.totalNanoseconds += duration;
        waitMetric.maxNanoseconds = std::max(waitMetric.maxNanoseconds, duration);
        if (stopped) return Result::Stopped;
        if (woke) { ++signaledWaits; return Result::Progress; }
        ++timedOutWaits;
        return Result::Timeout;
    }

    void NotifyInputRelease() { Notify(true); }
    void NotifyOutput() { Notify(false); }

    void Stop()
    {
        if (!requested) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            ++generation;
        }
        changed.notify_all();
    }

    AmfInputWaitStatus Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        AmfInputWaitStatus result{};
        result.version = 1;
        result.requested = requested ? 1u : 0u;
        result.mode = requested ? AmfInputWaitProgressNotification : AmfInputWaitLegacySleep;
        result.fallbackTimeoutMs = requested ? FallbackTimeoutMs : 0;
        result.waits = waits;
        result.signaledWaits = signaledWaits;
        result.timedOutWaits = timedOutWaits;
        result.inputReleaseNotifications = inputReleaseNotifications;
        result.outputNotifications = outputNotifications;
        result.wait = waitMetric;
        return result;
    }
};

static_assert(sizeof(AmfInputWaitStatus) == 80, "Input-wait status ABI mismatch");
