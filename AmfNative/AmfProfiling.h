#pragma once
#include "AmfNative.h"
#include <atomic>
#include <chrono>

inline constexpr const char* AmfProfileStageNames[] = {
    "slot_wait", "copy_resource_cpu", "submit_input", "input_retry_wait", "query_output",
    "output_poll_wait", "output_mux_wait", "bitstream_mux", "audio_write", "writer_sample",
    "finalize", "mp4_finalize", "slot_residence"
};
static_assert(sizeof(AmfProfileStageNames) / sizeof(AmfProfileStageNames[0]) == AmfProfileStageCount, "Profile stage names mismatch");

// Fixed-size, allocation-free CPU wall-time counters. No logging or GPU waits.
struct AmfProfiler
{
    struct Metric
    {
        std::atomic<uint64_t> count{ 0 }, total{ 0 }, maximum{ 0 };
        void Add(uint64_t ns)
        {
            total.fetch_add(ns, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            uint64_t current = maximum.load(std::memory_order_relaxed);
            while (ns > current && !maximum.compare_exchange_weak(current, ns, std::memory_order_relaxed)) {}
        }
    };
    std::atomic_bool enabled{ false };
    std::atomic<uint64_t> accepted{ 0 }, completed{ 0 }, retries{ 0 };
    Metric metrics[AmfProfileStageCount];

    static uint64_t Now()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    AmfProfileSnapshot Snapshot() const
    {
        AmfProfileSnapshot result{};
        result.version = 1;
        result.metricCount = AmfProfileStageCount;
        result.acceptedFrames = accepted.load(std::memory_order_relaxed);
        result.completedFrames = completed.load(std::memory_order_relaxed);
        result.inputRetries = retries.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < AmfProfileStageCount; ++i)
        {
            result.metrics[i] = { metrics[i].count.load(std::memory_order_relaxed),
                metrics[i].total.load(std::memory_order_relaxed), metrics[i].maximum.load(std::memory_order_relaxed) };
        }
        return result;
    }
};

class AmfProfileScope
{
    AmfProfiler::Metric* metric = nullptr;
    uint64_t start = 0;
public:
    AmfProfileScope(AmfProfiler& profiler, AmfProfileStage stage)
    {
        if (profiler.enabled.load(std::memory_order_relaxed))
        {
            metric = &profiler.metrics[stage];
            start = AmfProfiler::Now();
        }
    }
    AmfProfileScope(const AmfProfileScope&) = delete;
    AmfProfileScope& operator=(const AmfProfileScope&) = delete;
    void Stop()
    {
        if (metric)
        {
            metric->Add(AmfProfiler::Now() - start);
            metric = nullptr;
        }
    }
    ~AmfProfileScope() { Stop(); }
};

static_assert(sizeof(AmfProfileMetric) == 24, "Profiling metric ABI mismatch");
static_assert(sizeof(AmfProfileSnapshot) == 344, "Profiling snapshot ABI mismatch");
