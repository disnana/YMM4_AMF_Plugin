#pragma once

#include <stdint.h>

struct ID3D11Device;
struct ID3D11Texture2D;

// Append-only profiling ABI; existing encoder entry points are unchanged.
enum AmfProfileStage : uint32_t
{
    SlotWait, CopyResourceCpu, SubmitInput, InputRetryWait, QueryOutput,
    OutputPollWait, OutputMuxWait, BitstreamMux, AudioWrite, WriterSample,
    Finalize, Mp4Finalize, SlotResidence, AmfProfileStageCount
};

struct AmfProfileMetric
{
    uint64_t count;
    uint64_t totalNanoseconds;
    uint64_t maxNanoseconds;
};

struct AmfProfileSnapshot
{
    uint32_t version;
    uint32_t metricCount;
    uint64_t acceptedFrames;
    uint64_t completedFrames;
    uint64_t inputRetries;
    AmfProfileMetric metrics[AmfProfileStageCount];
};

extern "C" {
    __declspec(dllexport) void* AmfCreate(
        ID3D11Device* device,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int codec,
        int quality,
        int rateControlMode,
        int maxBitrateKbps,
        int surfaceFormat,
        int texturePoolSize,
        int enableDebugLog,
        const wchar_t* outputPath);

    __declspec(dllexport) int AmfEncode(void* handle, ID3D11Texture2D* texture);

    __declspec(dllexport) int AmfWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels);

    __declspec(dllexport) int AmfFinalize(void* handle);

    __declspec(dllexport) void AmfDestroy(void* handle);

    __declspec(dllexport) const wchar_t* AmfGetLastError(void* handle);

    __declspec(dllexport) int AmfEnableProfiling(void* handle);
    __declspec(dllexport) int AmfGetProfile(void* handle, AmfProfileSnapshot* result, uint32_t resultSize);
}
